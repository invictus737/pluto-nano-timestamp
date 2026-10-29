/*
 * EHCI host controller driver for CherryUSB on the NXP MCUXpresso pipe model
 * (CHERRY_HCD=nxp, rtos/cherryusb/cherryusb.mk). This header replaces CherryUSB's
 * port/ehci/usb_hc_ehci.h on the include path: it keeps every declaration the Nexus glue
 * (port/usb_glue_zynq.c) and the AIC8800 adapter (aic/aicwf_cherry.c) use from the patched
 * CherryUSB EHCI port, and adds the internal API of usb_hc_ehci_nxp.c for host tests.
 *
 * The controller model follows NXP host/usb_host_ehci.c (mcux-sdk-middleware-usb, BSD-3-Clause,
 * Copyright (c) 2015 - 2016, Freescale Semiconductor, Inc., Copyright 2016,2019 - 2020 NXP):
 * see usb_hc_ehci_nxp.c for the license text and what was taken from it.
 *
 * ---- internal API (for the register/DMA model of test/ehci_nxp) ----
 *
 * Objects (all per bus, in g_ehci_nxp[busid]):
 *   struct ehci_nxp_qh    qh_pool[CONFIG_USB_EHCI_QH_NUM]     DMA (noncache), 64 bytes, 32-aligned
 *   struct ehci_nxp_qtd   qtd_pool[CONFIG_USB_EHCI_QTD_NUM]   DMA (noncache), 64 bytes, 32-aligned
 *   struct ehci_nxp_qh    async_head                          DMA: the H-bit head of the async ring
 *   uint32_t              framelist[FRAME_LIST_SIZE]          DMA: periodic frame list (T or QH links)
 *   struct ehci_nxp_pipe  pipes[CONFIG_USB_EHCI_QH_NUM]       pipe i owns qh_pool[i]
 *   struct ehci_nxp_xfer  xfers[CONFIG_USB_EHCI_NXP_XFER_NUM] one per URB in the controller
 *
 * Pipe = (hport, endpoint address, type). Opened at the first usbh_submit_urb() for it, its QH
 * is linked once (async ring right after the head, or every period-th frame list entry) and
 * stays linked until the pipe is closed: when the device goes away (usbh_kill_urb() on a
 * disconnected hport, i.e. usbh_hubport_release()), on usb_hc_deinit(), or on a periodic pipe
 * whose endpoint characteristics changed while idle. Each QH ends with an inactive dummy qTD
 * (pipe->dummy, overlay.next points at it while idle); a URB is appended by filling the dummy
 * as its first qTD, chaining new qTDs plus a new dummy behind it and activating the old dummy's
 * token last (after a DMA barrier). Data toggle lives in the QH overlay (DTC = 0, except
 * control: DTC = 1, toggle from the qTDs). URBs complete strictly in order per pipe.
 *
 * Cancel (usbh_kill_urb, timeout) and close never use the IAA doorbell: they stop the schedule
 * with the NXP handshake (wait USBSTS.AS == USBCMD.ASE, clear ASE, wait AS = 0; the same with
 * PSE/PS for periodic pipes), edit the qTD chain / overlay or unlink the QH, write
 * ASYNCLISTADDR = async_head, set ASE and wait AS = 1 (USB_HostEhciStopAsync/StartAsync).
 *
 * Entry points: usb_hc_init / usb_hc_deinit / usbh_roothub_control / usbh_submit_urb /
 * usbh_kill_urb / usbh_get_frame_number / USBH_IRQHandler (CherryUSB usb_hc.h), all called
 * with or without the OSAL critical section (it is recursive); USBH_IRQHandler is called by the
 * glue's "usbh_irq" thread inside it. Completions run inside the critical section.
 *
 * Register access: every controller register goes through EHCI_NXP_REG_READ/EHCI_NXP_REG_WRITE
 * (address = reg_base + CONFIG_USB_EHCI_HCCR_OFFSET [+ CAPLENGTH] + offset below), so a test
 * defines both before including this header (e.g. with -include) to route them to a model.
 * Time: usb_ehci_time_us() (glue). DMA barrier before the controller may see a descriptor
 * change: CONFIG_USB_EHCI_DMA_WMB() (DSB + L2C-310 sync, osal usb_osal_dma_barrier()).
 * Buffers: usb_dcache_flush() of setup/transfer buffer before linking (as the old port).
 * Addresses: usb_ramaddr2phyaddr()/usb_phyaddr2ramaddr() (identity on RTEMS); 32-bit DMA.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _USB_EHCI_PRIV_H
#define _USB_EHCI_PRIV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "usbh_core.h"
#include "usbh_hub.h"
#include "usb_ehci_reg.h"

#define USB_HC_EHCI_NXP 1

#ifndef EHCI_NXP_REG_READ
#define EHCI_NXP_REG_READ(addr)       (*(volatile uint32_t *)(uintptr_t)(addr))
#endif
#ifndef EHCI_NXP_REG_WRITE
#define EHCI_NXP_REG_WRITE(addr, val) (*(volatile uint32_t *)(uintptr_t)(addr) = (uint32_t)(val))
#endif

/* capability registers (from reg_base + CONFIG_USB_EHCI_HCCR_OFFSET) */
#define EHCI_NXP_CAPLENGTH   0x00U /* low byte; HCIVERSION in 31:16 */
#define EHCI_NXP_HCSPARAMS   0x04U
#define EHCI_NXP_HCCPARAMS   0x08U
/* operational registers (from the capability base + CAPLENGTH) */
#define EHCI_NXP_USBCMD      0x00U
#define EHCI_NXP_USBSTS      0x04U
#define EHCI_NXP_USBINTR     0x08U
#define EHCI_NXP_FRINDEX     0x0cU
#define EHCI_NXP_PERIODICLIST 0x14U
#define EHCI_NXP_ASYNCLIST   0x18U
#define EHCI_NXP_PORTSC(n)   (0x44U + 4U * (uint32_t)(n)) /* n = 0 .. */
/* PORTSC write-1-to-clear bits, never written back by a read-modify-write (NXP EHCI_PORTSC1_W1_BITS) */
#define EHCI_NXP_PORTSC_W1C  (EHCI_PORTSC_CSC | EHCI_PORTSC_PEC | EHCI_PORTSC_OCC)

#ifndef CONFIG_USB_EHCI_QH_NUM
#define CONFIG_USB_EHCI_QH_NUM 16
#endif
#ifndef CONFIG_USB_EHCI_QTD_NUM
#define CONFIG_USB_EHCI_QTD_NUM (CONFIG_USB_EHCI_QH_NUM * 4)
#endif
#ifndef CONFIG_USB_EHCI_NXP_XFER_NUM
#define CONFIG_USB_EHCI_NXP_XFER_NUM 64
#endif
/*
 * Schedule handshake bound (NXP waits forever). A miss is counted and nothing is edited: the
 * cancel or close is deferred and retried by ehci_nxp_service() (interrupt handler and the
 * glue's poll through usbh_ehci_async_watchdog()), each retry waiting at most
 * CONFIG_USB_EHCI_NXP_RETRY_US.
 */
#ifndef CONFIG_USB_EHCI_NXP_HANDSHAKE_US
#define CONFIG_USB_EHCI_NXP_HANDSHAKE_US 100000
#endif
#ifndef CONFIG_USB_EHCI_NXP_RETRY_US
#define CONFIG_USB_EHCI_NXP_RETRY_US 1000
#endif
/* a synchronous URB whose cancel is deferred: its submitter waits this long, then abandons it */
#ifndef CONFIG_USB_EHCI_NXP_ORPHAN_MS
#define CONFIG_USB_EHCI_NXP_ORPHAN_MS 5000
#endif

struct ehci_nxp_pipe;
struct ehci_nxp_xfer;

struct ehci_nxp_qtd {
    struct ehci_qtd hw;           /* 32 bytes, read and written by the controller */
    struct ehci_nxp_qtd *sw_next; /* free list link */
    uint32_t len;                 /* bytes queued in this qTD */
    uint8_t inuse;
    uint8_t stage;                /* EHCI_NXP_STAGE_* */
} __attribute__((aligned(32)));

#define EHCI_NXP_STAGE_DATA   0
#define EHCI_NXP_STAGE_SETUP  1
#define EHCI_NXP_STAGE_STATUS 2

struct ehci_nxp_qh {
    struct ehci_qh hw;            /* 48 bytes, read and written by the controller */
    struct ehci_nxp_pipe *pipe;   /* NULL: the async head */
} __attribute__((aligned(32)));

struct ehci_nxp_xfer {
    struct usbh_urb *urb;
    struct ehci_nxp_pipe *pipe;
    struct ehci_nxp_xfer *next;   /* pipe queue, in hardware order */
    struct ehci_nxp_qtd *first;   /* the former dummy */
    struct ehci_nxp_qtd *last;    /* IOC; last->hw.next_qtd = the next URB's first qTD or the dummy */
    usb_osal_sem_t sem;           /* synchronous URBs (timeout != 0) */
    uint8_t inuse;
    uint8_t sync;
    uint8_t done;                 /* synchronous: completed, waiting for its submitter */
    uint8_t nqtd;
    uint8_t in;                   /* IN endpoint (data stage IN for control) */
    uint8_t cancel_pending;       /* cancel deferred: schedule handshake missed */
    uint8_t orphan;               /* synchronous submitter gave up: no URB to hand back */
    uint32_t seq;                 /* incremented at every allocation */
};

struct ehci_nxp_pipe {
    uint8_t inuse;
    uint8_t type;                 /* USB_ENDPOINT_TYPE_* */
    uint8_t ep_addr;              /* bEndpointAddress (control: 0) */
    uint8_t periodic;             /* on the frame list (interrupt), else on the async ring */
    uint8_t linked;
    uint8_t closing;
    uint8_t xacterrs;             /* in-place restarts of the current qTD (Linux QH_XACTERR_MAX) */
    uint8_t reset_toggle;         /* CLEAR_FEATURE(ENDPOINT_HALT) seen while busy: DATA0 when idle */
    uint8_t close_pending;        /* close deferred: schedule handshake missed */
    int close_result;
    uint16_t period;              /* frames between two frame list entries (periodic) */
    uint16_t uframe_interval;     /* NXP uframeInterval: frame list order */
    uint32_t epchar, epcap;
    struct usbh_hubport *hport;
    struct ehci_nxp_qh *qh;
    struct ehci_nxp_qtd *dummy;
    struct ehci_nxp_xfer *head, *tail;
    uint32_t gen;                 /* incremented at every open */
};

struct ehci_nxp_stats {
    uint32_t pipes_opened, pipes_closed;
    uint32_t submits, completions, errors, kills;
    uint32_t async_stops, periodic_stops;
    uint32_t handshake_timeouts;  /* AS/PS did not follow ASE/PSE within CONFIG_USB_EHCI_NXP_HANDSHAKE_US */
    uint32_t xacterr_restarts, stalls, halted_restarts;
    uint32_t no_qtd, no_xfer, no_pipe;
    uint32_t ep_updates;          /* endpoint characteristics rewritten in place (EP0 address/mps) */
    uint32_t toggle_sets;         /* idle pipe toggle changed (URB data_toggle, CLEAR_FEATURE(HALT)) */
    uint32_t deferred;            /* cancels/closes deferred by a missed handshake */
    uint32_t cancel_races;        /* cancelled transfers the controller finished before it stopped */
    uint32_t orphans;             /* synchronous transfers abandoned by their submitter */
    uint32_t max_handshake_us;
};

struct ehci_nxp_hcd {
    struct ehci_nxp_qh *qh_pool;      /* -> g_ehci_nxp_qh[busid] */
    struct ehci_nxp_qtd *qtd_pool;
    struct ehci_nxp_qh *async_head;
    uint32_t *framelist;
    struct ehci_nxp_qtd *qtd_free;
    uint32_t qtd_free_count;
    struct ehci_nxp_pipe pipes[CONFIG_USB_EHCI_QH_NUM];
    struct ehci_nxp_xfer xfers[CONFIG_USB_EHCI_NXP_XFER_NUM];
    uint32_t hcor_offset;
    uint8_t n_ports;
    bool ppc;
    bool running;
    uint8_t sched_depth[2];       /* nested stops of the async [0] / periodic [1] schedule */
    uint8_t sched_stuck[2];       /* enable bit cleared, status not yet 0 */
    uint8_t pending;              /* deferred work for ehci_nxp_service() */
    uint32_t xfer_seq;
    struct ehci_nxp_stats stats;
};

extern struct ehci_nxp_hcd g_ehci_nxp[CONFIG_USBHOST_MAX_BUS];
extern struct ehci_nxp_qh g_ehci_nxp_qh[CONFIG_USBHOST_MAX_BUS][CONFIG_USB_EHCI_QH_NUM];
extern struct ehci_nxp_qtd g_ehci_nxp_qtd[CONFIG_USBHOST_MAX_BUS][CONFIG_USB_EHCI_QTD_NUM];
extern struct ehci_nxp_qh g_ehci_nxp_async_head[CONFIG_USBHOST_MAX_BUS];
extern uint32_t g_framelist[CONFIG_USBHOST_MAX_BUS][CONFIG_USB_EHCI_FRAME_LIST_SIZE];

/* internal helpers exported for tests (critical section held) */
struct ehci_nxp_pipe *ehci_nxp_pipe_find(struct usbh_bus *bus, struct usbh_hubport *hport, uint8_t ep_addr, uint8_t type);
void ehci_nxp_close_hport(struct usbh_bus *bus, struct usbh_hubport *hport);
void ehci_nxp_scan(struct usbh_bus *bus);
/* deferred cancels/closes and a stuck schedule stop; critical section held */
void ehci_nxp_service(struct usbh_bus *bus);

/* glue (port/usb_glue_zynq.c) */
extern uint8_t usbh_get_port_speed(struct usbh_bus *bus, const uint8_t port);
void usb_hc_low_level_init(struct usbh_bus *bus);
void usb_hc_low_level2_init(struct usbh_bus *bus);
void usb_hc_low_level_deinit(struct usbh_bus *bus);
/* microsecond clock (wraps), provided by the port glue */
uint32_t usb_ehci_time_us(void);

/* ---- compatibility with the patched CherryUSB EHCI port (glue and AIC adapter) ---- */

/* IAA statistics of the old port: this driver rings no doorbell, so they stay zero except the
 * completion count, HSE (fatal) and, for the glue's log, the handshake misses (no_handshake) */
struct usbh_ehci_iaa_stats {
    uint32_t cycles, irq_ends, polled_ends, watchdog_ends, lost, late, spurious, stale_iaa;
    uint32_t iaad_busy, quarantined, released, stuck_ends;
    uint32_t stream_creates, stream_appends, stream_teardowns;
    uint32_t not_running, kill_waits, fatal, max_cycle_us;
    uint32_t last_cmd, last_sts, last_async;
    uint32_t ok_count;
    uint32_t ok_snap[9];
    uint32_t async_kicks, async_kick_unfetched, async_kick_iaad, async_kick_no_handshake;
    uint32_t kick_cmd, kick_sts, kick_async;
};
extern struct usbh_ehci_iaa_stats g_ehci_iaa_stats[CONFIG_USBHOST_MAX_BUS];
uint32_t usbh_ehci_iaa_pending(uint8_t busid);   /* 1 while deferred work waits (glue polls every 2 ms) */
uint32_t usbh_ehci_iaa_watchdog(uint8_t busid);  /* always 0 */
uint32_t usbh_ehci_async_watchdog(uint8_t busid); /* runs ehci_nxp_service(), 1 if work was pending */
extern volatile uint32_t g_ehci_xacterr_retries;

/*
 * URB flags of the old port. Every bulk/interrupt URB is queued on its endpoint's pipe here,
 * so USBH_EHCI_URB_STREAM and USBH_EHCI_URB_APPEND_ONLY only keep their length limit check.
 */
#define USBH_EHCI_URB_STREAM      (1 << 28)
#define USBH_EHCI_URB_APPEND_ONLY (1 << 29)
#define USBH_EHCI_STREAM_MAX_LEN  0x4000U

/* the OSAL critical section, released at every nesting level and taken back (no sleep in it) */
size_t usb_osal_crit_suspend(void);
void usb_osal_crit_resume(size_t depth);

/* state of an URB that timed out (-USB_ERR_TIMEOUT), captured under the lock */
#define USBH_EHCI_DIAG_QTDS 8
struct usbh_ehci_diag {
    uint32_t t_us;
    int qh_index;           /* -1: no pipe */
    uint32_t hlp, epchar, epcap, curr, next, alt, token;
    uint8_t iaa_state, stream, stream_open, quarantine;
    uint8_t nqtd, on_ring;
    uint16_t ring_len;
    uint32_t qtd_addr[USBH_EHCI_DIAG_QTDS], qtd_token[USBH_EHCI_DIAG_QTDS];
    uint32_t usbcmd, usbsts, async, frindex0, frindex1, portsc;
    uint32_t pending, in_progress, cycle_us;
    uint32_t delta[9];
    uint32_t ok_count;
};
void usbh_ehci_diag_capture(struct usbh_urb *urb, struct usbh_ehci_diag *d);
void usbh_ehci_diag_print(const struct usbh_ehci_diag *d, const char *tag);
int usbh_ehci_diag_allowed(void);

#endif
