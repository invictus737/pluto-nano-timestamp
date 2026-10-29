/*
 * EHCI host controller driver for CherryUSB, NXP MCUXpresso pipe model (CHERRY_HCD=nxp).
 *
 * Derived from NXP host/usb_host_ehci.c of mcux-sdk-middleware-usb
 * (https://github.com/nxp-mcuxpresso/mcux-sdk-middleware-usb, commit b330ba1e98be), the host
 * controller driver NXP ships for the same ChipIdea (ARC/TDI) EHCI core as the Zynq-7000 USB0
 * (i.MX RT USBHS; also used by Zephyr drivers/usb/uhc/uhc_mcux_ehci.c). Taken from it:
 *   - USB_HostEhciStartAsync/StopAsync, StartPeriodic/StopPeriodic: the schedule enable
 *     handshake (wait until USBSTS.AS/PS equals USBCMD.ASE/PSE, change the enable bit, wait
 *     for the status; ASYNCLISTADDR rewritten to the head before ASE goes on);
 *   - USB_HostEhciOpenControlBulk/CloseControlBulk/QhInit/QhDeinit: one QH per pipe, linked
 *     right after the async head at open and unlinked under a stopped asynchronous schedule;
 *   - USB_HostEhciOpenInterrupt/CloseInterrupt/AddQhToFrame/RemoveFromFrame: interrupt QHs
 *     on the frame list every interval, ordered by decreasing interval;
 *   - USB_HostEhciTransferQtdListDeinit/QhQtdListDeinit/CancelPipe: cancelling a transfer by
 *     stopping the schedule and editing the QH (no Interrupt on Async Advance);
 *   - USB_HostEhciTransactionDone: in-order completion per pipe from the tail qTD, error
 *     status cleared in the overlay and the queue restarted behind the failed transfer, data
 *     toggle reset on a successful CLEAR_FEATURE(ENDPOINT_HALT);
 *   - USB_HostEhciIoctl kUSB_HostUpdateControlPacketSize: EP0 characteristics rewritten in
 *     place with the asynchronous schedule stopped;
 *   - USB_HostEhciResetIP/StartIP and the PORTSC write-1-to-clear masking.
 * Changed for CherryUSB and this target: qTDs are appended behind a dummy qTD (Linux
 * ehci-q.c qh_append_tds) instead of NXP's walk-and-link, so appending never races the
 * controller; pipes are keyed by (hport, endpoint) and opened by the first URB; CherryUSB
 * URBs, synchronous timeouts and completions; bounded handshakes; XACTERR in-place restart
 * (Linux QH_XACTERR_MAX); the roothub request handling of CherryUSB's EHCI port.
 *
 * NXP license (BSD-3-Clause) of the original file:
 *
 * Copyright (c) 2015 - 2016, Freescale Semiconductor, Inc.
 * Copyright 2016,2019 - 2020 NXP
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are
 * permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice, this list of
 *    conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of
 *    conditions and the following disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The CherryUSB parts (roothub requests, URB API) are Copyright (c) 2022, sakumisu,
 * Apache-2.0.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <string.h>

#include "usb_hc_ehci.h"

#define EHCI_TUNE_CERR    3 /* qTD error counter */
#define EHCI_TUNE_RL_HS   4 /* NAK reload, high-speed control */
#define EHCI_TUNE_MULT_HS 1
#define EHCI_TUNE_MULT_TT 1
#ifndef CONFIG_USB_EHCI_BULK_RL_HS
#define CONFIG_USB_EHCI_BULK_RL_HS 0
#endif
#ifndef CONFIG_USB_EHCI_XACTERR_RETRIES
#define CONFIG_USB_EHCI_XACTERR_RETRIES 32
#endif

#ifdef CONFIG_USB_EHCI_DMA_WMB
#define ehci_dma_wmb() CONFIG_USB_EHCI_DMA_WMB()
#else
#define ehci_dma_wmb() __asm__ volatile("" ::: "memory")
#endif

#define QTD_PTR(q)   ((uint32_t)usb_ramaddr2phyaddr((uintptr_t)&(q)->hw) & ~0x1FU)
#define QTD_PTR_T(q) ((q) ? QTD_PTR(q) : QTD_LIST_END)
#define QH_LINK(qh)  (((uint32_t)usb_ramaddr2phyaddr((uintptr_t)&(qh)->hw) & ~0x1FU) | 0x2U)
#define LINK_ADDR(x) ((uint32_t)(x) & ~0x1FU)

struct ehci_nxp_hcd g_ehci_nxp[CONFIG_USBHOST_MAX_BUS];
struct usbh_ehci_iaa_stats g_ehci_iaa_stats[CONFIG_USBHOST_MAX_BUS];
volatile uint32_t g_ehci_xacterr_retries;

USB_NOCACHE_RAM_SECTION struct ehci_nxp_qh g_ehci_nxp_qh[CONFIG_USBHOST_MAX_BUS][CONFIG_USB_EHCI_QH_NUM];
USB_NOCACHE_RAM_SECTION struct ehci_nxp_qtd g_ehci_nxp_qtd[CONFIG_USBHOST_MAX_BUS][CONFIG_USB_EHCI_QTD_NUM];
USB_NOCACHE_RAM_SECTION struct ehci_nxp_qh g_ehci_nxp_async_head[CONFIG_USBHOST_MAX_BUS];
USB_NOCACHE_RAM_SECTION uint32_t g_framelist[CONFIG_USBHOST_MAX_BUS][CONFIG_USB_EHCI_FRAME_LIST_SIZE]
    __attribute__((aligned(4096)));

/* ---- registers ---- */

static inline uintptr_t ehci_cap_base(struct usbh_bus *bus)
{
    return (uintptr_t)bus->hcd.reg_base + CONFIG_USB_EHCI_HCCR_OFFSET;
}

static inline uint32_t cap_rd(struct usbh_bus *bus, uint32_t off)
{
    return EHCI_NXP_REG_READ(ehci_cap_base(bus) + off);
}

static inline uint32_t op_rd(struct usbh_bus *bus, uint32_t off)
{
    return EHCI_NXP_REG_READ(ehci_cap_base(bus) + g_ehci_nxp[bus->hcd.hcd_id].hcor_offset + off);
}

static inline void op_wr(struct usbh_bus *bus, uint32_t off, uint32_t val)
{
    EHCI_NXP_REG_WRITE(ehci_cap_base(bus) + g_ehci_nxp[bus->hcd.hcd_id].hcor_offset + off, val);
}

/* USBCMD read-modify-write that never writes the IAA doorbell back */
static inline void usbcmd_update(struct usbh_bus *bus, uint32_t clr, uint32_t set)
{
    uint32_t v = op_rd(bus, EHCI_NXP_USBCMD);

    op_wr(bus, EHCI_NXP_USBCMD, ((v & ~(clr | EHCI_USBCMD_IAAD)) | set));
}

/* PORTSC read-modify-write that never clears a change bit by accident */
static inline void portsc_update(struct usbh_bus *bus, uint8_t port, uint32_t clr, uint32_t set)
{
    uint32_t v = op_rd(bus, EHCI_NXP_PORTSC(port));

    op_wr(bus, EHCI_NXP_PORTSC(port), ((v & ~(clr | EHCI_NXP_PORTSC_W1C)) | set));
}

static inline struct ehci_nxp_hcd *hcd_of(struct usbh_bus *bus)
{
    return &g_ehci_nxp[bus->hcd.hcd_id];
}

/* ---- schedule handshakes (NXP USB_HostEhciStartAsync/StopAsync/StartPeriodic/StopPeriodic) ---- */

static bool ehci_hc_halted(struct usbh_bus *bus)
{
    return (op_rd(bus, EHCI_NXP_USBSTS) & EHCI_USBSTS_HALTED) != 0;
}

static void note_handshake(struct usbh_bus *bus, uint32_t t0, bool ok)
{
    struct ehci_nxp_hcd *h = hcd_of(bus);
    uint32_t us = usb_ehci_time_us() - t0;

    if (us > h->stats.max_handshake_us) {
        h->stats.max_handshake_us = us;
    }
    if (!ok) {
        h->stats.handshake_timeouts++;
        g_ehci_iaa_stats[bus->hcd.hcd_id].async_kick_no_handshake++;
    }
}

/* wait until (USBSTS & sts_bit) != 0 equals (USBCMD & cmd_bit) != 0 */
static bool ehci_wait_sync(struct usbh_bus *bus, uint32_t sts_bit, uint32_t cmd_bit, uint32_t t0)
{
    for (;;) {
        bool s = (op_rd(bus, EHCI_NXP_USBSTS) & sts_bit) != 0;
        bool c = (op_rd(bus, EHCI_NXP_USBCMD) & cmd_bit) != 0;

        if (s == c) {
            return true;
        }
        if ((uint32_t)(usb_ehci_time_us() - t0) > CONFIG_USB_EHCI_NXP_HANDSHAKE_US) {
            return false;
        }
    }
}

static bool ehci_wait_sts(struct usbh_bus *bus, uint32_t sts_bit, bool on, uint32_t t0)
{
    for (;;) {
        if (((op_rd(bus, EHCI_NXP_USBSTS) & sts_bit) != 0) == on) {
            return true;
        }
        if ((uint32_t)(usb_ehci_time_us() - t0) > CONFIG_USB_EHCI_NXP_HANDSHAKE_US) {
            return false;
        }
    }
}

static void ehci_stop_async(struct usbh_bus *bus)
{
    uint32_t t0 = usb_ehci_time_us();
    bool ok;

    hcd_of(bus)->stats.async_stops++;
    if (ehci_hc_halted(bus)) {
        usbcmd_update(bus, EHCI_USBCMD_ASEN, 0);
        return;
    }
    ok = ehci_wait_sync(bus, EHCI_USBSTS_ASS, EHCI_USBCMD_ASEN, t0);
    usbcmd_update(bus, EHCI_USBCMD_ASEN, 0);
    ok = ehci_wait_sts(bus, EHCI_USBSTS_ASS, false, t0) && ok;
    note_handshake(bus, t0, ok);
}

static void ehci_start_async(struct usbh_bus *bus)
{
    uint32_t t0 = usb_ehci_time_us();
    bool ok = true;

    if (op_rd(bus, EHCI_NXP_USBSTS) & EHCI_USBSTS_ASS) {
        return;
    }
    if (!ehci_hc_halted(bus)) {
        ok = ehci_wait_sync(bus, EHCI_USBSTS_ASS, EHCI_USBCMD_ASEN, t0);
    }
    op_wr(bus, EHCI_NXP_ASYNCLIST, LINK_ADDR(QH_LINK(hcd_of(bus)->async_head)));
    ehci_dma_wmb();
    usbcmd_update(bus, 0, EHCI_USBCMD_ASEN);
    if (!ehci_hc_halted(bus)) {
        ok = ehci_wait_sts(bus, EHCI_USBSTS_ASS, true, t0) && ok;
        note_handshake(bus, t0, ok);
    }
}

static void ehci_stop_periodic(struct usbh_bus *bus)
{
    uint32_t t0 = usb_ehci_time_us();
    bool ok;

    hcd_of(bus)->stats.periodic_stops++;
    if (ehci_hc_halted(bus)) {
        usbcmd_update(bus, EHCI_USBCMD_PSEN, 0);
        return;
    }
    ok = ehci_wait_sync(bus, EHCI_USBSTS_PSS, EHCI_USBCMD_PSEN, t0);
    usbcmd_update(bus, EHCI_USBCMD_PSEN, 0);
    ok = ehci_wait_sts(bus, EHCI_USBSTS_PSS, false, t0) && ok;
    note_handshake(bus, t0, ok);
}

static void ehci_start_periodic(struct usbh_bus *bus)
{
    uint32_t t0 = usb_ehci_time_us();
    bool ok = true;

    if (op_rd(bus, EHCI_NXP_USBSTS) & EHCI_USBSTS_PSS) {
        return;
    }
    if (!ehci_hc_halted(bus)) {
        ok = ehci_wait_sync(bus, EHCI_USBSTS_PSS, EHCI_USBCMD_PSEN, t0);
    }
    op_wr(bus, EHCI_NXP_PERIODICLIST, (uint32_t)usb_ramaddr2phyaddr((uintptr_t)hcd_of(bus)->framelist));
    ehci_dma_wmb();
    usbcmd_update(bus, 0, EHCI_USBCMD_PSEN);
    if (!ehci_hc_halted(bus)) {
        ok = ehci_wait_sts(bus, EHCI_USBSTS_PSS, true, t0) && ok;
        note_handshake(bus, t0, ok);
    }
}

static void ehci_stop_schedule(struct usbh_bus *bus, struct ehci_nxp_pipe *p)
{
    if (p->periodic) {
        ehci_stop_periodic(bus);
    } else {
        ehci_stop_async(bus);
    }
}

static void ehci_start_schedule(struct usbh_bus *bus, struct ehci_nxp_pipe *p)
{
    ehci_dma_wmb();
    if (p->periodic) {
        ehci_start_periodic(bus);
    } else {
        ehci_start_async(bus);
    }
}

/* ---- qTD and transfer pools ---- */

static struct ehci_nxp_qtd *qtd_alloc(struct ehci_nxp_hcd *h)
{
    struct ehci_nxp_qtd *q = h->qtd_free;

    if (q == NULL) {
        return NULL;
    }
    h->qtd_free = q->sw_next;
    h->qtd_free_count--;
    q->sw_next = NULL;
    q->inuse = 1;
    q->len = 0;
    q->stage = EHCI_NXP_STAGE_DATA;
    q->hw.next_qtd = QTD_LIST_END;
    q->hw.alt_next_qtd = QTD_LIST_END;
    q->hw.token = QTD_TOKEN_STATUS_HALTED; /* inactive (Linux dummy) */
    for (int i = 0; i < 5; i++) {
        q->hw.bpl[i] = 0;
    }
    return q;
}

static void qtd_free(struct ehci_nxp_hcd *h, struct ehci_nxp_qtd *q)
{
    q->inuse = 0;
    q->hw.token = QTD_TOKEN_STATUS_HALTED;
    q->sw_next = h->qtd_free;
    h->qtd_free = q;
    h->qtd_free_count++;
}

static struct ehci_nxp_qtd *qtd_from_ptr(struct ehci_nxp_hcd *h, uint32_t ptr)
{
    uintptr_t a, base = (uintptr_t)h->qtd_pool;

    if (ptr & QTD_LIST_END) {
        return NULL;
    }
    a = usb_phyaddr2ramaddr((uintptr_t)LINK_ADDR(ptr));
    if (a < base || a >= base + sizeof(struct ehci_nxp_qtd) * CONFIG_USB_EHCI_QTD_NUM ||
        (a - base) % sizeof(struct ehci_nxp_qtd)) {
        return NULL;
    }
    return (struct ehci_nxp_qtd *)a;
}

/* the qTDs of a transfer, first .. last, through the hardware links */
static struct ehci_nxp_qtd *xfer_qtd_next(struct ehci_nxp_hcd *h, struct ehci_nxp_xfer *x, struct ehci_nxp_qtd *q)
{
    if (q == x->last) {
        return NULL;
    }
    return qtd_from_ptr(h, q->hw.next_qtd);
}

static bool xfer_owns_ptr(struct ehci_nxp_hcd *h, struct ehci_nxp_xfer *x, uint32_t ptr)
{
    for (struct ehci_nxp_qtd *q = x->first; q; q = xfer_qtd_next(h, x, q)) {
        if (LINK_ADDR(ptr) == QTD_PTR(q)) {
            return true;
        }
    }
    return false;
}

static void xfer_free_qtds(struct ehci_nxp_hcd *h, struct ehci_nxp_xfer *x)
{
    struct ehci_nxp_qtd *q = x->first, *n;

    while (q) {
        n = xfer_qtd_next(h, x, q);
        qtd_free(h, q);
        q = n;
    }
    x->first = x->last = NULL;
}

static struct ehci_nxp_xfer *xfer_alloc(struct ehci_nxp_hcd *h)
{
    for (uint32_t i = 0; i < CONFIG_USB_EHCI_NXP_XFER_NUM; i++) {
        struct ehci_nxp_xfer *x = &h->xfers[i];

        if (!x->inuse) {
            usb_osal_sem_t sem = x->sem;

            memset(x, 0, sizeof(*x));
            x->sem = sem;
            x->inuse = 1;
            return x;
        }
    }
    h->stats.no_xfer++;
    return NULL;
}

static void xfer_release(struct ehci_nxp_xfer *x)
{
    if (x->urb && x->urb->hcpriv == x) {
        x->urb->hcpriv = NULL;
    }
    x->urb = NULL;
    x->pipe = NULL;
    x->inuse = 0;
}

static struct ehci_nxp_xfer *xfer_of_urb(struct ehci_nxp_hcd *h, struct usbh_urb *urb)
{
    uintptr_t p = (uintptr_t)urb->hcpriv, base = (uintptr_t)&h->xfers[0];

    if (p < base || p >= base + sizeof(h->xfers) || (p - base) % sizeof(struct ehci_nxp_xfer)) {
        return NULL;
    }
    if (!((struct ehci_nxp_xfer *)p)->inuse || ((struct ehci_nxp_xfer *)p)->urb != urb) {
        return NULL;
    }
    return (struct ehci_nxp_xfer *)p;
}

/* ---- endpoint characteristics (NXP USB_HostEhciQhInit; values of CherryUSB's port) ---- */

static void ehci_tt_of(struct usbh_hubport *hport, uint8_t *hubaddr, uint8_t *hubport)
{
    struct usbh_hub *hub = hport->parent;
    uint8_t port = hport->port;

    /* the transaction translator is in the nearest high-speed hub upstream */
    while (hub && !hub->is_roothub && hub->parent && hub->parent->speed != USB_SPEED_HIGH) {
        port = hub->parent->port;
        hub = hub->parent->parent;
    }
    *hubaddr = hub ? hub->hub_addr : 0;
    *hubport = port;
}

static void ehci_ep_params(struct usbh_urb *urb, uint32_t *epchar, uint32_t *epcap,
                           uint16_t *period, uint16_t *uframe_interval)
{
    struct usbh_hubport *hport = urb->hport;
    uint8_t type = USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes);
    uint16_t mps = USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize);
    uint8_t mult = USB_GET_MULT(urb->ep->wMaxPacketSize) + 1;
    uint8_t binterval = urb->ep->bInterval;
    uint32_t c = 0, k = 0;
    uint8_t hubaddr, hubport;

    c |= (uint32_t)hport->dev_addr << QH_EPCHAR_DEVADDR_SHIFT;
    c |= (uint32_t)(urb->ep->bEndpointAddress & 0xf) << QH_EPCHAR_ENDPT_SHIFT;
    c |= (uint32_t)mps << QH_EPCHAR_MAXPKT_SHIFT;
    if (type == USB_ENDPOINT_TYPE_CONTROL) {
        c |= QH_EPCHAR_DTC; /* toggle from the qTDs */
    }
    *period = 1;
    *uframe_interval = 0;
    if (hport->speed == USB_SPEED_HIGH) {
        c |= QH_EPCHAR_EPS_HIGH;
        if (type == USB_ENDPOINT_TYPE_CONTROL) {
            c |= (uint32_t)EHCI_TUNE_RL_HS << QH_EPCHAR_RL_SHIFT;
            k |= QH_EPCAPS_MULT(EHCI_TUNE_MULT_HS);
        } else if (type == USB_ENDPOINT_TYPE_BULK) {
            c |= (uint32_t)CONFIG_USB_EHCI_BULK_RL_HS << QH_EPCHAR_RL_SHIFT;
            k |= QH_EPCAPS_MULT(EHCI_TUNE_MULT_HS);
        } else {
            /* interrupt: RL 0 (NXP), 2^(bInterval-1) microframes */
            uint32_t uf = 1U << ((binterval ? (binterval > 16 ? 16 : binterval) : 1) - 1);

            k |= QH_EPCAPS_MULT(mult > 3 ? 3 : mult);
            if (uf >= 8) {
                uint32_t frames = uf / 8;

                if (frames > CONFIG_USB_EHCI_FRAME_LIST_SIZE) {
                    frames = CONFIG_USB_EHCI_FRAME_LIST_SIZE;
                }
                *period = (uint16_t)frames;
                k |= QH_EPCAPS_SSMASK(0x01);
            } else {
                k |= QH_EPCAPS_SSMASK(uf == 1 ? 0xff : (uf == 2 ? 0x55 : 0x11));
            }
            *uframe_interval = (uint16_t)(uf > 0xffff ? 0xffff : uf);
        }
    } else {
        if (hport->speed == USB_SPEED_LOW) {
            c |= QH_EPCHAR_EPS_LOW;
        }
        if (type == USB_ENDPOINT_TYPE_CONTROL) {
            c |= QH_EPCHAR_C;
        }
        ehci_tt_of(hport, &hubaddr, &hubport);
        k |= QH_EPCAPS_MULT(EHCI_TUNE_MULT_TT) | QH_EPCAPS_HUBADDR(hubaddr) | QH_EPCAPS_PORT(hubport);
        if (type == USB_ENDPOINT_TYPE_INTERRUPT) {
            /* frames, rounded down to a power of two (NXP USB_HostEhciGet2PowerValue) */
            uint32_t frames = binterval ? binterval : 1, p2 = 1;

            while ((p2 << 1) <= frames && (p2 << 1) <= CONFIG_USB_EHCI_FRAME_LIST_SIZE) {
                p2 <<= 1;
            }
            *period = (uint16_t)p2;
            *uframe_interval = (uint16_t)(p2 * 8 > 0xffff ? 0xffff : p2 * 8);
            k |= QH_EPCAPS_SSMASK(2) | QH_EPCAPS_SCMASK(0x78);
        }
    }
    *epchar = c;
    *epcap = k;
}

/* ---- frame list (NXP USB_HostEhciAddQhToFrame/RemoveFromFrame) ---- */

static struct ehci_nxp_qh *qh_from_link(struct ehci_nxp_hcd *h, uint32_t link)
{
    uintptr_t a, base = (uintptr_t)h->qh_pool;

    if ((link & 1U) || (link & 0x6U) != 0x2U) {
        return NULL;
    }
    a = usb_phyaddr2ramaddr((uintptr_t)LINK_ADDR(link));
    if (a < base || a >= base + sizeof(struct ehci_nxp_qh) * CONFIG_USB_EHCI_QH_NUM ||
        (a - base) % sizeof(struct ehci_nxp_qh)) {
        return NULL;
    }
    return (struct ehci_nxp_qh *)a;
}

static void frame_add_qh(struct ehci_nxp_hcd *h, struct ehci_nxp_pipe *p, uint32_t frame)
{
    volatile uint32_t *entry = &h->framelist[frame];

    for (;;) {
        uint32_t v = *entry;
        struct ehci_nxp_qh *other;

        if (v & QTD_LIST_END) {
            p->qh->hw.hlp = QH_HLP_END; /* insert at the end */
            ehci_dma_wmb();
            *entry = QH_LINK(p->qh);
            return;
        }
        if (LINK_ADDR(v) == LINK_ADDR(QH_LINK(p->qh))) {
            return; /* already there (shared tail) */
        }
        other = qh_from_link(h, v);
        if (other && other->pipe && other->pipe->uframe_interval <= p->uframe_interval) {
            p->qh->hw.hlp = v; /* insert before the first QH with a shorter or equal interval */
            ehci_dma_wmb();
            *entry = QH_LINK(p->qh);
            return;
        }
        if (other == NULL) {
            return; /* not a QH of ours: leave the list alone */
        }
        entry = &other->hw.hlp;
    }
}

static void frame_remove_qh(struct ehci_nxp_hcd *h, struct ehci_nxp_pipe *p, uint32_t frame)
{
    volatile uint32_t *entry = &h->framelist[frame];

    for (;;) {
        uint32_t v = *entry;
        struct ehci_nxp_qh *other;

        if (v & QTD_LIST_END) {
            return;
        }
        if (LINK_ADDR(v) == LINK_ADDR(QH_LINK(p->qh))) {
            *entry = p->qh->hw.hlp;
            return;
        }
        other = qh_from_link(h, v);
        if (other == NULL) {
            return;
        }
        entry = &other->hw.hlp;
    }
}

/* ---- pipes ---- */

struct ehci_nxp_pipe *ehci_nxp_pipe_find(struct usbh_bus *bus, struct usbh_hubport *hport, uint8_t ep_addr, uint8_t type)
{
    struct ehci_nxp_hcd *h = hcd_of(bus);

    if (type == USB_ENDPOINT_TYPE_CONTROL) {
        ep_addr = 0;
    }
    for (uint32_t i = 0; i < CONFIG_USB_EHCI_QH_NUM; i++) {
        struct ehci_nxp_pipe *p = &h->pipes[i];

        if (p->inuse && !p->closing && p->hport == hport && p->ep_addr == ep_addr && p->type == type) {
            return p;
        }
    }
    return NULL;
}

/* NXP USB_HostEhciOpenControlBulk / USB_HostEhciOpenInterrupt */
static struct ehci_nxp_pipe *ehci_pipe_open(struct usbh_bus *bus, struct usbh_urb *urb)
{
    struct ehci_nxp_hcd *h = hcd_of(bus);
    struct ehci_nxp_pipe *p = NULL;
    struct ehci_nxp_qh *qh;
    uint32_t i, gen;

    for (i = 0; i < CONFIG_USB_EHCI_QH_NUM; i++) {
        if (!h->pipes[i].inuse) {
            p = &h->pipes[i];
            break;
        }
    }
    if (p == NULL) {
        h->stats.no_pipe++;
        return NULL;
    }
    gen = p->gen + 1;
    memset(p, 0, sizeof(*p));
    p->gen = gen;
    p->qh = qh = &h->qh_pool[i];
    p->dummy = qtd_alloc(h);
    if (p->dummy == NULL) {
        h->stats.no_qtd++;
        return NULL;
    }
    p->inuse = 1;
    p->hport = urb->hport;
    p->type = USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes);
    p->ep_addr = p->type == USB_ENDPOINT_TYPE_CONTROL ? 0 : urb->ep->bEndpointAddress;
    p->periodic = p->type == USB_ENDPOINT_TYPE_INTERRUPT;
    ehci_ep_params(urb, &p->epchar, &p->epcap, &p->period, &p->uframe_interval);

    memset(&qh->hw, 0, sizeof(qh->hw));
    qh->pipe = p;
    qh->hw.hlp = QH_HLP_END;
    qh->hw.epchar = p->epchar;
    qh->hw.epcap = p->epcap;
    qh->hw.curr_qtd = 0;
    qh->hw.overlay.next_qtd = QTD_PTR(p->dummy);
    qh->hw.overlay.alt_next_qtd = QTD_LIST_END;
    qh->hw.overlay.token = 0; /* inactive, DATA0 */
    ehci_dma_wmb();

    if (p->periodic) {
        for (uint32_t f = 0; f < CONFIG_USB_EHCI_FRAME_LIST_SIZE; f += p->period) {
            frame_add_qh(h, p, f);
        }
    } else {
        /* right behind the head; linking needs no stop (NXP) */
        qh->hw.hlp = h->async_head->hw.hlp;
        ehci_dma_wmb();
        h->async_head->hw.hlp = QH_LINK(qh);
    }
    ehci_dma_wmb();
    p->linked = 1;
    h->stats.pipes_opened++;
    return p;
}

/* the QH out of the schedule, the schedule stopped around the edit (NXP CloseControlBulk/CloseInterrupt) */
static void ehci_pipe_unlink(struct usbh_bus *bus, struct ehci_nxp_pipe *p)
{
    struct ehci_nxp_hcd *h = hcd_of(bus);

    if (!p->linked) {
        return;
    }
    ehci_stop_schedule(bus, p);
    if (p->periodic) {
        for (uint32_t f = 0; f < CONFIG_USB_EHCI_FRAME_LIST_SIZE; f += p->period) {
            frame_remove_qh(h, p, f);
        }
    } else {
        struct ehci_nxp_qh *prev = h->async_head;
        uint32_t guard = 0;

        while (guard++ <= CONFIG_USB_EHCI_QH_NUM) {
            if (LINK_ADDR(prev->hw.hlp) == LINK_ADDR(QH_LINK(p->qh))) {
                prev->hw.hlp = p->qh->hw.hlp;
                break;
            }
            if (LINK_ADDR(prev->hw.hlp) == LINK_ADDR(QH_LINK(h->async_head))) {
                break;
            }
            prev = qh_from_link(h, prev->hw.hlp);
            if (prev == NULL) {
                break;
            }
        }
    }
    p->qh->hw.hlp = QH_HLP_END;
    p->linked = 0;
    ehci_start_schedule(bus, p);
}

static void ehci_xfer_giveback(struct usbh_bus *bus, struct ehci_nxp_xfer *x, int result, uint32_t actual);

/*
 * Close a pipe: unlinked under a stopped schedule, then every transfer on it handed back with
 * `result` (NXP USB_HostEhciClosePipe + QhQtdListDeinit). Critical section held.
 */
static void ehci_pipe_close(struct usbh_bus *bus, struct ehci_nxp_pipe *p, int result)
{
    struct ehci_nxp_hcd *h = hcd_of(bus);
    struct ehci_nxp_xfer *x, *list;

    if (!p->inuse || p->closing) {
        return;
    }
    p->closing = 1;
    ehci_pipe_unlink(bus, p);
    list = p->head;
    p->head = p->tail = NULL;
    for (x = list; x; x = x->next) {
        xfer_free_qtds(h, x);
    }
    if (p->dummy) {
        qtd_free(h, p->dummy);
        p->dummy = NULL;
    }
    h->stats.pipes_closed++;
    while (list) {
        x = list;
        list = x->next;
        x->next = NULL;
        ehci_xfer_giveback(bus, x, result, x->urb ? x->urb->actual_length : 0);
    }
    p->qh->pipe = NULL;
    p->inuse = 0;
    p->closing = 0;
}

void ehci_nxp_close_hport(struct usbh_bus *bus, struct usbh_hubport *hport)
{
    struct ehci_nxp_hcd *h = hcd_of(bus);

    for (uint32_t i = 0; i < CONFIG_USB_EHCI_QH_NUM; i++) {
        if (h->pipes[i].inuse && h->pipes[i].hport == hport) {
            ehci_pipe_close(bus, &h->pipes[i], -USB_ERR_SHUTDOWN);
        }
    }
}

/* data toggle of an idle bulk/interrupt pipe (overlay inactive, no qTD active behind it) */
static void ehci_pipe_set_toggle(struct ehci_nxp_pipe *p, bool dt)
{
    uint32_t tok = p->qh->hw.overlay.token;

    if (p->type == USB_ENDPOINT_TYPE_CONTROL || p->head != NULL || (tok & QTD_TOKEN_STATUS_ACTIVE)) {
        return;
    }
    if (((tok & QTD_TOKEN_TOGGLE) != 0) != dt) {
        p->qh->hw.overlay.token = (tok & ~QTD_TOKEN_TOGGLE) | (dt ? QTD_TOKEN_TOGGLE : 0);
    }
}

/* ---- transfers ---- */

static void qtd_fill_buf(struct ehci_nxp_qtd *q, uintptr_t buf, uint32_t len)
{
    uint32_t addr = (uint32_t)usb_ramaddr2phyaddr(buf);

    /* NXP: page pointers set whatever the length */
    q->hw.bpl[0] = addr;
    for (int i = 1; i < 5; i++) {
        q->hw.bpl[i] = (addr + (uint32_t)i * 0x1000U) & 0xFFFFF000U;
    }
    q->len = len;
}

/*
 * Append a transfer behind the pipe's dummy (Linux qh_append_tds): the dummy becomes the
 * transfer's first qTD, new qTDs and a new dummy follow, the first token goes active last.
 */
static int ehci_xfer_queue(struct usbh_bus *bus, struct ehci_nxp_pipe *p, struct ehci_nxp_xfer *x)
{
    struct ehci_nxp_hcd *h = hcd_of(bus);
    struct usbh_urb *urb = x->urb;
    struct ehci_nxp_qtd *q[3 + 64], *ndummy;
    uint32_t tok[3 + 64];
    uint32_t nq, i, len = urb->transfer_buffer_length;
    bool in = (urb->ep->bEndpointAddress & 0x80) != 0;
    uintptr_t buf = (uintptr_t)urb->transfer_buffer;

    if (p->type == USB_ENDPOINT_TYPE_CONTROL) {
        nq = len ? 3 : 2;
    } else {
        nq = (len + 0x3FFFU) / 0x4000U;
        if (nq == 0) {
            nq = 1;
        }
        if (nq > 64) {
            return -USB_ERR_INVAL;
        }
    }
    if (h->qtd_free_count < nq) {
        h->stats.no_qtd++;
        return -USB_ERR_NOMEM;
    }
    q[0] = p->dummy;
    for (i = 1; i < nq; i++) {
        q[i] = qtd_alloc(h);
    }
    ndummy = qtd_alloc(h);

#ifdef CONFIG_USB_DCACHE_ENABLE
    if (urb->setup && p->type == USB_ENDPOINT_TYPE_CONTROL) {
        usb_dcache_flush((uintptr_t)urb->setup, USB_ALIGN_UP(8, CONFIG_USB_ALIGN_SIZE));
    }
    if (len && buf) {
        usb_dcache_flush(buf, USB_ALIGN_UP(len, CONFIG_USB_ALIGN_SIZE));
    }
#endif

    if (p->type == USB_ENDPOINT_TYPE_CONTROL) {
        bool data_in = (urb->setup->bmRequestType & USB_REQUEST_DIR_MASK) == USB_REQUEST_DIR_IN;
        uint32_t k = 0;

        /* NXP QhQtdListInit: setup DATA0, data DATA1, status DATA1 with IOC */
        q[k]->stage = EHCI_NXP_STAGE_SETUP;
        qtd_fill_buf(q[k], (uintptr_t)urb->setup, 8);
        tok[k] = QTD_TOKEN_PID_SETUP | ((uint32_t)EHCI_TUNE_CERR << QTD_TOKEN_CERR_SHIFT) |
                 (8U << QTD_TOKEN_NBYTES_SHIFT) | QTD_TOKEN_STATUS_ACTIVE;
        k++;
        if (len) {
            q[k]->stage = EHCI_NXP_STAGE_DATA;
            qtd_fill_buf(q[k], buf, len);
            tok[k] = (data_in ? QTD_TOKEN_PID_IN : QTD_TOKEN_PID_OUT) | QTD_TOKEN_TOGGLE |
                     ((uint32_t)EHCI_TUNE_CERR << QTD_TOKEN_CERR_SHIFT) |
                     (len << QTD_TOKEN_NBYTES_SHIFT) | QTD_TOKEN_STATUS_ACTIVE;
            k++;
        }
        q[k]->stage = EHCI_NXP_STAGE_STATUS;
        qtd_fill_buf(q[k], 0, 0);
        tok[k] = ((len == 0 || !data_in) ? QTD_TOKEN_PID_IN : QTD_TOKEN_PID_OUT) | QTD_TOKEN_TOGGLE |
                 ((uint32_t)EHCI_TUNE_CERR << QTD_TOKEN_CERR_SHIFT) | QTD_TOKEN_IOC | QTD_TOKEN_STATUS_ACTIVE;
        for (i = 0; i < nq; i++) {
            q[i]->hw.next_qtd = i + 1 < nq ? QTD_PTR(q[i + 1]) : QTD_PTR(ndummy);
            /* a short data stage goes on with the status stage (alternate pointer unused) */
            q[i]->hw.alt_next_qtd = QTD_LIST_END;
        }
    } else {
        for (i = 0; i < nq; i++) {
            uint32_t l = len > 0x4000U ? 0x4000U : len;

            q[i]->stage = EHCI_NXP_STAGE_DATA;
            qtd_fill_buf(q[i], buf, l);
            tok[i] = (in ? QTD_TOKEN_PID_IN : QTD_TOKEN_PID_OUT) |
                     ((uint32_t)EHCI_TUNE_CERR << QTD_TOKEN_CERR_SHIFT) |
                     (l << QTD_TOKEN_NBYTES_SHIFT) | QTD_TOKEN_STATUS_ACTIVE;
            if (i + 1 == nq) {
                tok[i] |= QTD_TOKEN_IOC;
            }
            q[i]->hw.next_qtd = i + 1 < nq ? QTD_PTR(q[i + 1]) : QTD_PTR(ndummy);
            /* a short IN packet ends the URB: the controller goes on with the next one */
            q[i]->hw.alt_next_qtd = (in && i + 1 < nq) ? QTD_PTR(ndummy) : QTD_LIST_END;
            buf += l;
            len -= l;
        }
    }
    ndummy->hw.next_qtd = QTD_LIST_END;
    ndummy->hw.alt_next_qtd = QTD_LIST_END;
    ndummy->hw.token = QTD_TOKEN_STATUS_HALTED;

    x->first = q[0];
    x->last = q[nq - 1];
    x->nqtd = (uint8_t)nq;
    x->pipe = p;
    x->next = NULL;

    /* the toggle the URB carries, while the pipe is idle (old port: urb->data_toggle) */
    if (p->head == NULL) {
        if (p->reset_toggle) {
            p->reset_toggle = 0;
            ehci_pipe_set_toggle(p, false);
        } else {
            ehci_pipe_set_toggle(p, urb->data_toggle != 0);
        }
    }

    for (i = 1; i < nq; i++) {
        q[i]->hw.token = tok[i];
    }
    ehci_dma_wmb();
    q[0]->hw.token = tok[0];
    p->dummy = ndummy;
    ehci_dma_wmb();

    if (p->tail) {
        p->tail->next = x;
    } else {
        p->head = x;
    }
    p->tail = x;
    return 0;
}

static int ehci_qtd_result(uint32_t token)
{
    if (token & QTD_TOKEN_STATUS_BABBLE) {
        return -USB_ERR_BABBLE;
    }
    if (token & (QTD_TOKEN_STATUS_DBERR | QTD_TOKEN_STATUS_XACTERR)) {
        return -USB_ERR_IO;
    }
    if (token & QTD_TOKEN_STATUS_HALTED) {
        return -USB_ERR_STALL;
    }
    return 0;
}

enum { XFER_RUNNING, XFER_DONE, XFER_HALTED };

/* state of a transfer from its qTDs (NXP TransactionDone: done when the IOC qTD is inactive) */
static int ehci_xfer_state(struct ehci_nxp_hcd *h, struct ehci_nxp_xfer *x, uint32_t *actual,
                           struct ehci_nxp_qtd **halted_qtd)
{
    bool in = (x->urb->ep->bEndpointAddress & 0x80) != 0;
    uint32_t sum = 0;

    *halted_qtd = NULL;
    for (struct ehci_nxp_qtd *q = x->first; q; q = xfer_qtd_next(h, x, q)) {
        uint32_t tok = q->hw.token;
        uint32_t left = (tok & QTD_TOKEN_NBYTES_MASK) >> QTD_TOKEN_NBYTES_SHIFT;

        if (tok & QTD_TOKEN_STATUS_ACTIVE) {
            *actual = sum;
            return XFER_RUNNING;
        }
        if (q->stage == EHCI_NXP_STAGE_DATA && q->len >= left) {
            sum += q->len - left;
        }
        if (tok & QTD_TOKEN_STATUS_HALTED) {
            *actual = sum;
            *halted_qtd = q;
            return XFER_HALTED;
        }
        if (x->pipe->type == USB_ENDPOINT_TYPE_CONTROL) {
            if (q->stage == EHCI_NXP_STAGE_DATA && left && in) {
                /* short control data: the status stage follows by the next pointer */
            }
        } else if (in && left) {
            *actual = sum; /* short packet: the controller went on by the alternate pointer */
            return XFER_DONE;
        }
        if (q == x->last) {
            break;
        }
    }
    *actual = sum;
    return XFER_DONE;
}

/* NXP TransactionDone: CLEAR_FEATURE(ENDPOINT_HALT) done -> that pipe starts again with DATA0 */
static void ehci_note_clear_halt(struct usbh_bus *bus, struct ehci_nxp_pipe *ctrl, struct usbh_urb *urb)
{
    struct usb_setup_packet *s = urb->setup;
    struct ehci_nxp_pipe *p;
    uint8_t ep;

    if (!s || s->bRequest != USB_REQUEST_CLEAR_FEATURE ||
        s->bmRequestType != (USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_ENDPOINT) ||
        (s->wValue & 0xff) != USB_FEATURE_ENDPOINT_HALT) {
        return;
    }
    ep = (uint8_t)s->wIndex;
    p = ehci_nxp_pipe_find(bus, ctrl->hport, ep, USB_ENDPOINT_TYPE_BULK);
    if (p == NULL) {
        p = ehci_nxp_pipe_find(bus, ctrl->hport, ep, USB_ENDPOINT_TYPE_INTERRUPT);
    }
    if (p == NULL) {
        return;
    }
    if (p->head == NULL) {
        ehci_pipe_set_toggle(p, false);
    } else {
        p->reset_toggle = 1;
    }
}

/* hand a transfer back (critical section held; its qTDs are already off the pipe) */
static void ehci_xfer_giveback(struct usbh_bus *bus, struct ehci_nxp_xfer *x, int result, uint32_t actual)
{
    struct ehci_nxp_hcd *h = hcd_of(bus);
    struct usbh_urb *urb = x->urb;
    struct ehci_nxp_pipe *p = x->pipe;

    if (urb == NULL) {
        xfer_release(x);
        return;
    }
    urb->actual_length = actual;
    urb->errorcode = result;
    if (p && p->qh && p->type != USB_ENDPOINT_TYPE_CONTROL) {
        urb->data_toggle = (p->qh->hw.overlay.token & QTD_TOKEN_TOGGLE) ? 1 : 0;
    }
    if (result == 0) {
        h->stats.completions++;
        g_ehci_iaa_stats[bus->hcd.hcd_id].ok_count++;
        if (p && p->type == USB_ENDPOINT_TYPE_CONTROL) {
            ehci_note_clear_halt(bus, p, urb);
        }
    } else if (result != -USB_ERR_SHUTDOWN) {
        h->stats.errors++;
    }
    if (x->sync) {
        x->done = 1;
        usb_osal_sem_give(x->sem);
    } else {
        xfer_release(x);
    }
    if (urb->complete) {
        urb->complete(urb->arg, result < 0 ? result : (int)actual);
    }
}

/* retire finished transfers of one pipe, in order (critical section held) */
static void ehci_scan_pipe(struct usbh_bus *bus, struct ehci_nxp_pipe *p)
{
    struct ehci_nxp_hcd *h = hcd_of(bus);
    struct ehci_nxp_xfer *x;

    while (p->inuse && !p->closing && (x = p->head) != NULL) {
        struct ehci_nxp_qtd *hq;
        uint32_t actual;
        int st = ehci_xfer_state(h, x, &actual, &hq), result = 0;

        if (st == XFER_RUNNING) {
            return;
        }
        if (st == XFER_HALTED) {
            uint32_t tok = hq->hw.token;
            struct ehci_qtd *ov = &p->qh->hw.overlay;

            /* transaction errors only (CERR exhausted): restart the qTD in place (Linux) */
            if ((tok & QTD_TOKEN_STATUS_XACTERR) &&
                !(tok & (QTD_TOKEN_STATUS_BABBLE | QTD_TOKEN_STATUS_DBERR)) &&
                p->xacterrs < CONFIG_USB_EHCI_XACTERR_RETRIES &&
                LINK_ADDR(p->qh->hw.curr_qtd) == QTD_PTR(hq)) {
                uint32_t nt = (tok & ~(QTD_TOKEN_STATUS_MASK | QTD_TOKEN_CERR_MASK)) |
                              ((uint32_t)EHCI_TUNE_CERR << QTD_TOKEN_CERR_SHIFT) | QTD_TOKEN_STATUS_ACTIVE;

                p->xacterrs++;
                g_ehci_xacterr_retries++;
                h->stats.xacterr_restarts++;
                hq->hw.token = nt;
                ehci_dma_wmb();
                /* the overlay keeps its toggle (DTC = 0) */
                ov->token = (nt & ~QTD_TOKEN_TOGGLE) |
                            (p->type == USB_ENDPOINT_TYPE_CONTROL ? (nt & QTD_TOKEN_TOGGLE) : (ov->token & QTD_TOKEN_TOGGLE));
                ehci_dma_wmb();
                return;
            }
            result = ehci_qtd_result(tok);
            if (result == -USB_ERR_STALL) {
                h->stats.stalls++;
            }
            /*
             * NXP TransactionDone: the queue goes on behind the failed transfer, error status
             * cleared in the overlay (a halted QH is not used by the controller). After a STALL
             * the endpoint restarts with DATA0.
             */
            h->stats.halted_restarts++;
            ov->next_qtd = x->last->hw.next_qtd;
            ov->alt_next_qtd = QTD_LIST_END;
            ehci_dma_wmb();
            ov->token = (result == -USB_ERR_STALL || p->type == USB_ENDPOINT_TYPE_CONTROL) ? 0 :
                                                                                            (ov->token & QTD_TOKEN_TOGGLE);
            ehci_dma_wmb();
        }
        p->xacterrs = 0;
        p->head = x->next;
        if (p->head == NULL) {
            p->tail = NULL;
        }
        x->next = NULL;
        xfer_free_qtds(h, x);
        ehci_xfer_giveback(bus, x, result, actual);
    }
}

void ehci_nxp_scan(struct usbh_bus *bus)
{
    struct ehci_nxp_hcd *h = hcd_of(bus);

    for (uint32_t i = 0; i < CONFIG_USB_EHCI_QH_NUM; i++) {
        if (h->pipes[i].inuse && h->pipes[i].head) {
            ehci_scan_pipe(bus, &h->pipes[i]);
        }
    }
}

/*
 * Cancel one transfer (NXP USB_HostEhciTransferQtdListDeinit): schedule stopped with the
 * handshake, its qTDs taken out of the chain (predecessor next/alternate pointers and the
 * overlay redirected to what follows it, toggle kept), schedule started again. No IAA.
 */
static void ehci_xfer_cancel(struct usbh_bus *bus, struct ehci_nxp_pipe *p, struct ehci_nxp_xfer *x)
{
    struct ehci_nxp_hcd *h = hcd_of(bus);
    struct ehci_nxp_xfer *prev = NULL, *it;
    struct ehci_qh *hw = &p->qh->hw;
    uint32_t after, first = QTD_PTR(x->first), actual = 0;
    struct ehci_nxp_qtd *hq;

    for (it = p->head; it && it != x; it = it->next) {
        prev = it;
    }
    if (it == NULL) {
        return;
    }
    ehci_stop_schedule(bus, p);
    after = x->last->hw.next_qtd;
    (void)ehci_xfer_state(h, x, &actual, &hq);
    if (prev) {
        prev->last->hw.next_qtd = after;
        for (struct ehci_nxp_qtd *q = prev->first; q; q = xfer_qtd_next(h, prev, q)) {
            if (q->hw.alt_next_qtd == first) {
                q->hw.alt_next_qtd = after;
            }
        }
    }
    if (xfer_owns_ptr(h, x, hw->curr_qtd) &&
        (hw->overlay.token & (QTD_TOKEN_STATUS_ACTIVE | QTD_TOKEN_STATUS_HALTED) ||
         LINK_ADDR(hw->overlay.next_qtd) != LINK_ADDR(after))) {
        /* the controller is in (or halted in) this transfer: the overlay skips the rest of it */
        hw->overlay.next_qtd = after;
        hw->overlay.alt_next_qtd = QTD_LIST_END;
        hw->overlay.token = p->type == USB_ENDPOINT_TYPE_CONTROL ? 0 : (hw->overlay.token & QTD_TOKEN_TOGGLE);
    } else {
        if (LINK_ADDR(hw->overlay.next_qtd) == first) {
            hw->overlay.next_qtd = after;
        }
        if (LINK_ADDR(hw->overlay.alt_next_qtd) == first) {
            hw->overlay.alt_next_qtd = after;
        }
    }
    ehci_start_schedule(bus, p);

    if (prev) {
        prev->next = x->next;
    } else {
        p->head = x->next;
    }
    if (p->tail == x) {
        p->tail = prev;
    }
    x->next = NULL;
    p->xacterrs = 0;
    xfer_free_qtds(h, x);
    h->stats.kills++;
    ehci_xfer_giveback(bus, x, -USB_ERR_SHUTDOWN, actual);
}

/* ---- roothub (CherryUSB EHCI port, W1C-safe register updates) ---- */

static int usbh_reset_port(struct usbh_bus *bus, const uint8_t port)
{
    uint32_t timeout = 0;

    portsc_update(bus, port - 1, EHCI_PORTSC_PE, EHCI_PORTSC_RESET);
    usb_osal_msleep(55);
    portsc_update(bus, port - 1, EHCI_PORTSC_RESET, 0);
    while (op_rd(bus, EHCI_NXP_PORTSC(port - 1)) & EHCI_PORTSC_RESET) {
        usb_osal_msleep(1);
        if (++timeout > 100) {
            return -USB_ERR_TIMEOUT;
        }
    }
    return 0;
}

int usbh_roothub_control(struct usbh_bus *bus, struct usb_setup_packet *setup, uint8_t *buf)
{
    struct ehci_nxp_hcd *h = hcd_of(bus);
    uint8_t nports = h->n_ports;
    uint8_t port = setup->wIndex;
    uint32_t temp, status;

    if (setup->bmRequestType & USB_REQUEST_RECIPIENT_DEVICE) {
        switch (setup->bRequest) {
            case HUB_REQUEST_CLEAR_FEATURE:
            case HUB_REQUEST_SET_FEATURE:
                if (setup->wValue != HUB_FEATURE_HUB_C_LOCALPOWER &&
                    setup->wValue != HUB_FEATURE_HUB_C_OVERCURRENT) {
                    return -USB_ERR_NOTSUPP;
                }
                break;
            case HUB_REQUEST_GET_STATUS:
                memset(buf, 0, 4);
                break;
            default:
                break;
        }
        return 0;
    }
    if (!(setup->bmRequestType & USB_REQUEST_RECIPIENT_OTHER)) {
        return 0;
    }
    if (!port || port > nports) {
        return -USB_ERR_INVAL;
    }
    switch (setup->bRequest) {
        case HUB_REQUEST_CLEAR_FEATURE:
            switch (setup->wValue) {
                case HUB_PORT_FEATURE_ENABLE:
                    portsc_update(bus, port - 1, EHCI_PORTSC_PE, 0);
                    break;
                case HUB_PORT_FEATURE_SUSPEND: {
                    uint32_t t = 0;

                    portsc_update(bus, port - 1, 0, EHCI_PORTSC_RESUME);
                    usb_osal_msleep(20);
                    portsc_update(bus, port - 1, EHCI_PORTSC_RESUME, 0);
                    while ((op_rd(bus, EHCI_NXP_PORTSC(port - 1)) & EHCI_PORTSC_RESUME) && ++t < 100) {
                        usb_osal_msleep(1);
                    }
                    usbcmd_update(bus, 0, EHCI_USBCMD_RUN);
                    ehci_start_periodic(bus);
                    ehci_start_async(bus);
                    break;
                }
                case HUB_PORT_FEATURE_C_SUSPEND:
                case HUB_PORT_FEATURE_C_RESET:
                    break;
                case HUB_PORT_FEATURE_POWER:
                    portsc_update(bus, port - 1, EHCI_PORTSC_PP, 0);
                    break;
                case HUB_PORT_FEATURE_C_CONNECTION:
                    portsc_update(bus, port - 1, 0, EHCI_PORTSC_CSC);
                    break;
                case HUB_PORT_FEATURE_C_ENABLE:
                    portsc_update(bus, port - 1, 0, EHCI_PORTSC_PEC);
                    break;
                case HUB_PORT_FEATURE_C_OVER_CURREN:
                    portsc_update(bus, port - 1, 0, EHCI_PORTSC_OCC);
                    break;
                default:
                    return -USB_ERR_NOTSUPP;
            }
            break;
        case HUB_REQUEST_SET_FEATURE:
            switch (setup->wValue) {
                case HUB_PORT_FEATURE_SUSPEND: {
                    uint32_t t = 0;

                    ehci_stop_async(bus);
                    ehci_stop_periodic(bus);
                    portsc_update(bus, port - 1, 0, EHCI_PORTSC_SUSPEND);
                    while (!(op_rd(bus, EHCI_NXP_PORTSC(port - 1)) & EHCI_PORTSC_SUSPEND) && ++t < 100) {
                        usb_osal_msleep(1);
                    }
                    break;
                }
                case HUB_PORT_FEATURE_POWER:
                    portsc_update(bus, port - 1, 0, EHCI_PORTSC_PP);
                    break;
                case HUB_PORT_FEATURE_RESET:
                    return usbh_reset_port(bus, port);
                default:
                    return -USB_ERR_NOTSUPP;
            }
            break;
        case HUB_REQUEST_GET_STATUS:
            temp = op_rd(bus, EHCI_NXP_PORTSC(port - 1));
            status = 0;
            if (temp & EHCI_PORTSC_CSC) {
                status |= (1 << HUB_PORT_FEATURE_C_CONNECTION);
            }
            if (temp & EHCI_PORTSC_PEC) {
                status |= (1 << HUB_PORT_FEATURE_C_ENABLE);
            }
            if (temp & EHCI_PORTSC_OCC) {
                status |= (1 << HUB_PORT_FEATURE_C_OVER_CURREN);
            }
            if (temp & EHCI_PORTSC_CCS) {
                status |= (1 << HUB_PORT_FEATURE_CONNECTION);
            }
            if (temp & EHCI_PORTSC_PE) {
                uint8_t speed = usbh_get_port_speed(bus, port);

                status |= (1 << HUB_PORT_FEATURE_ENABLE);
                if (speed == USB_SPEED_LOW) {
                    status |= (1 << HUB_PORT_FEATURE_LOWSPEED);
                } else if (speed == USB_SPEED_HIGH) {
                    status |= (1 << HUB_PORT_FEATURE_HIGHSPEED);
                }
            }
            if (temp & EHCI_PORTSC_SUSPEND) {
                status |= (1 << HUB_PORT_FEATURE_SUSPEND);
            }
            if (temp & EHCI_PORTSC_OCA) {
                status |= (1 << HUB_PORT_FEATURE_OVERCURRENT);
            }
            if (temp & EHCI_PORTSC_RESET) {
                status |= (1 << HUB_PORT_FEATURE_RESET);
            }
            if ((temp & EHCI_PORTSC_PP) || !h->ppc) {
                status |= (1 << HUB_PORT_FEATURE_POWER);
            }
            memcpy(buf, &status, 4);
            break;
        default:
            break;
    }
    return 0;
}

/* ---- controller (NXP USB_HostEhciResetIP/StartIP, CherryUSB usb_hc_init) ---- */

__WEAK void usb_hc_low_level_init(struct usbh_bus *bus)
{
    (void)bus;
}

__WEAK void usb_hc_low_level2_init(struct usbh_bus *bus)
{
    (void)bus;
}

__WEAK void usb_hc_low_level_deinit(struct usbh_bus *bus)
{
    (void)bus;
}

int usb_hc_init(struct usbh_bus *bus)
{
    uint8_t id = bus->hcd.hcd_id;
    struct ehci_nxp_hcd *h = &g_ehci_nxp[id];
    uint32_t i, regval, timeout;
    struct ehci_nxp_qh *head;

    bus->hcd.roothub.speed = USB_SPEED_HIGH;

    for (i = 0; i < CONFIG_USB_EHCI_NXP_XFER_NUM; i++) {
        if (h->xfers[i].sem) {
            usb_osal_sem_delete(h->xfers[i].sem);
        }
    }
    memset(h, 0, sizeof(*h));
    h->qh_pool = g_ehci_nxp_qh[id];
    h->qtd_pool = g_ehci_nxp_qtd[id];
    h->async_head = &g_ehci_nxp_async_head[id];
    h->framelist = g_framelist[id];
    memset(h->qh_pool, 0, sizeof(g_ehci_nxp_qh[id]));
    memset(h->qtd_pool, 0, sizeof(g_ehci_nxp_qtd[id]));
    if (((uintptr_t)h->qh_pool | (uintptr_t)h->qtd_pool | (uintptr_t)h->async_head) & 0x1F) {
        USB_LOG_ERR("ehci-nxp: descriptors not 32-byte aligned\r\n");
        return -USB_ERR_INVAL;
    }
    for (i = CONFIG_USB_EHCI_QTD_NUM; i-- > 0;) {
        qtd_free(h, &h->qtd_pool[i]);
    }
    for (i = 0; i < CONFIG_USB_EHCI_NXP_XFER_NUM; i++) {
        h->xfers[i].sem = usb_osal_sem_create(0);
    }

    /* async head: H bit, halted overlay, a ring of itself (NXP shedFirstQh) */
    head = h->async_head;
    memset(head, 0, sizeof(*head));
    head->hw.hlp = QH_LINK(head);
    head->hw.epchar = QH_EPCHAR_H;
    head->hw.overlay.next_qtd = QTD_LIST_END;
    head->hw.overlay.alt_next_qtd = QTD_LIST_END;
    head->hw.overlay.token = QTD_TOKEN_STATUS_HALTED;
    for (i = 0; i < CONFIG_USB_EHCI_FRAME_LIST_SIZE; i++) {
        h->framelist[i] = QH_HLP_END;
    }
    ehci_dma_wmb();

    usb_hc_low_level_init(bus);

    h->hcor_offset = cap_rd(bus, EHCI_NXP_CAPLENGTH) & 0xff;
    regval = cap_rd(bus, EHCI_NXP_HCSPARAMS);
    h->ppc = (regval & EHCI_HCSPARAMS_PPC) != 0;
    h->n_ports = (regval & EHCI_HCSPARAMS_NPORTS_MASK) >> EHCI_HCSPARAMS_NPORTS_SHIFT;
    if (h->n_ports == 0 || h->n_ports > CONFIG_USBHOST_MAX_RHPORTS) {
        h->n_ports = CONFIG_USBHOST_MAX_RHPORTS;
    }
    USB_LOG_INFO("EHCI (NXP model) HCSPARAMS:0x%06x caplength 0x%02x ports %u ppc %u\r\n",
                 (unsigned int)regval, (unsigned int)h->hcor_offset, h->n_ports, h->ppc);

    /* NXP ResetIP: stop, reset */
    usbcmd_update(bus, EHCI_USBCMD_RUN, 0);
    usb_osal_msleep(2);
    op_wr(bus, EHCI_NXP_USBCMD, EHCI_USBCMD_HCRESET);
    timeout = 0;
    while (op_rd(bus, EHCI_NXP_USBCMD) & EHCI_USBCMD_HCRESET) {
        usb_osal_msleep(1);
        if (++timeout > 100) {
            return -USB_ERR_TIMEOUT;
        }
    }
    op_wr(bus, EHCI_NXP_USBINTR, 0);
    op_wr(bus, EHCI_NXP_USBSTS, op_rd(bus, EHCI_NXP_USBSTS));

    /* host mode, ULPI, park mode and the other tunables: re-applied after every reset */
    usb_hc_low_level2_init(bus);

    op_wr(bus, EHCI_NXP_ASYNCLIST, LINK_ADDR(QH_LINK(head)));
    op_wr(bus, EHCI_NXP_PERIODICLIST, (uint32_t)usb_ramaddr2phyaddr((uintptr_t)h->framelist));

    /* NXP StartIP: frame list size, no interrupt threshold, run */
    regval = op_rd(bus, EHCI_NXP_USBCMD);
    regval &= ~(EHCI_USBCMD_ITHRE_MASK | EHCI_USBCMD_FLSIZE_MASK | EHCI_USBCMD_IAAD |
                EHCI_USBCMD_ASEN | EHCI_USBCMD_PSEN);
#if CONFIG_USB_EHCI_FRAME_LIST_SIZE == 1024
    regval |= EHCI_USBCMD_FLSIZE_1024;
#elif CONFIG_USB_EHCI_FRAME_LIST_SIZE == 512
    regval |= EHCI_USBCMD_FLSIZE_512;
#elif CONFIG_USB_EHCI_FRAME_LIST_SIZE == 256
    regval |= EHCI_USBCMD_FLSIZE_256;
#else
#error Unsupported frame list size
#endif
    regval |= EHCI_USBCMD_RUN;
    ehci_dma_wmb();
    op_wr(bus, EHCI_NXP_USBCMD, regval);
    timeout = 0;
    while (op_rd(bus, EHCI_NXP_USBSTS) & EHCI_USBSTS_HALTED) {
        usb_osal_msleep(1);
        if (++timeout > 100) {
            return -USB_ERR_TIMEOUT;
        }
    }
    h->running = true;
    ehci_start_periodic(bus);
    ehci_start_async(bus);

    if (h->ppc) {
        for (uint8_t port = 0; port < h->n_ports; port++) {
            portsc_update(bus, port, 0, EHCI_PORTSC_PP);
        }
    }
    /* USBINT, USBERRINT, port change, host system error (no IAA: no doorbell is ever rung) */
    op_wr(bus, EHCI_NXP_USBINTR, EHCI_USBIE_INT | EHCI_USBIE_ERR | EHCI_USBIE_PCD | EHCI_USBIE_FATAL);
    return 0;
}

int usb_hc_deinit(struct usbh_bus *bus)
{
    struct ehci_nxp_hcd *h = hcd_of(bus);
    uint32_t timeout = 0;
    size_t flags;

    flags = usb_osal_enter_critical_section();
    for (uint32_t i = 0; i < CONFIG_USB_EHCI_QH_NUM; i++) {
        if (h->pipes[i].inuse) {
            ehci_pipe_close(bus, &h->pipes[i], -USB_ERR_SHUTDOWN);
        }
    }
    op_wr(bus, EHCI_NXP_USBINTR, 0);
    usbcmd_update(bus, EHCI_USBCMD_ASEN | EHCI_USBCMD_PSEN | EHCI_USBCMD_RUN, 0);
    usb_osal_leave_critical_section(flags);

    while (!(op_rd(bus, EHCI_NXP_USBSTS) & EHCI_USBSTS_HALTED)) {
        usb_osal_msleep(1);
        if (++timeout > 100) {
            break;
        }
    }
    if (h->ppc) {
        for (uint8_t port = 0; port < h->n_ports; port++) {
            portsc_update(bus, port, EHCI_PORTSC_PP, 0);
        }
    }
    op_wr(bus, EHCI_NXP_USBSTS, op_rd(bus, EHCI_NXP_USBSTS));
    op_wr(bus, EHCI_NXP_USBCMD, EHCI_USBCMD_HCRESET);
    h->running = false;
    for (uint32_t i = 0; i < CONFIG_USB_EHCI_NXP_XFER_NUM; i++) {
        if (h->xfers[i].sem) {
            usb_osal_sem_delete(h->xfers[i].sem);
            h->xfers[i].sem = NULL;
        }
    }
    usb_hc_low_level_deinit(bus);
    return timeout > 100 ? -USB_ERR_TIMEOUT : 0;
}

uint16_t usbh_get_frame_number(struct usbh_bus *bus)
{
    return (uint16_t)(((op_rd(bus, EHCI_NXP_FRINDEX) & EHCI_FRINDEX_MASK) >> 3) & 0x3ff);
}

/* ---- URB API ---- */

int usbh_submit_urb(struct usbh_urb *urb)
{
    struct usbh_bus *bus;
    struct ehci_nxp_hcd *h;
    struct ehci_nxp_pipe *p;
    struct ehci_nxp_xfer *x;
    struct usbh_hubport *rh;
    struct usbh_hub *hub;
    uint8_t type;
    size_t flags;
    int ret;

    if (!urb || !urb->hport || !urb->ep || !urb->hport->bus) {
        return -USB_ERR_INVAL;
    }
#ifdef CONFIG_USB_DCACHE_ENABLE
    USB_ASSERT_MSG(!((uintptr_t)urb->setup % CONFIG_USB_ALIGN_SIZE) &&
                       !((uintptr_t)urb->transfer_buffer % CONFIG_USB_ALIGN_SIZE),
                   "urb->setup or urb->transfer_buffer is not aligned %d", CONFIG_USB_ALIGN_SIZE);
#endif
    bus = urb->hport->bus;
    h = hcd_of(bus);
    type = USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes);
    if (type == USB_ENDPOINT_TYPE_ISOCHRONOUS) {
        return -USB_ERR_NOTSUPP; /* no iTD/siTD support in this port (BT SCO is not used) */
    }
    if (type == USB_ENDPOINT_TYPE_CONTROL && urb->setup == NULL) {
        return -USB_ERR_INVAL;
    }
    if ((urb->transfer_flags & (USBH_EHCI_URB_STREAM | USBH_EHCI_URB_APPEND_ONLY)) &&
        urb->transfer_buffer_length > USBH_EHCI_STREAM_MAX_LEN) {
        return -USB_ERR_INVAL;
    }

    /* the root port the device hangs from */
    rh = urb->hport;
    hub = urb->hport->parent;
    while (hub && !hub->is_roothub) {
        rh = hub->parent;
        hub = hub->parent->parent;
    }
    if (!urb->hport->connected || rh->port == 0 ||
        !(op_rd(bus, EHCI_NXP_PORTSC(rh->port - 1)) & EHCI_PORTSC_CCS)) {
        return -USB_ERR_NOTCONN;
    }

    flags = usb_osal_enter_critical_section();
    if (!h->running) {
        usb_osal_leave_critical_section(flags);
        return -USB_ERR_NOTCONN;
    }
    if (urb->hcpriv && xfer_of_urb(h, urb)) {
        usb_osal_leave_critical_section(flags);
        return -USB_ERR_BUSY; /* still in the controller */
    }
    p = ehci_nxp_pipe_find(bus, urb->hport, urb->ep->bEndpointAddress, type);
    if (p) {
        uint32_t epchar, epcap;
        uint16_t period, ufi;

        ehci_ep_params(urb, &epchar, &epcap, &period, &ufi);
        if (epchar != p->epchar || epcap != p->epcap || period != p->period) {
            if (p->head) {
                usb_osal_leave_critical_section(flags);
                return -USB_ERR_BUSY;
            }
            if (p->periodic) {
                /* a new interval: the QH moves in the frame list */
                ehci_pipe_close(bus, p, -USB_ERR_SHUTDOWN);
                p = NULL;
            } else {
                /* NXP kUSB_HostUpdateControlPacketSize: rewritten with the schedule stopped */
                ehci_stop_async(bus);
                p->qh->hw.epchar = epchar;
                p->qh->hw.epcap = epcap;
                p->epchar = epchar;
                p->epcap = epcap;
                h->stats.ep_updates++;
                ehci_start_async(bus);
            }
        }
    }
    if (p == NULL) {
        p = ehci_pipe_open(bus, urb);
        if (p == NULL) {
            usb_osal_leave_critical_section(flags);
            return -USB_ERR_NOMEM;
        }
    }
    x = xfer_alloc(h);
    if (x == NULL) {
        usb_osal_leave_critical_section(flags);
        return -USB_ERR_NOMEM;
    }
    x->urb = urb;
    x->sync = urb->timeout > 0;
    if (x->sync) {
        usb_osal_sem_reset(x->sem);
    }
    urb->hcpriv = x;
    urb->errorcode = -USB_ERR_BUSY;
    urb->actual_length = 0;
    ret = ehci_xfer_queue(bus, p, x);
    if (ret < 0) {
        xfer_release(x);
        urb->errorcode = 0;
        usb_osal_leave_critical_section(flags);
        return ret;
    }
    h->stats.submits++;
    usb_osal_leave_critical_section(flags);

    if (!x->sync) {
        return 0;
    }
    ret = usb_osal_sem_take(x->sem, urb->timeout);
    flags = usb_osal_enter_critical_section();
    if (ret < 0 && !x->done) {
        /* timeout: cancel it (it may finish first; then its own result counts) */
        ehci_scan_pipe(bus, x->pipe);
        if (!x->done && x->pipe) {
            ehci_xfer_cancel(bus, x->pipe, x);
        }
        ret = urb->errorcode == -USB_ERR_SHUTDOWN ? -USB_ERR_TIMEOUT : urb->errorcode;
        urb->errorcode = ret;
    } else {
        ret = urb->errorcode;
    }
    urb->timeout = 0;
    xfer_release(x);
    usb_osal_leave_critical_section(flags);
    return ret;
}

int usbh_kill_urb(struct usbh_urb *urb)
{
    struct usbh_bus *bus;
    struct ehci_nxp_hcd *h;
    struct ehci_nxp_xfer *x;
    size_t flags;

    if (!urb || !urb->hport || !urb->hport->bus) {
        return -USB_ERR_INVAL;
    }
    bus = urb->hport->bus;
    h = hcd_of(bus);
    flags = usb_osal_enter_critical_section();
    if (!urb->hport->connected) {
        /* usbh_hubport_release(): the device is gone, its pipes close (NXP ClosePipe) */
        ehci_nxp_close_hport(bus, urb->hport);
        usb_osal_leave_critical_section(flags);
        return 0;
    }
    x = urb->hcpriv ? xfer_of_urb(h, urb) : NULL;
    if (x == NULL || x->done || x->pipe == NULL) {
        usb_osal_leave_critical_section(flags);
        return x ? 0 : -USB_ERR_INVAL;
    }
    /* finished already: its own result */
    ehci_scan_pipe(bus, x->pipe);
    if (x->inuse && x->urb == urb && !x->done && x->pipe) {
        ehci_xfer_cancel(bus, x->pipe, x);
    }
    usb_osal_leave_critical_section(flags);
    return 0;
}

void USBH_IRQHandler(uint8_t busid)
{
    struct usbh_bus *bus = &g_usbhost_bus[busid];
    struct ehci_nxp_hcd *h = &g_ehci_nxp[busid];
    uint32_t usbsts;
    size_t flags;

    if (!h->running) {
        return;
    }
    flags = usb_osal_enter_critical_section();
    usbsts = op_rd(bus, EHCI_NXP_USBSTS) & op_rd(bus, EHCI_NXP_USBINTR);
    op_wr(bus, EHCI_NXP_USBSTS, usbsts);

    if (usbsts & (EHCI_USBSTS_INT | EHCI_USBSTS_ERR)) {
        ehci_nxp_scan(bus);
    }
    if (usbsts & EHCI_USBSTS_PCD) {
        for (uint8_t port = 0; port < h->n_ports; port++) {
            if (op_rd(bus, EHCI_NXP_PORTSC(port)) & EHCI_PORTSC_CSC) {
                bus->hcd.roothub.int_buffer[0] |= (1 << (port + 1));
                usbh_hub_thread_wakeup(&bus->hcd.roothub);
            }
        }
    }
    if (usbsts & EHCI_USBSTS_FATAL) {
        g_ehci_iaa_stats[busid].fatal++;
    }
    usb_osal_leave_critical_section(flags);
}

/* ---- compatibility with the patched CherryUSB EHCI port ---- */

uint32_t usbh_ehci_iaa_pending(uint8_t busid)
{
    (void)busid;
    return 0;
}

uint32_t usbh_ehci_iaa_watchdog(uint8_t busid)
{
    (void)busid;
    return 0;
}

uint32_t usbh_ehci_async_watchdog(uint8_t busid)
{
    (void)busid;
    return 0;
}

static uint32_t g_ehci_diag_count, g_ehci_diag_last_us;

int usbh_ehci_diag_allowed(void)
{
    size_t flags = usb_osal_enter_critical_section();
    uint32_t now = usb_ehci_time_us();
    int ok = g_ehci_diag_count < 4 || (uint32_t)(now - g_ehci_diag_last_us) >= 10000000U;

    if (ok) {
        g_ehci_diag_count++;
        g_ehci_diag_last_us = now;
    }
    usb_osal_leave_critical_section(flags);
    return ok;
}

void usbh_ehci_diag_capture(struct usbh_urb *urb, struct usbh_ehci_diag *d)
{
    struct usbh_bus *bus = (urb && urb->hport && urb->hport->bus) ? urb->hport->bus : &g_usbhost_bus[0];
    struct ehci_nxp_hcd *h = hcd_of(bus);
    struct ehci_nxp_xfer *x;
    struct ehci_nxp_pipe *p = NULL;
    struct ehci_nxp_qh *it;
    uint32_t t, guard = 0;
    size_t flags;

    memset(d, 0, sizeof(*d));
    d->qh_index = -1;
    flags = usb_osal_enter_critical_section();
    d->t_us = usb_ehci_time_us();
    x = (urb && urb->hcpriv) ? xfer_of_urb(h, urb) : NULL;
    if (x && x->pipe) {
        p = x->pipe;
    } else if (urb && urb->hport && urb->ep) {
        p = ehci_nxp_pipe_find(bus, urb->hport, urb->ep->bEndpointAddress,
                               USB_GET_ENDPOINT_TYPE(urb->ep->bmAttributes));
    }
    if (p) {
        struct ehci_nxp_qtd *q = qtd_from_ptr(h, p->qh->hw.overlay.next_qtd);

        d->qh_index = (int)(p->qh - h->qh_pool);
        d->hlp = p->qh->hw.hlp;
        d->epchar = p->qh->hw.epchar;
        d->epcap = p->qh->hw.epcap;
        d->curr = p->qh->hw.curr_qtd;
        d->next = p->qh->hw.overlay.next_qtd;
        d->alt = p->qh->hw.overlay.alt_next_qtd;
        d->token = p->qh->hw.overlay.token;
        d->stream = p->head && p->head->next;
        d->stream_open = p->linked;
        if (x && x->first) {
            q = x->first;
        }
        while (q && d->nqtd < USBH_EHCI_DIAG_QTDS) {
            d->qtd_addr[d->nqtd] = QTD_PTR(q);
            d->qtd_token[d->nqtd] = q->hw.token;
            d->nqtd++;
            if (q == p->dummy) {
                break;
            }
            q = qtd_from_ptr(h, q->hw.next_qtd);
        }
        if (!p->periodic) {
            for (it = qh_from_link(h, h->async_head->hw.hlp); it && guard < 64;
                 it = qh_from_link(h, it->hw.hlp), guard++) {
                d->ring_len++;
                if (it == p->qh) {
                    d->on_ring = 1;
                }
            }
        } else {
            d->on_ring = p->linked;
        }
    }
    d->usbcmd = op_rd(bus, EHCI_NXP_USBCMD);
    d->usbsts = op_rd(bus, EHCI_NXP_USBSTS);
    d->async = op_rd(bus, EHCI_NXP_ASYNCLIST);
    d->frindex0 = op_rd(bus, EHCI_NXP_FRINDEX);
    t = usb_ehci_time_us();
    while ((uint32_t)(usb_ehci_time_us() - t) < 250) {
    }
    d->frindex1 = op_rd(bus, EHCI_NXP_FRINDEX);
    d->portsc = op_rd(bus, EHCI_NXP_PORTSC(0));
    d->ok_count = g_ehci_iaa_stats[bus->hcd.hcd_id].ok_count;
    usb_osal_leave_critical_section(flags);
}

void usbh_ehci_diag_print(const struct usbh_ehci_diag *d, const char *tag)
{
    uint32_t sts = d->usbsts, ps = d->portsc;

    if (d->qh_index < 0) {
        CONFIG_USB_PRINTF("ehci: %s: no pipe for this endpoint\n", tag);
    } else {
        char line[USBH_EHCI_DIAG_QTDS * 24 + 1];
        int pos = 0;

        for (uint32_t i = 0; i < d->nqtd && pos < (int)sizeof(line) - 24; i++) {
            pos += snprintf(line + pos, sizeof(line) - pos, " %08lx:%08lx", (unsigned long)d->qtd_addr[i],
                            (unsigned long)d->qtd_token[i]);
        }
        line[pos] = 0;
        CONFIG_USB_PRINTF("ehci: %s: pipe qh %d hlp %08lx epchar %08lx epcap %08lx cur %08lx next %08lx "
                          "alt %08lx token %08lx, %s the schedule (%u async QHs), queued>1 %u\n",
                          tag, d->qh_index, (unsigned long)d->hlp, (unsigned long)d->epchar,
                          (unsigned long)d->epcap, (unsigned long)d->curr, (unsigned long)d->next,
                          (unsigned long)d->alt, (unsigned long)d->token, d->on_ring ? "on" : "NOT on",
                          (unsigned)d->ring_len, d->stream);
        CONFIG_USB_PRINTF("ehci: %s: qtd chain (addr:token):%s\n", tag, line);
    }
    CONFIG_USB_PRINTF("ehci: %s: USBCMD %08lx (RUN %lu ASE %lu PSE %lu) USBSTS %08lx (ASS %lu PSS %lu HCH %lu) "
                      "ASYNCLISTADDR %08lx FRINDEX %04lx->%04lx PORTSC1 %08lx (CCS %lu PE %lu SUSP %lu "
                      "speed %lu); completions ok %lu\n",
                      tag, (unsigned long)d->usbcmd, (unsigned long)(d->usbcmd & 1),
                      (unsigned long)((d->usbcmd >> 5) & 1), (unsigned long)((d->usbcmd >> 4) & 1),
                      (unsigned long)sts, (unsigned long)((sts >> 15) & 1), (unsigned long)((sts >> 14) & 1),
                      (unsigned long)((sts >> 12) & 1), (unsigned long)d->async,
                      (unsigned long)(d->frindex0 & 0x3fff), (unsigned long)(d->frindex1 & 0x3fff),
                      (unsigned long)ps, (unsigned long)(ps & 1), (unsigned long)((ps >> 2) & 1),
                      (unsigned long)((ps >> 7) & 1), (unsigned long)((ps >> 26) & 3),
                      (unsigned long)d->ok_count);
}
