/*
 * Host tests of the ehci_nxp CherryUSB host controller driver (port/ehci_nxp) on the EHCI
 * controller model (sim_hc.c) with an AIC8800-like Wi-Fi + Bluetooth device (sim_dev.c).
 *
 * Only the driver's CherryUSB entry points are used (usb_hc_init/deinit, usbh_roothub_control,
 * usbh_submit_urb, usbh_kill_urb, USBH_IRQHandler via the model's interrupt line); its
 * exported state (g_ehci_nxp) is read for pool/leak accounting only. Transfers follow the
 * AIC adapter (aic/aicwf_cherry.c): asynchronous URBs (timeout 0) completed in the interrupt
 * handler with the critical section held and resubmitted from the completion with the data
 * toggle carried in urb->data_toggle, timeouts by usbh_kill_urb() from a thread holding the
 * critical section; plus the core's synchronous control transfers (urb->timeout != 0).
 *
 * Every data frame carries a sequence number and a pattern; the device and the host side both
 * verify order and content, the model verifies the EHCI rules (sim_hc.h violations).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "usbh_core.h"
#include "usb_hc_ehci.h"
#include "sim_dev.h"
#include "sim_hc.h"

extern uint32_t g_sim_hub_wakeups, g_sim_sems, g_sim_barriers;

static int g_fail;
static const char *g_test;
#define CHECK(c)                                                                            \
    do {                                                                                    \
        if (!(c)) {                                                                         \
            printf("  FAIL %s:%d (%s): %s\n", __FILE__, __LINE__, g_test, #c);              \
            g_fail++;                                                                       \
        }                                                                                   \
    } while (0)
#define CHECKV(c, fmt, ...)                                                                 \
    do {                                                                                    \
        if (!(c)) {                                                                         \
            printf("  FAIL %s:%d (%s): %s: " fmt "\n", __FILE__, __LINE__, g_test, #c,       \
                   ##__VA_ARGS__);                                                          \
            g_fail++;                                                                       \
        }                                                                                   \
    } while (0)

static struct usbh_bus *const B = &g_usbhost_bus[0];
static struct usbh_hubport g_hp;
static struct usb_setup_packet *g_setup;
static uint8_t *g_ctlbuf;
static struct usbh_urb g_ctl_urb;
static int g_stop;
static int g_toggle_noise; /* wk_submit: a random urb->data_toggle (a class driver's stale copy) */

static struct usb_endpoint_descriptor g_ep_out_data, g_ep_out_msg, g_ep_in_data, g_ep_in_msg, g_ep_evt,
    g_ep_acl_in, g_ep_acl_out;

static void mk_ep(struct usb_endpoint_descriptor *e, uint8_t addr, uint8_t type, uint16_t mps, uint8_t ival)
{
    memset(e, 0, sizeof(*e));
    e->bLength = 7;
    e->bDescriptorType = 5;
    e->bEndpointAddress = addr;
    e->bmAttributes = type;
    e->wMaxPacketSize = mps;
    e->bInterval = ival;
}

static struct ehci_nxp_hcd *H(void) { return &g_ehci_nxp[0]; }

/* ---------------- streams and workers (the AIC adapter's pattern) ---------------- */
struct stream {
    uint8_t ep;
    uint8_t toggle;          /* carried from completion to the next submit (ep->toggle) */
    uint32_t next_seq;       /* OUT: next frame to send; IN: next frame expected */
    uint32_t minlen, maxlen;
    uint64_t frames, bytes, bad, gap;
    uint32_t limit;          /* OUT: frames to send at most */
};

struct wk;
typedef void (*rx_fn)(struct wk *w, int n);
struct wk {
    struct usbh_urb urb;
    struct usb_endpoint_descriptor *ep;
    struct stream *st;
    uint8_t *buf;
    uint32_t buflen;
    int busy, want, is_in;
    uint32_t timeout_ms;
    uint64_t deadline;
    uint64_t done, ok, err, shut, tmo, submit_err;
    int last_err;
    rx_fn rx;
};

static void wk_submit(struct wk *w);

static void rx_frames(struct wk *w, int n)
{
    struct stream *s = w->st;
    int64_t seq;

    sim_cache_invalidate((uintptr_t)w->buf, (size_t)n); /* aicwf: invalidate IN after the transfer */
    seq = sim_frame_check(w->buf, (uint32_t)n, s->ep);
    s->frames++;
    s->bytes += (uint32_t)n;
    if (seq < 0) {
        s->bad++;
        if (s->bad <= 3) {
            printf("  [host] ep %#x: corrupt IN frame of %d bytes (%02x %02x %02x %02x)\n", s->ep, n, w->buf[0],
                   w->buf[1], w->buf[2], w->buf[3]);
        }
    } else if ((uint32_t)seq != (s->next_seq & 0xFFFF)) {
        s->gap++;
        if (s->gap <= 3) {
            printf("  [host] ep %#x: IN frame seq %u, expected %u\n", s->ep, (unsigned)seq, s->next_seq & 0xFFFF);
        }
        s->next_seq = (uint32_t)seq + 1;
    } else {
        s->next_seq++;
    }
}

static void wk_done(void *arg, int nbytes)
{
    struct wk *w = arg;

    if (!g_sim_in_isr && !g_sim_crit) {
        printf("  FAIL (%s): completion outside the interrupt handler/critical section\n", g_test);
        g_fail++;
    }
    w->busy = 0;
    w->done++;
    w->st->toggle = w->urb.data_toggle;
    if (nbytes >= 0) {
        w->ok++;
        if (w->is_in && nbytes > 0 && w->rx) {
            w->rx(w, nbytes);
        }
        if (!w->is_in) {
            w->st->frames++;
            w->st->bytes += (uint32_t)nbytes;
        }
    } else if (nbytes == -USB_ERR_SHUTDOWN) {
        w->shut++;
    } else {
        w->err++;
        w->last_err = nbytes;
    }
    if (w->want && !g_stop) {
        wk_submit(w);
    }
}

static void wk_submit(struct wk *w)
{
    uint32_t len = w->buflen;
    int rc;

    if (!w->is_in) {
        struct stream *s = w->st;
        if (s->limit && s->next_seq >= s->limit) {
            w->want = 0;
            return;
        }
        len = s->minlen + (s->maxlen > s->minlen ? sim_rand() % (s->maxlen - s->minlen + 1) : 0);
        if (len % USB_GET_MAXPACKETSIZE(w->ep->wMaxPacketSize) == 0) {
            len++; /* the device ends a frame at a short packet */
        }
        if (s->ep == 0x03) {
            len = sim_frame_acl(w->buf, len - 4, s->next_seq, s->ep);
        } else {
            len = sim_frame_wifi(w->buf, len, s->next_seq, s->ep);
        }
        s->next_seq++;
    }
    sim_cache_cpu_wrote(w->buf, w->is_in ? w->buflen : len); /* the driver must clean/flush */
    if (USB_GET_ENDPOINT_TYPE(w->ep->bmAttributes) == USB_ENDPOINT_TYPE_INTERRUPT) {
        usbh_int_urb_fill(&w->urb, &g_hp, w->ep, w->buf, len, 0, wk_done, w);
    } else {
        usbh_bulk_urb_fill(&w->urb, &g_hp, w->ep, w->buf, len, 0, wk_done, w);
    }
    w->urb.data_toggle = g_toggle_noise ? (uint8_t)(sim_rand() & 1) : w->st->toggle;
    w->busy = 1;
    rc = usbh_submit_urb(&w->urb);
    if (rc != 0) {
        w->busy = 0;
        w->submit_err++;
        w->last_err = rc;
        return;
    }
    w->deadline = w->timeout_ms ? sim_now_ns() + (uint64_t)w->timeout_ms * 1000000ULL : 0;
}

static void wk_init(struct wk *w, struct usb_endpoint_descriptor *ep, struct stream *st, uint32_t buflen)
{
    memset(w, 0, sizeof(*w));
    w->ep = ep;
    w->st = st;
    w->is_in = (ep->bEndpointAddress & 0x80) != 0;
    w->buflen = buflen;
    w->buf = sim_dma_alloc(buflen + 32, 32);
    sim_cache_track(w->buf, buflen + 32, w->is_in);
    w->rx = rx_frames;
}

static void wk_free(struct wk *w)
{
    sim_cache_untrack(w->buf);
    sim_dma_free(w->buf);
    w->buf = NULL;
}

static void stream_init(struct stream *s, uint8_t ep, uint32_t minlen, uint32_t maxlen)
{
    memset(s, 0, sizeof(*s));
    s->ep = ep;
    s->minlen = minlen;
    s->maxlen = maxlen;
}

/* the adapter's cancel: a thread kills under the critical section */
static void wk_kill(struct wk *w)
{
    size_t f = usb_osal_enter_critical_section();
    int want = w->want;
    w->want = 0;
    usbh_kill_urb(&w->urb);
    w->want = want;
    usb_osal_leave_critical_section(f);
}

static struct wk *g_wk[32];
static int g_nwk;

static void wk_register(struct wk *w) { g_wk[g_nwk++] = w; }

/* advance time; workers whose deadline passed are killed and resubmitted (timeout path) */
static void pump_us(uint64_t us)
{
    uint64_t end = sim_now_ns() + us * 1000ULL;
    while (sim_now_ns() < end) {
        sim_advance_ns(125000);
        for (int i = 0; i < g_nwk; i++) {
            struct wk *w = g_wk[i];
            if (w->busy && w->deadline && sim_now_ns() >= w->deadline) {
                w->tmo++;
                wk_kill(w);
                if (w->busy) {
                    printf("  FAIL (%s): usbh_kill_urb returned before the completion ran\n", g_test);
                    g_fail++;
                }
            }
            if (!w->busy && w->want && !g_stop) {
                wk_submit(w);
            }
        }
    }
}

/* ---------------- bring-up ---------------- */
static int rh_req(uint8_t bmrt, uint8_t req, uint16_t wv, uint16_t wi, uint8_t *buf)
{
    struct usb_setup_packet s = { .bmRequestType = bmrt, .bRequest = req, .wValue = wv, .wIndex = wi, .wLength = 4 };
    return usbh_roothub_control(B, &s, buf);
}

static uint32_t port_status(void)
{
    uint32_t st = 0;
    CHECK(rh_req(USB_REQUEST_DIR_IN | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER, HUB_REQUEST_GET_STATUS, 0, 1,
                 (uint8_t *)&st) == 0);
    return st;
}

static int ctrl(uint8_t bmrt, uint8_t req, uint16_t wv, uint16_t wi, uint16_t wl, uint32_t timeout)
{
    int rc;
    memset(&g_ctl_urb, 0, sizeof(g_ctl_urb));
    g_setup->bmRequestType = bmrt;
    g_setup->bRequest = req;
    g_setup->wValue = wv;
    g_setup->wIndex = wi;
    g_setup->wLength = wl;
    usbh_control_urb_fill(&g_ctl_urb, &g_hp, g_setup, wl ? g_ctlbuf : NULL, wl, timeout, NULL, NULL);
    rc = usbh_submit_urb(&g_ctl_urb);
    return rc < 0 ? rc : (int)g_ctl_urb.actual_length;
}

static void hport_init(void)
{
    memset(&g_hp, 0, sizeof(g_hp));
    g_hp.connected = true;
    g_hp.port = 1;
    g_hp.speed = USB_SPEED_HIGH;
    g_hp.dev_addr = 0;
    g_hp.parent = &B->hcd.roothub;
    g_hp.bus = B;
    mk_ep(&g_hp.ep0, 0, USB_ENDPOINT_TYPE_CONTROL, 64, 0);
}

static void bringup(void)
{
    uint32_t st;

    sim_hc_power_on_reset();
    memset(&g_dev_cfg, 0, sizeof(g_dev_cfg));
    g_dev_cfg.msg_latency_uf = 16;
    g_dev_cfg.hci_latency_uf = 16;
    g_dev_cfg.acl_ncp_every = 4;
    memset(&g_dev_stats, 0, sizeof(g_dev_stats));
    g_nwk = 0;
    g_stop = 0;
    memset(B, 0, sizeof(*B));
    B->busid = 0;
    B->hcd.reg_base = SIM_REG_BASE;
    B->hcd.hcd_id = 0;
    B->hcd.roothub.is_roothub = true;
    B->hcd.roothub.connected = true;
    B->hcd.roothub.bus = B;
    B->hcd.roothub.speed = USB_SPEED_HIGH;
    B->hcd.roothub.nports = 1;
    B->hcd.roothub.int_buffer = B->hcd.roothub_intbuf;
    if (!g_setup) {
        g_setup = sim_dma_alloc(64, 32);
        g_ctlbuf = sim_dma_alloc(1024, 32);
    }
    CHECK(usb_hc_init(B) == 0);
    CHECK(sim_reg_peek(0x1a8) == 3); /* USBMODE host */
    g_sim_hub_wakeups = 0;
    sim_port_connect();
    sim_advance_ms(2);
    CHECK(g_sim_hub_wakeups > 0);
    CHECK(B->hcd.roothub.int_buffer[0] & 2);
    st = port_status();
    CHECK(st & (1U << HUB_PORT_FEATURE_CONNECTION));
    CHECK(st & (1U << HUB_PORT_FEATURE_C_CONNECTION));
    CHECK(rh_req(USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER, HUB_REQUEST_CLEAR_FEATURE,
                 HUB_PORT_FEATURE_C_CONNECTION, 1, NULL) == 0);
    CHECK(!(port_status() & (1U << HUB_PORT_FEATURE_C_CONNECTION)));
    CHECK(rh_req(USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER, HUB_REQUEST_SET_FEATURE, HUB_PORT_FEATURE_RESET, 1,
                 NULL) == 0);
    st = port_status();
    CHECK(st & (1U << HUB_PORT_FEATURE_ENABLE));
    CHECK(st & (1U << HUB_PORT_FEATURE_HIGHSPEED));
    CHECK(!(st & (1U << HUB_PORT_FEATURE_RESET)));
    hport_init();
    mk_ep(&g_ep_out_data, 0x01, USB_ENDPOINT_TYPE_BULK, 512, 0);
    mk_ep(&g_ep_out_msg, 0x02, USB_ENDPOINT_TYPE_BULK, 512, 0);
    mk_ep(&g_ep_in_data, 0x81, USB_ENDPOINT_TYPE_BULK, 512, 0);
    mk_ep(&g_ep_in_msg, 0x82, USB_ENDPOINT_TYPE_BULK, 512, 0);
    mk_ep(&g_ep_evt, 0x83, USB_ENDPOINT_TYPE_INTERRUPT, 16, 4);
    mk_ep(&g_ep_acl_in, 0x84, USB_ENDPOINT_TYPE_BULK, 512, 0);
    mk_ep(&g_ep_acl_out, 0x03, USB_ENDPOINT_TYPE_BULK, 512, 0);
}

static void enumerate(uint8_t addr)
{
    int n;
    uint16_t total;

    CHECK((n = ctrl(0x80, 6, 0x0100, 0, 8, 500)) == 8);
    CHECK(g_ctlbuf[7] == 64);
    CHECK(ctrl(0x00, 5, addr, 0, 0, 500) == 0);
    g_hp.dev_addr = addr;
    sim_advance_ms(2);
    CHECK((n = ctrl(0x80, 6, 0x0100, 0, 18, 500)) == 18);
    CHECK(g_ctlbuf[0] == 18 && g_ctlbuf[1] == 1 && g_ctlbuf[8] == 0x9C);
    CHECK((n = ctrl(0x80, 6, 0x0200, 0, 9, 500)) == 9);
    total = (uint16_t)(g_ctlbuf[2] | (g_ctlbuf[3] << 8));
    CHECK(total > 9 && total < 256);
    CHECK((n = ctrl(0x80, 6, 0x0200, 0, total, 500)) == total);
    CHECK(g_ctlbuf[9 + 1] == 4 && g_ctlbuf[9 + 5] == 0xFF);
    CHECKV((n = ctrl(0x80, 6, 0x0300, 0, 255, 500)) == 4, "n=%d", n); /* short control read */
    CHECK((n = ctrl(0x80, 6, 0x0600, 0, 10, 500)) == 10);
    CHECK(ctrl(0x00, 9, 1, 0, 0, 500) == 0);
    CHECK(g_dev_stats.addr == addr);
    CHECK(g_dev_stats.config == 1);
}

/* ---------------- accounting ---------------- */
struct pools {
    uint32_t pipes, xfers, qtds_free, qtds_in_xfers, linked_async, linked_periodic;
};

static struct pools pools_now(void)
{
    struct pools p = { 0 };
    struct ehci_nxp_hcd *h = H();

    for (int i = 0; i < CONFIG_USB_EHCI_QH_NUM; i++) {
        struct ehci_nxp_pipe *pp = &h->pipes[i];
        if (!pp->inuse) {
            continue;
        }
        p.pipes++;
        if (pp->linked) {
            if (pp->periodic) {
                p.linked_periodic++;
            } else {
                p.linked_async++;
            }
        }
        for (struct ehci_nxp_xfer *x = pp->head; x; x = x->next) {
            p.qtds_in_xfers += x->nqtd;
        }
    }
    for (int i = 0; i < CONFIG_USB_EHCI_NXP_XFER_NUM; i++) {
        p.xfers += h->xfers[i].inuse;
    }
    p.qtds_free = h->qtd_free_count;
    return p;
}

static int busy_workers(void)
{
    int n = 0;
    for (int i = 0; i < g_nwk; i++) {
        n += g_wk[i]->busy;
    }
    return n;
}

/* qTDs: free + one dummy per open pipe + those of queued transfers == pool; xfers == URBs in flight */
static void check_accounting(const char *where)
{
    struct pools p = pools_now();
    uint32_t ring[64], pq[64];
    uint32_t nring = sim_async_ring(ring, 64), npq = sim_periodic_qhs(pq, 64);

    CHECKV(p.qtds_free + p.pipes + p.qtds_in_xfers == CONFIG_USB_EHCI_QTD_NUM,
           "%s: qTD free %u + dummies %u + queued %u != %u", where, p.qtds_free, p.pipes, p.qtds_in_xfers,
           CONFIG_USB_EHCI_QTD_NUM);
    CHECKV(p.xfers == (uint32_t)busy_workers(), "%s: %u transfer slots in use, %d URBs in flight", where, p.xfers,
           busy_workers());
    CHECKV(nring == 1 + p.linked_async, "%s: async ring %u QHs, driver linked %u + head", where, nring, p.linked_async);
    CHECKV(npq == p.linked_periodic, "%s: periodic list %u QHs, driver linked %u", where, npq, p.linked_periodic);
}

/* device gone or test over: every pipe closed, every pool full, ring and frame list empty */
static void close_all_and_check(void)
{
    struct pools p;
    uint32_t ring[64], pq[64];

    g_stop = 1;
    for (int i = 0; i < g_nwk; i++) {
        g_wk[i]->want = 0;
    }
    g_hp.connected = false;
    {
        size_t f = usb_osal_enter_critical_section();
        CHECK(usbh_kill_urb(&g_ctl_urb) == 0); /* usbh_hubport_release() path: closes the pipes */
        usb_osal_leave_critical_section(f);
    }
    sim_advance_ms(2);
    p = pools_now();
    CHECKV(p.pipes == 0, "%u pipes still open", p.pipes);
    CHECKV(p.xfers == 0, "%u transfer slots still in use", p.xfers);
    CHECKV(p.qtds_free == CONFIG_USB_EHCI_QTD_NUM, "qTD pool %u/%u", p.qtds_free, CONFIG_USB_EHCI_QTD_NUM);
    CHECKV(busy_workers() == 0, "%d URBs never completed", busy_workers());
    CHECKV(sim_async_ring(ring, 64) == 1, "async ring still has %u QHs", sim_async_ring(ring, 64));
    CHECKV(sim_periodic_qhs(pq, 64) == 0, "frame list still has %u QHs", sim_periodic_qhs(pq, 64));
    CHECK(usb_hc_deinit(B) == 0);
    CHECKV(g_sim_sems == 0, "%u semaphores leaked", g_sim_sems);
    CHECKV(g_sim_crit == 0, "critical section depth %d", g_sim_crit);
    for (int i = 0; i < g_nwk; i++) {
        wk_free(g_wk[i]);
    }
    g_nwk = 0;
}

static void check_no_violations(void)
{
    uint32_t v = sim_viol_total();
    if (v) {
        sim_viol_dump();
    }
    CHECKV(v == 0, "%u EHCI rule violations", v);
}

static void print_stats(const char *tag)
{
    struct ehci_nxp_stats *s = &H()->stats;
    printf("  %s: t=%.1fms uframes %lu packets %lu naks %lu irq %lu | drv submits %u compl %u err %u kills %u "
           "astop %u pstop %u hs_to %u xerr %u stall %u | iaa %lu ase %lu pse %lu links %lu\n",
           tag, sim_now_ns() / 1e6, (unsigned long)g_sim_stats.uframes, (unsigned long)g_sim_stats.packets,
           (unsigned long)g_sim_stats.naks, (unsigned long)g_sim_stats.irq_calls, s->submits, s->completions,
           s->errors, s->kills, s->async_stops, s->periodic_stops, s->handshake_timeouts, s->xacterr_restarts,
           s->stalls, (unsigned long)g_sim_stats.iaa_rings, (unsigned long)g_sim_stats.ase_toggles,
           (unsigned long)g_sim_stats.pse_toggles, (unsigned long)g_sim_stats.qh_link_events);
}

/* ================= tests ================= */

static void t_enumeration(void)
{
    bringup();
    enumerate(5);
    /* a STALLed vendor request, then the pipe works again */
    CHECK(ctrl(0x40, 0xFF, 0, 0, 0, 500) == -USB_ERR_STALL);
    CHECK(ctrl(0xC0, 0x01, 0, 0, 16, 500) == 16);
    CHECK(ctrl(0x80, 6, 0x0100, 0, 18, 500) == 18);
    /* transaction errors on EP0 (CERR retries, in-place restarts) */
    g_dev_cfg.noresp_permille = 300;
    for (int i = 0; i < 50; i++) {
        int n = ctrl(0x80, 6, 0x0200, 0, 9, 500);
        CHECKV(n == 9, "iteration %d: %d", i, n);
    }
    g_dev_cfg.noresp_permille = 0;
    CHECK(H()->stats.ep_updates >= 1); /* EP0 address changed in place */
    check_accounting("after enumeration");
    check_no_violations();
    print_stats("enumeration");
    close_all_and_check();
}

static struct stream s_out, s_in, s_mout, s_min, s_evt, s_aout, s_ain;
static struct wk w_out[2], w_in[4], w_mout, w_min[2], w_evt, w_aout, w_ain[2];

static void start_wifi(int n_in, int n_out)
{
    stream_init(&s_out, 0x01, 60, 1600);
    stream_init(&s_in, 0x81, 0, 0);
    stream_init(&s_mout, 0x02, 16, 300);
    stream_init(&s_min, 0x82, 0, 0);
    for (int i = 0; i < n_out; i++) {
        wk_init(&w_out[i], &g_ep_out_data, &s_out, 2048);
        w_out[i].want = 1;
        wk_register(&w_out[i]);
    }
    for (int i = 0; i < n_in; i++) {
        wk_init(&w_in[i], &g_ep_in_data, &s_in, 4096);
        w_in[i].want = 1;
        wk_register(&w_in[i]);
    }
    wk_init(&w_mout, &g_ep_out_msg, &s_mout, 512);
    w_mout.want = 1;
    wk_register(&w_mout);
    for (int i = 0; i < 2; i++) {
        wk_init(&w_min[i], &g_ep_in_msg, &s_min, 1024);
        w_min[i].want = 1;
        wk_register(&w_min[i]);
    }
}

static void check_wifi_integrity(void)
{
    CHECKV(g_dev_stats.out[1].bad == 0, "%lu corrupt OUT frames", (unsigned long)g_dev_stats.out[1].bad);
    CHECKV(g_dev_stats.out[1].dup_or_gap == 0, "%lu OUT order errors", (unsigned long)g_dev_stats.out[1].dup_or_gap);
    CHECKV(g_dev_stats.out[1].frames == s_out.frames, "device got %lu OUT frames, host completed %lu",
           (unsigned long)g_dev_stats.out[1].frames, (unsigned long)s_out.frames);
    CHECKV(s_in.bad == 0 && s_in.gap == 0, "IN frames: %lu corrupt %lu out of order", (unsigned long)s_in.bad,
           (unsigned long)s_in.gap);
    CHECKV(s_min.bad == 0 && s_min.gap == 0, "message IN: %lu corrupt %lu out of order", (unsigned long)s_min.bad,
           (unsigned long)s_min.gap);
    CHECKV(g_dev_stats.out[2].bad == 0 && g_dev_stats.out[2].dup_or_gap == 0, "message OUT %lu bad %lu order",
           (unsigned long)g_dev_stats.out[2].bad, (unsigned long)g_dev_stats.out[2].dup_or_gap);
    CHECKV(g_dev_stats.toggle_errors == 0, "%lu toggle errors", (unsigned long)g_dev_stats.toggle_errors);
}

/* 10k bulk OUT + 10k bulk IN: no doorbell, no schedule stop, every QH linked once */
static void hot_path(int stale, uint32_t nak, const char *tag)
{
    uint64_t iaa0, ase0, pse0, link0;
    uint32_t stops0, astops0;

    bringup();
    enumerate(1);
    g_sim_cfg.stale_cache = stale;
    g_dev_cfg.nak_permille = nak;
    start_wifi(4, 2);
    s_out.limit = 10000;
    w_mout.st->limit = 500;
    sim_dev_in_enqueue(0x81, 10000, 20, 1600);
    pump_us(20000); /* warm-up: every pipe opened */
    iaa0 = g_sim_stats.iaa_rings;
    ase0 = g_sim_stats.ase_toggles;
    pse0 = g_sim_stats.pse_toggles;
    link0 = g_sim_stats.qh_link_events;
    astops0 = H()->stats.async_stops;
    stops0 = H()->stats.periodic_stops;
    for (int i = 0; i < 4000 && (s_out.frames < 10000 || s_in.frames < 10000 || s_min.frames < 500); i++) {
        pump_us(1000);
    }
    CHECKV(s_out.frames == 10000, "OUT frames %lu", (unsigned long)s_out.frames);
    CHECKV(s_in.frames == 10000, "IN frames %lu", (unsigned long)s_in.frames);
    CHECKV(s_min.frames == 500, "message responses %lu", (unsigned long)s_min.frames);
    CHECKV(g_sim_stats.iaa_rings == iaa0, "%lu doorbells in the hot path", (unsigned long)(g_sim_stats.iaa_rings - iaa0));
    CHECKV(g_sim_stats.ase_toggles == ase0 && H()->stats.async_stops == astops0,
           "async schedule stopped %u times in the hot path", H()->stats.async_stops - astops0);
    CHECKV(g_sim_stats.pse_toggles == pse0 && H()->stats.periodic_stops == stops0, "periodic schedule toggled");
    CHECKV(g_sim_stats.qh_link_events == link0, "%lu QH (re)links in the hot path",
           (unsigned long)(g_sim_stats.qh_link_events - link0));
    CHECK(g_sim_stats.ring_len == 1 + 5); /* head + EP0 + 4 bulk pipes */
    check_wifi_integrity();
    check_accounting(tag);
    check_no_violations();
    print_stats(tag);
    close_all_and_check();
}

static void t_hot_path(void) { hot_path(0, 0, "hot path"); }

/*
 * A class driver (the AIC adapter re-applies its per-endpoint toggle before every submit) whose
 * urb->data_toggle lags or contradicts the QH overlay: the toggle of a linked pipe belongs to
 * the controller (DTC = 0), so no schedule stop, no toggle error, no data lost.
 */
static void t_toggle_stale_every_submit(void)
{
    uint32_t astops0, pstops0, ign0;

    bringup();
    enumerate(22);
    start_wifi(4, 2);
    s_out.limit = 3000;
    w_mout.st->limit = 300;
    sim_dev_in_enqueue(0x81, 3000, 20, 1600);
    pump_us(20000); /* every pipe opened with the right toggle */
    astops0 = H()->stats.async_stops;
    pstops0 = H()->stats.periodic_stops;
    ign0 = H()->stats.toggle_ignored;
    g_toggle_noise = 1;
    for (int i = 0; i < 3000 && (s_out.frames < 3000 || s_in.frames < 3000 || s_min.frames < 300); i++) {
        pump_us(1000);
    }
    g_toggle_noise = 0;
    CHECKV(s_out.frames == 3000 && s_in.frames == 3000 && s_min.frames == 300, "OUT %lu IN %lu msg %lu",
           (unsigned long)s_out.frames, (unsigned long)s_in.frames, (unsigned long)s_min.frames);
    CHECKV(H()->stats.async_stops == astops0, "%u async schedule stops from URB toggles (causes: toggle %u)",
           H()->stats.async_stops - astops0, H()->stats.async_stop_cause[EHCI_NXP_STOP_TOGGLE]);
    CHECK(H()->stats.periodic_stops == pstops0);
    CHECKV(H()->stats.toggle_ignored > ign0 + 10, "toggle_ignored %u", H()->stats.toggle_ignored - ign0);
    check_wifi_integrity();
    check_accounting("stale toggles");
    check_no_violations();
    print_stats("stale toggle on every submit");
    close_all_and_check();
}
static void t_hot_path_stale_nak(void) { hot_path(1, 250, "hot path, stale QH cache, 25% NAK"); }

/* several bulk IN URBs queued on one endpoint, NAKing device that answers late */
static void t_multi_in(void)
{
    bringup();
    enumerate(2);
    g_dev_cfg.nak_permille = 400;
    stream_init(&s_in, 0x81, 0, 0);
    for (int i = 0; i < 4; i++) {
        wk_init(&w_in[i], &g_ep_in_data, &s_in, 4096);
        w_in[i].want = 1;
        wk_register(&w_in[i]);
    }
    pump_us(5000);
    CHECK(pools_now().xfers == 4);
    CHECK(s_in.frames == 0);
    for (int round = 0; round < 50; round++) {
        sim_dev_in_enqueue(0x81, 1 + sim_rand() % 9, 1, 4096);
        pump_us(1000 + sim_rand() % 20000);
    }
    pump_us(50000);
    CHECKV(s_in.frames == sim_dev_in_next_seq(0x81), "host %lu frames, device sent %u", (unsigned long)s_in.frames,
           sim_dev_in_next_seq(0x81));
    CHECK(s_in.bad == 0 && s_in.gap == 0);
    CHECK(g_dev_stats.toggle_errors == 0);
    check_accounting("multi IN");
    check_no_violations();
    print_stats("multi IN");
    close_all_and_check();
}

/* kill one of several queued URBs while the HC holds a cached QH: no reuse while held */
static void kill_test(int stale, uint32_t lag)
{
    uint32_t okills = 0;

    bringup();
    enumerate(3);
    g_sim_cfg.stale_cache = stale;
    g_sim_cfg.as_lag_max_uf = lag;
    start_wifi(4, 2); /* OUT traffic keeps the async engine moving between QHs */
    pump_us(5000);
    for (int round = 0; round < 300; round++) {
        /* no IN data pending: every IN URB waits; kill one of them (middle, head or tail) */
        struct wk *w = &w_in[sim_rand() % 4];
        uint64_t done = w->done, f0 = s_in.frames;
        if (w->busy) {
            wk_kill(w);
            CHECK(!w->busy || w->want); /* resubmitted from its completion */
            CHECK(w->done == done + 1); /* completed exactly once (cancelled, or finished first) */
        }
        pump_us(200 + sim_rand() % 3000);
        if (round % 10 == 9) {
            sim_dev_in_enqueue(0x81, 8, 1, 3000);
            pump_us(20000);
            CHECKV(s_in.frames == f0 + 8, "round %d: %lu of 8 frames", round, (unsigned long)(s_in.frames - f0));
            if (s_in.frames != f0 + 8 && g_fail < 3) {
                struct usbh_ehci_diag d;
                for (int k = 0; k < 4; k++) {
                    if (w_in[k].busy) {
                        usbh_ehci_diag_capture(&w_in[k].urb, &d);
                        usbh_ehci_diag_print(&d, "stalled IN");
                        break;
                    }
                }
            }
        }
        /* an OUT URB queued behind the active one: kill it before it starts */
        if (round % 7 == 3 && w_out[0].busy && w_out[1].busy) {
            wk_kill(&w_out[1]);
            okills++;
        }
    }
    g_stop = 1;
    for (int i = 0; i < g_nwk; i++) {
        g_wk[i]->want = 0;
    }
    pump_us(20000);
    CHECK(s_in.bad == 0 && s_in.gap == 0);
    /* a killed OUT URB the controller had started leaves a partial frame (one corrupt frame) */
    CHECKV(g_dev_stats.out[1].bad <= okills, "%lu corrupt OUT frames, %u OUT kills",
           (unsigned long)g_dev_stats.out[1].bad, okills);
    CHECK(g_dev_stats.toggle_errors == 0);
    CHECK(H()->stats.kills > 0);
    check_accounting("kill");
    check_no_violations();
    print_stats(stale ? "kill, stale QH cache" : "kill");
    close_all_and_check();
}

static void t_kill(void) { kill_test(0, 0); }
static void t_kill_stale(void) { kill_test(1, 0); }
static void t_kill_stale_aslag_short(void) { kill_test(1, 24); }  /* AS follows within 3 ms */
static void t_kill_stale_aslag_long(void) { kill_test(1, 200); } /* AS lags up to 25 ms */

/* a transfer cancelled after some packets went: actual_length counts them (overlay progress) */
static void t_cancel_partial(void)
{
    uint32_t part;

    bringup();
    enumerate(13);
    /* OUT: 3000-byte frame, the device takes 2 packets then NAKs */
    stream_init(&s_out, 0x01, 3000, 3000);
    wk_init(&w_out[0], &g_ep_out_data, &s_out, 4096);
    wk_register(&w_out[0]);
    sim_dev_block(0x01, 2);
    wk_submit(&w_out[0]);
    pump_us(3000);
    part = sim_dev_out_partial(0x01);
    CHECK(part == 1024);
    CHECK(w_out[0].busy);
    wk_kill(&w_out[0]);
    CHECK(!w_out[0].busy && w_out[0].shut == 1);
    CHECKV(w_out[0].urb.actual_length == part,
           "cancelled OUT URB reports actual_length %u, the device received %u bytes",
           w_out[0].urb.actual_length, part);
    /* IN: a 2000-byte frame, the device sends 1 packet then NAKs */
    stream_init(&s_in, 0x81, 0, 0);
    wk_init(&w_in[0], &g_ep_in_data, &s_in, 4096);
    wk_register(&w_in[0]);
    sim_dev_block(0x81, 1);
    sim_dev_in_enqueue(0x81, 1, 2000, 2000);
    wk_submit(&w_in[0]);
    pump_us(3000);
    part = sim_dev_in_partial(0x81);
    CHECK(part == 512);
    wk_kill(&w_in[0]);
    CHECK(!w_in[0].busy && w_in[0].shut == 1);
    CHECKV(w_in[0].urb.actual_length == part,
           "cancelled IN URB reports actual_length %u, %u bytes were written to its buffer",
           w_in[0].urb.actual_length, part);
    CHECK(sim_frame_check(w_in[0].buf, 0, 0x81) < 0 || 1);
    sim_dev_block(0x01, -1);
    sim_dev_block(0x81, -1);
    check_accounting("cancel partial");
    check_no_violations();
    print_stats("cancel partial");
    close_all_and_check();
}

/*
 * Data that arrives while a cancel waits for the schedule to stop: the controller keeps running
 * until USBSTS.AS/PS reads 0, so the transfer may complete inside the handshake. It must then be
 * handed back as completed (or its data left with the device), never dropped.
 */
static void cancel_race(int periodic)
{
    struct wk *w = periodic ? &w_evt : &w_in[0];
    uint8_t ep = periodic ? 0x83 : 0x81;
    uint32_t lost = 0, raced = 0;

    bringup();
    enumerate(14);
    if (periodic) {
        stream_init(&s_evt, 0x83, 0, 0);
        wk_init(&w_evt, &g_ep_evt, &s_evt, 4096);
        w_evt.rx = NULL;
    } else {
        stream_init(&s_in, 0x81, 0, 0);
        wk_init(&w_in[0], &g_ep_in_data, &s_in, 4096);
    }
    wk_register(w);
    for (int i = 0; i < 64; i++) {
        uint64_t ok0 = w->ok;
        uint32_t sent0 = (uint32_t)g_dev_stats.in[ep & 15].frames;

        sim_dev_block(ep, 0);
        if (periodic) {
            uint8_t ev[6] = { 0x0E, 4, 1, (uint8_t)i, 0x0C, 0 };
            sim_dev_hci_event(ev, 6);
        } else {
            sim_dev_in_enqueue(ep, 1, 100, 400);
        }
        wk_submit(w);
        pump_us(125 * (uint64_t)(sim_rand() % 16)); /* any phase of the frame */
        sim_dev_block(ep, -1);                      /* the data is there now ... */
        wk_kill(w);                                 /* ... and the URB is cancelled */
        pump_us(3000);
        if (g_dev_stats.in[ep & 15].frames != sent0) {
            raced++;
            if (w->ok == ok0) {
                lost++;
            }
        }
        if (w->busy) {
            wk_kill(w);
        }
        while (sim_dev_in_pending(ep)) { /* drain what was not taken */
            w->want = 0;
            wk_submit(w);
            pump_us(3000);
            if (w->busy) {
                wk_kill(w);
            }
        }
    }
    printf("  %s: %u cancels raced a completion, %u of them lost the data\n", periodic ? "interrupt" : "bulk",
           raced, lost);
    CHECKV(lost == 0, "%u transfers the device completed were handed back as cancelled (data lost)", lost);
    check_accounting("cancel race");
    check_no_violations();
    close_all_and_check();
}

static void t_cancel_race_bulk(void) { cancel_race(0); }
static void t_cancel_race_int(void) { cancel_race(1); }

/* synchronous URBs (urb->timeout) that time out and are cancelled, under Wi-Fi traffic */
static void t_sync_timeout(void)
{
    static struct usbh_urb u;
    uint8_t *buf;
    uint32_t got = 0, expect = 0, tmo = 0;

    bringup();
    enumerate(4);
    g_sim_cfg.stale_cache = 1;
    start_wifi(2, 1);
    sim_dev_in_enqueue(0x81, 100000, 20, 1600);
    buf = sim_dma_alloc(4096, 32);
    for (int i = 0; i < 400; i++) {
        int rc;
        if (i % 5 == 4) {
            sim_dev_in_enqueue(0x84, 1, 10, 500); /* one ACL frame (one packet) now and then */
            expect++;
        }
        memset(&u, 0, sizeof(u));
        usbh_bulk_urb_fill(&u, &g_hp, &g_ep_acl_in, buf, 4096, 1 + sim_rand() % 30, NULL, NULL);
        u.data_toggle = s_ain.toggle;
        rc = usbh_submit_urb(&u);
        s_ain.toggle = u.data_toggle;
        if (rc == 0) {
            int64_t seq = sim_frame_check(buf, u.actual_length, 0x84);
            CHECKV(seq == got, "ACL frame seq %ld, expected %u", (long)seq, got);
            got++;
        } else {
            CHECKV(rc == -USB_ERR_TIMEOUT, "rc %d", rc);
            tmo++;
        }
        CHECK(u.hcpriv == NULL);
    }
    while (got < expect) {
        int rc;
        memset(&u, 0, sizeof(u));
        usbh_bulk_urb_fill(&u, &g_hp, &g_ep_acl_in, buf, 4096, 50, NULL, NULL);
        u.data_toggle = s_ain.toggle;
        rc = usbh_submit_urb(&u);
        s_ain.toggle = u.data_toggle;
        if (rc != 0) {
            break;
        }
        CHECK(sim_frame_check(buf, u.actual_length, 0x84) == got);
        got++;
    }
    CHECKV(got == expect, "ACL frames %u of %u", got, expect);
    CHECK(tmo > 100);
    check_wifi_integrity();
    check_accounting("sync timeout");
    check_no_violations();
    print_stats("sync timeout");
    sim_dma_free(buf);
    close_all_and_check();
}

/* STALL on bulk IN, CLEAR_FEATURE(ENDPOINT_HALT), data again with DATA0 */
static void t_stall_clear(void)
{
    bringup();
    enumerate(6);
    stream_init(&s_in, 0x81, 0, 0);
    wk_init(&w_in[0], &g_ep_in_data, &s_in, 4096);
    wk_register(&w_in[0]);
    sim_dev_in_enqueue(0x81, 3, 100, 1500);
    w_in[0].want = 1;
    pump_us(10000);
    CHECK(s_in.frames == 3);
    w_in[0].want = 0;
    sim_dev_halt_ep(0x81);
    wk_submit(&w_in[0]);
    pump_us(5000);
    CHECKV(w_in[0].last_err == -USB_ERR_STALL, "last error %d", w_in[0].last_err);
    CHECK(ctrl(0x02, 1, 0, 0x81, 0, 500) == 0); /* CLEAR_FEATURE(ENDPOINT_HALT) */
    s_in.toggle = 0;                            /* the class driver starts over with DATA0 */
    sim_dev_in_enqueue(0x81, 5, 100, 1500);
    w_in[0].want = 1;
    wk_submit(&w_in[0]);
    pump_us(20000);
    CHECKV(s_in.frames == 8, "frames %lu", (unsigned long)s_in.frames);
    CHECK(g_dev_stats.toggle_errors == 0);
    check_accounting("stall");
    check_no_violations();
    print_stats("stall/clear halt");
    close_all_and_check();
}

/* transaction errors: CERR exhaustion, in-place restarts, no data lost or duplicated */
static void t_xacterr(void)
{
    bringup();
    enumerate(7);
    g_dev_cfg.noresp_permille = 150;
    start_wifi(2, 1);
    s_out.limit = 2000;
    sim_dev_in_enqueue(0x81, 2000, 20, 1600);
    for (int i = 0; i < 3000 && (s_out.frames < 2000 || s_in.frames < 2000); i++) {
        pump_us(1000);
    }
    CHECKV(s_out.frames == 2000 && s_in.frames == 2000, "OUT %lu IN %lu", (unsigned long)s_out.frames,
           (unsigned long)s_in.frames);
    CHECK(H()->stats.xacterr_restarts > 0);
    check_wifi_integrity();
    check_accounting("xacterr");
    check_no_violations();
    print_stats("transaction errors");
    close_all_and_check();
}

/* the device disappears with URBs queued on every pipe; the pipes close and the pools refill */
static void t_disconnect(void)
{
    uint32_t pq[8];

    bringup();
    enumerate(8);
    g_sim_cfg.stale_cache = 1;
    start_wifi(4, 2);
    stream_init(&s_evt, 0x83, 0, 0);
    wk_init(&w_evt, &g_ep_evt, &s_evt, 4096);
    w_evt.rx = NULL;
    w_evt.want = 1;
    wk_register(&w_evt);
    sim_dev_in_enqueue(0x81, 500, 20, 1600);
    pump_us(30000);
    CHECK(sim_periodic_qhs(pq, 8) == 1);
    g_sim_hub_wakeups = 0;
    sim_port_disconnect();
    pump_us(20000);
    CHECK(g_sim_hub_wakeups > 0);
    CHECK(!(port_status() & (1U << HUB_PORT_FEATURE_CONNECTION)));
    check_accounting("after unplug, before release");
    close_all_and_check();
    CHECK(w_evt.shut + w_evt.err > 0);
    check_no_violations();
    print_stats("disconnect");
    /* plug in again: a fresh controller enumerates it */
    bringup();
    enumerate(9);
    check_no_violations();
    close_all_and_check();
}

/* ---------------- Bluetooth ---------------- */
static uint32_t g_cc_expected, g_cc_got, g_ncp_pkts, g_evt_bad;
static uint16_t g_cc_opcode[256];
static uint32_t g_cc_head, g_cc_tail;

static void rx_events(struct wk *w, int n)
{
    uint8_t *e = w->buf;
    sim_cache_invalidate((uintptr_t)w->buf, (size_t)n);
    if (n < 2 || e[1] + 2 != n) {
        g_evt_bad++;
        printf("  [host] bad HCI event of %d bytes (%02x %02x)\n", n, e[0], e[1]);
        return;
    }
    if (e[0] == 0x0E) {
        uint16_t op = (uint16_t)(e[3] | (e[4] << 8));
        if (g_cc_head == g_cc_tail || g_cc_opcode[g_cc_head % 256] != op) {
            g_evt_bad++;
            printf("  [host] Command Complete for %#x not expected\n", op);
        } else {
            g_cc_head++;
            g_cc_got++;
        }
    } else if (e[0] == 0x13) {
        g_ncp_pkts += (uint32_t)(e[5] | (e[6] << 8));
    } else {
        g_evt_bad++;
    }
}

static int hci_cmd(uint16_t op)
{
    int n;
    g_ctlbuf[0] = (uint8_t)op;
    g_ctlbuf[1] = (uint8_t)(op >> 8);
    g_ctlbuf[2] = 1;
    g_ctlbuf[3] = 0x5A;
    g_cc_opcode[g_cc_tail++ % 256] = op;
    g_cc_expected++;
    n = ctrl(USB_REQUEST_CLASS, 0, 0, 0, 4, 1000);
    return n;
}

static void t_bluetooth(void)
{
    uint32_t pq[8], pq0 = 0, acl_sent = 0;
    uint64_t pse0, link0;
    uint32_t pstops0;
    uint64_t next_acl;

    bringup();
    enumerate(10);
    g_sim_cfg.stale_cache = 1;
    g_dev_cfg.nak_permille = 100;
    g_cc_expected = g_cc_got = g_ncp_pkts = g_evt_bad = 0;
    g_cc_head = g_cc_tail = 0;
    /* Wi-Fi bulk traffic */
    start_wifi(4, 2);
    sim_dev_in_enqueue(0x81, 100000, 20, 1600);
    /* HCI event pipe (interrupt IN), ACL IN workers, ACL OUT */
    stream_init(&s_evt, 0x83, 0, 0);
    wk_init(&w_evt, &g_ep_evt, &s_evt, 4096);
    w_evt.rx = rx_events;
    w_evt.want = 1;
    wk_register(&w_evt);
    stream_init(&s_ain, 0x84, 0, 0);
    for (int i = 0; i < 2; i++) {
        wk_init(&w_ain[i], &g_ep_acl_in, &s_ain, 4096);
        w_ain[i].want = 1;
        wk_register(&w_ain[i]);
    }
    stream_init(&s_aout, 0x03, 672 + 4, 672 + 4);
    wk_init(&w_aout, &g_ep_acl_out, &s_aout, 1024);
    wk_register(&w_aout);
    wk_submit(&w_aout); /* every pipe open before the steady state */
    acl_sent++;
    pump_us(10000);
    CHECK(sim_periodic_qhs(pq, 8) == 1);
    pq0 = pq[0];
    pse0 = g_sim_stats.pse_toggles;
    pstops0 = H()->stats.periodic_stops;
    link0 = g_sim_stats.qh_link_events;
    next_acl = sim_now_ns() + 15000000ULL;
    /* 3 s: an HCI command every ~30 ms, a 672-byte ACL frame every 15 ms (~360 kbit/s), ACL IN */
    for (int ms = 0; ms < 3000; ms++) {
        if (ms % 30 == 7) {
            int n = hci_cmd((uint16_t)(0x0C03 + (ms & 0xFF)));
            CHECKV(n == 4, "HCI command transfer %d", n);
        }
        if (sim_now_ns() >= next_acl && !w_aout.busy) {
            wk_submit(&w_aout);
            acl_sent++;
            next_acl += 15000000ULL;
        }
        if (ms % 20 == 0) {
            sim_dev_in_enqueue(0x84, 1, 30, 1000);
        }
        pump_us(1000);
        if (ms % 10 == 0) {
            CHECKV(sim_periodic_qhs(pq, 8) == 1 && pq[0] == pq0, "interrupt QH left the frame list at %d ms", ms);
        }
    }
    pump_us(50000);
    CHECKV(g_cc_got == g_cc_expected, "Command Complete %u of %u", g_cc_got, g_cc_expected);
    CHECKV(g_ncp_pkts == (acl_sent / 4) * 4, "NCP events for %u packets, %u ACL frames sent", g_ncp_pkts, acl_sent);
    CHECK(g_evt_bad == 0);
    CHECKV(g_dev_stats.out[3].frames == acl_sent && g_dev_stats.out[3].bad == 0 && g_dev_stats.out[3].dup_or_gap == 0,
           "ACL OUT device %lu frames (%lu bad, %lu order) of %u", (unsigned long)g_dev_stats.out[3].frames,
           (unsigned long)g_dev_stats.out[3].bad, (unsigned long)g_dev_stats.out[3].dup_or_gap, acl_sent);
    CHECKV(s_ain.frames == sim_dev_in_next_seq(0x84) && s_ain.bad == 0 && s_ain.gap == 0, "ACL IN %lu of %u",
           (unsigned long)s_ain.frames, sim_dev_in_next_seq(0x84));
    CHECK(g_sim_stats.pse_toggles == pse0 && H()->stats.periodic_stops == pstops0);
    CHECK(g_sim_stats.qh_link_events == link0);
    CHECK(g_sim_stats.periodic_tx > 0);
    CHECK(s_out.frames > 1000 && s_in.frames > 1000);
    check_wifi_integrity();
    check_accounting("bluetooth");
    check_no_violations();
    print_stats("bluetooth + Wi-Fi");
    /* unplug: the interrupt pipe leaves the frame list, everything is handed back */
    sim_port_disconnect();
    pump_us(10000);
    close_all_and_check();
    CHECK(sim_periodic_qhs(pq, 8) == 0);
    check_no_violations();
}

/* the BT event worker's own pattern: interrupt URBs with a 1 s timeout cancelled while idle */
static void t_bt_event_timeouts(void)
{
    bringup();
    enumerate(11);
    g_sim_cfg.stale_cache = 1;
    g_cc_expected = g_cc_got = g_ncp_pkts = g_evt_bad = 0;
    g_cc_head = g_cc_tail = 0;
    start_wifi(2, 1);
    sim_dev_in_enqueue(0x81, 100000, 20, 1600);
    stream_init(&s_evt, 0x83, 0, 0);
    wk_init(&w_evt, &g_ep_evt, &s_evt, 4096);
    w_evt.rx = rx_events;
    w_evt.want = 1;
    w_evt.timeout_ms = 7; /* scaled down from 1000 ms: many cancels */
    wk_register(&w_evt);
    for (int i = 0; i < 300; i++) {
        if (i % 3 == 0) {
            CHECK(hci_cmd((uint16_t)(0x1000 + i)) == 4);
        }
        pump_us(1000 + sim_rand() % 9000);
    }
    pump_us(30000);
    CHECKV(w_evt.tmo > 50, "%lu timeouts", (unsigned long)w_evt.tmo);
    CHECKV(g_cc_got == g_cc_expected, "Command Complete %u of %u", g_cc_got, g_cc_expected);
    CHECK(g_evt_bad == 0);
    check_wifi_integrity();
    check_accounting("bt event timeouts");
    check_no_violations();
    print_stats("BT event timeouts");
    close_all_and_check();
}

/* everything at once with random NAKs, errors, late answers, kills and timeouts */
static void t_stress(void)
{
    bringup();
    enumerate(12);
    g_sim_cfg.stale_cache = 1;
    g_sim_cfg.irq_latency_uf = 2;
    g_dev_cfg.nak_permille = 300;
    g_dev_cfg.noresp_permille = 5;
    g_dev_cfg.msg_latency_uf = 64;
    g_cc_expected = g_cc_got = g_ncp_pkts = g_evt_bad = 0;
    g_cc_head = g_cc_tail = 0;
    start_wifi(4, 2);
    for (int i = 0; i < 4; i++) {
        w_in[i].timeout_ms = 3 + sim_rand() % 20;
    }
    stream_init(&s_evt, 0x83, 0, 0);
    wk_init(&w_evt, &g_ep_evt, &s_evt, 4096);
    w_evt.rx = rx_events;
    w_evt.want = 1;
    w_evt.timeout_ms = 11;
    wk_register(&w_evt);
    stream_init(&s_ain, 0x84, 0, 0);
    for (int i = 0; i < 2; i++) {
        wk_init(&w_ain[i], &g_ep_acl_in, &s_ain, 4096);
        w_ain[i].want = 1;
        wk_register(&w_ain[i]);
    }
    for (int ms = 0; ms < 4000; ms++) {
        if (sim_rand() % 8 == 0) {
            sim_dev_in_enqueue(0x81, 1 + sim_rand() % 4, 1, 1600);
        }
        if (ms % 50 == 0) {
            sim_dev_in_enqueue(0x84, 1, 10, 500);
        }
        if (ms % 97 == 0) {
            CHECK(hci_cmd((uint16_t)(0x2000 + ms)) == 4);
        }
        pump_us(1000);
    }
    g_dev_cfg.nak_permille = 0;
    g_dev_cfg.noresp_permille = 0;
    for (int i = 0; i < 4; i++) {
        w_in[i].timeout_ms = 0;
    }
    pump_us(100000);
    /* IN frames cut by a timeout kill are lost (host gap), never corrupt or duplicated */
    {
        uint64_t in_kills = 0;
        for (int i = 0; i < 4; i++) {
            in_kills += w_in[i].tmo;
        }
        CHECKV(s_in.bad <= in_kills, "%lu corrupt IN frames, %lu IN cancels", (unsigned long)s_in.bad,
               (unsigned long)in_kills);
    }
    CHECK(g_dev_stats.out[1].bad == 0 && g_dev_stats.out[1].dup_or_gap == 0);
    CHECK(g_dev_stats.toggle_errors == 0);
    CHECKV(g_cc_got == g_cc_expected, "Command Complete %u of %u", g_cc_got, g_cc_expected);
    check_accounting("stress");
    check_no_violations();
    print_stats("stress");
    printf("  stress: IN frames %lu (gaps from cancels %lu), OUT %lu, ACL IN %lu, kills %u\n",
           (unsigned long)s_in.frames, (unsigned long)s_in.gap, (unsigned long)s_out.frames,
           (unsigned long)s_ain.frames, H()->stats.kills);
    close_all_and_check();
}

/* ---------------- model self-tests: each breaks a rule on purpose, the model must see it ---------------- */
static struct ehci_nxp_pipe *pipe_of(uint8_t ep, uint8_t type)
{
    return ehci_nxp_pipe_find(B, &g_hp, ep, type);
}

static void selftest_setup_active_in(void)
{
    bringup();
    enumerate(20);
    stream_init(&s_in, 0x81, 0, 0);
    wk_init(&w_in[0], &g_ep_in_data, &s_in, 4096);
    wk_register(&w_in[0]);
    sim_dev_block(0x81, 1);
    sim_dev_in_enqueue(0x81, 1, 3000, 3000);
    wk_submit(&w_in[0]);
    pump_us(3000); /* one packet in, overlay active, NAKs from now on */
}

static void selftest_end(void)
{
    sim_viol_clear();
    g_stop = 1;
    w_in[0].want = 0;
    sim_dev_block(0x81, -1);
    close_all_and_check();
    sim_viol_clear();
}

static void t_selftest_overlay_touch(void)
{
    struct ehci_nxp_pipe *p;

    selftest_setup_active_in();
    p = pipe_of(0x81, USB_ENDPOINT_TYPE_BULK);
    CHECK(p && (p->qh->hw.overlay.token & QTD_TOKEN_STATUS_ACTIVE));
    p->qh->hw.overlay.token ^= QTD_TOKEN_TOGGLE; /* bad: live overlay edit */
    pump_us(500);
    CHECKV(g_viol[V_OVERLAY_TOUCH] > 0, "model missed a live overlay edit");
    selftest_end();
}

static void t_selftest_reuse_cached(void)
{
    struct ehci_nxp_pipe *p;
    uint32_t qa;
    int i;

    selftest_setup_active_in();
    g_sim_cfg.stale_cache = 1;
    g_sim_cfg.async_budget = 1; /* the engine parks on every QH in turn */
    p = pipe_of(0x81, USB_ENDPOINT_TYPE_BULK);
    qa = (uint32_t)(uintptr_t)&p->qh->hw;
    for (i = 0; i < 100 && sim_hc_current_qh() != qa; i++) {
        sim_advance_ns(125000);
    }
    CHECK(sim_hc_current_qh() == qa);
    /* bad: unlink without stopping the schedule or an IAA cycle, then reuse the QH at once */
    {
        struct ehci_nxp_qh *prev = H()->async_head;
        while ((prev->hw.hlp & ~0x1FU) != qa) {
            prev = (struct ehci_nxp_qh *)(uintptr_t)(prev->hw.hlp & ~0x1FU);
        }
        prev->hw.hlp = p->qh->hw.hlp;
        p->qh->hw.epchar = (p->qh->hw.epchar & ~QH_EPCHAR_ENDPT_MASK) | (2U << QH_EPCHAR_ENDPT_SHIFT);
    }
    pump_us(500);
    CHECKV(g_viol[V_QH_REUSE_CACHED] > 0, "model missed reuse of a QH the HC still held");
    selftest_end();
}

static void t_selftest_toggle_dma(void)
{
    bringup();
    enumerate(21);
    stream_init(&s_out, 0x01, 100, 100);
    wk_init(&w_out[0], &g_ep_out_data, &s_out, 1024);
    wk_register(&w_out[0]);
    wk_submit(&w_out[0]);
    pump_us(2000);
    CHECK(s_out.frames == 1 && s_out.toggle == 1);
    /* bad: the toggle of the idle QH changed behind the controller's back (DTC = 0) */
    {
        size_t f = usb_osal_enter_critical_section();
        struct ehci_nxp_pipe *p = ehci_nxp_pipe_find(B, &g_hp, 0x01, USB_ENDPOINT_TYPE_BULK);
        CHECK(p != NULL);
        if (p) {
            p->qh->hw.overlay.token ^= QTD_TOKEN_TOGGLE;
        }
        usb_osal_leave_critical_section(f);
    }
    wk_submit(&w_out[0]);
    pump_us(2000);
    CHECKV(g_viol[V_TOGGLE] > 0, "model missed a data toggle mismatch");
    sim_dev_block(0x01, 0);
    wk_submit(&w_out[0]);
    sim_cache_cpu_wrote(w_out[0].buf, 64); /* bad: CPU writes the buffer after it was cleaned */
    sim_dev_block(0x01, -1);
    pump_us(2000);
    CHECKV(g_viol[V_DMA_DIRTY] > 0, "model missed a DMA read of a dirty cache line");
    sim_viol_clear();
    close_all_and_check();
    sim_viol_clear();
}

static void t_kill_aslag_long(void) { kill_test(0, 200); }

/* ================= runner ================= */
static const struct {
    const char *name;
    void (*fn)(void);
} k_tests[] = {
    { "selftest_overlay_touch", t_selftest_overlay_touch },
    { "selftest_reuse_cached", t_selftest_reuse_cached },
    { "selftest_toggle_dma", t_selftest_toggle_dma },
    { "enumeration", t_enumeration },
    { "hot_path", t_hot_path },
    { "toggle_stale_every_submit", t_toggle_stale_every_submit },
    { "hot_path_stale_nak", t_hot_path_stale_nak },
    { "multi_in", t_multi_in },
    { "kill", t_kill },
    { "kill_stale", t_kill_stale },
    { "kill_stale_aslag_short", t_kill_stale_aslag_short },
    { "kill_stale_aslag_long", t_kill_stale_aslag_long },
    { "kill_aslag_long", t_kill_aslag_long },
    { "cancel_partial", t_cancel_partial },
    { "cancel_race_bulk", t_cancel_race_bulk },
    { "cancel_race_int", t_cancel_race_int },
    { "sync_timeout", t_sync_timeout },
    { "stall_clear", t_stall_clear },
    { "xacterr", t_xacterr },
    { "disconnect", t_disconnect },
    { "bluetooth", t_bluetooth },
    { "bt_event_timeouts", t_bt_event_timeouts },
    { "stress", t_stress },
};

static void on_alarm(int sig)
{
    (void)sig;
    static const char m[] = "test watchdog: no progress (driver spinning?)\n";
    (void)!write(2, m, sizeof(m) - 1);
    _exit(4);
}

int main(int argc, char **argv)
{
    int failed = 0, ran = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    sim_init();
    signal(SIGALRM, on_alarm);
    for (unsigned i = 0; i < sizeof(k_tests) / sizeof(k_tests[0]); i++) {
        int sel = argc < 2;
        for (int a = 1; a < argc; a++) {
            if (strstr(k_tests[i].name, argv[a])) {
                sel = 1;
            }
        }
        if (!sel) {
            continue;
        }
        g_test = k_tests[i].name;
        g_fail = 0;
        printf("== %s\n", g_test);
        alarm(300);
        k_tests[i].fn();
        alarm(0);
        printf("%s %s\n", g_fail ? "FAIL" : "PASS", g_test);
        failed += g_fail != 0;
        ran++;
    }
    printf("%d/%d tests passed\n", ran - failed, ran);
    return failed ? 1 : 0;
}
