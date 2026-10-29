/*
 * CherryUSB host API for the RT-Smart AIC8800 driver (USB_STACK=cherry): replaces the libbsd
 * usbh shim (rtos/libbsd/aic8800/shim/usbh_shim.c) and keeps the semantics the driver was
 * written for on the K230 DWC2 port.
 *
 *  - One CherryUSB class driver ("aic8800") takes every interface; connect() matches the
 *    driver's own K230-layout class tables (as the shim did) and runs the matching RT-Smart
 *    connect() on the hub thread, then brings the station up (aicwf_wlan_attach_done()).
 *    Storage-mode sticks go to aicwf_cherry_modeswitch.c.
 *  - URBs: every endpoint has a software queue. With NX_IN_URBS_PER_EP > 1 (usb_config.h,
 *    default 1) bulk IN URBs of at most 16 KiB stay armed in the controller as Linux keeps
 *    them (aicwf_usb.c: 20 data + 100 message RX URBs): up to NX_IN_URBS_PER_EP per endpoint,
 *    chained on one EHCI stream QH (USBH_EHCI_URB_STREAM, the
 *    data toggle stays in its overlay). Separate QHs per URB cannot do this safely: each would
 *    carry its own toggle and the device's next packet would be discarded as a repeat by the
 *    other one. Any other transfer has one URB in the controller per endpoint; the next one is
 *    started from the completion (still in the interrupt handler) with the toggle carried over.
 *  - Completions run on the "aic-usbc" giveback thread, never in the interrupt handler and
 *    never in the submitting thread; usbh_kill_urb() returns after the completion ran, as the
 *    shim's; synchronous URBs (timeout != 0) wait in the caller.
 *  - URB_ZERO_PACKET: an OUT transfer that is a non-zero multiple of wMaxPacketSize is
 *    followed by a zero-length packet before it completes.
 *  - Cache: buffers that are not cache-line aligned (or IN buffers whose length is not a
 *    multiple of the line) go through a non-cacheable bounce buffer; IN buffers used in place
 *    are invalidated after the transfer (the EHCI port only cleans before it).
 *
 * Locking: all state is under the OSAL critical section (a recursive mutex also held by the
 * interrupt handler); driver callbacks always run without it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define AICWF_CHERRY_ADAPTER

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rtems.h>
#include <rtems/thread.h>

#include <rtthread.h>
#include <rthw.h>
#include <usbh_core.h>

#include "aic8800_wifi.h"
#include "aicwf_shim.h"

#include "nexus_cherryusb.h"
#include "usb_hc_ehci.h" /* stream URBs, timeout diagnostics */

#define DBG_TAG "aicwf.usb"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* CherryUSB core (not in its headers) */
int usbh_enumerate(struct usbh_hubport *hport);
void usbh_hubport_release(struct usbh_hubport *hport);

/* aicwf_cherry_modeswitch.c */
int aicwf_cherry_is_storage_mode(uint16_t vid, uint16_t pid);
int aicwf_cherry_modeswitch_connect(struct usbh_hubport *hport, uint8_t intf);

#define NX_MAX_REQ   192
#define NX_MAX_EP    32
#define NX_MAX_DEV   4
#define NX_LINE      32U
#define NX_SYNC_GRACE_MS 2000
#define NX_START_WAIT_MS 60000
/* bulk IN URBs kept in the controller per endpoint (usb_config.h; > 1: on an EHCI stream QH) */
#ifndef NX_IN_URBS_PER_EP
#define NX_IN_URBS_PER_EP 1
#endif
#if NX_IN_URBS_PER_EP < 1
#error NX_IN_URBS_PER_EP must be at least 1
#endif

/* ---- the driver's class tables (K230 layout) ---- */

extern const struct aicwf_k230_class_info g_aic8801_class_info, g_aic8800_boot_class_info,
    g_aic8800dc_class_info, g_aic8800dw_class_info, g_aic8800d81_class_info,
    g_aic8800d80_boot_class_info, g_aic8800d83_class_info, g_aic8800d84_class_info,
    g_aic8800d85_class_info, g_aic8800d86_class_info, g_aic8800d88_class_info,
    g_aic8800d81_v2_class_info, g_aic8800d83_v2_class_info, g_aic8800d84_v2_class_info,
    g_aic8800d85_v2_class_info, g_aic8800d86_v2_class_info, g_aic8800d88_v2_class_info,
    g_aic8800d41_class_info, g_aic8800d40_boot_class_info, g_aic8800d81x2_class_info,
    g_aic8800d89x2_class_info, g_aic8800d80x2_boot_class_info;
#ifdef AIC8800_WIFI_BLE
extern const struct aicwf_k230_class_info g_aic_btusb_8801, g_aic_btusb_88dc, g_aic_btusb_8d81,
    g_aic_btusb_8d41, g_aic_btusb_8d99, g_aic_btusb_88dd, g_aic_btusb_8d91;
#endif

static const struct aicwf_k230_class_info *const g_class_table[] = {
    &g_aic8801_class_info, &g_aic8800_boot_class_info, &g_aic8800dc_class_info,
    &g_aic8800dw_class_info, &g_aic8800d81_class_info, &g_aic8800d80_boot_class_info,
    &g_aic8800d83_class_info, &g_aic8800d84_class_info, &g_aic8800d85_class_info,
    &g_aic8800d86_class_info, &g_aic8800d88_class_info, &g_aic8800d81_v2_class_info,
    &g_aic8800d83_v2_class_info, &g_aic8800d84_v2_class_info, &g_aic8800d85_v2_class_info,
    &g_aic8800d86_v2_class_info, &g_aic8800d88_v2_class_info, &g_aic8800d41_class_info,
    &g_aic8800d40_boot_class_info, &g_aic8800d81x2_class_info, &g_aic8800d89x2_class_info,
    &g_aic8800d80x2_boot_class_info,
#ifdef AIC8800_WIFI_BLE
    &g_aic_btusb_8801, &g_aic_btusb_88dc, &g_aic_btusb_8d81, &g_aic_btusb_8d41,
    &g_aic_btusb_8d99, &g_aic_btusb_88dd, &g_aic_btusb_8d91,
#endif
};

/* ---- request / endpoint state ---- */

enum { REQ_FREE, REQ_QUEUED, REQ_HW, REQ_ZLP, REQ_DONE, REQ_DELIVERING };

struct nx_ep;

struct nx_req {
    struct usbh_urb *urb;
    struct nx_ep *ep;
    struct nx_req *next;
    usbh_complete_callback_t complete;
    void *arg;
    uint8_t *buf;
    uint8_t *bounce;
    uint32_t len;
    uint32_t actual;
    int result;
    uint8_t state;
    uint8_t zlp;
    uint8_t is_in;
    uint8_t in_hw;    /* counted in ep->inflight */
    uint8_t stream;   /* on the endpoint's EHCI stream QH */
    uint8_t killing;  /* aicwf_cherry_kill_urb() cancels it */
    int flags;        /* the driver's transfer_flags */
    rtems_binary_semaphore *waiter; /* synchronous submit */
};

struct nx_ep {
    struct usbh_hubport *hport;     /* NULL: free */
    uint8_t addr;
    uint8_t toggle;
    uint8_t streaming;  /* the requests in the controller are on a stream QH */
    uint16_t inflight;  /* requests in the controller */
    uint16_t max_inflight;
    unsigned timeouts;  /* synchronous transfers that timed out */
    struct nx_req *head, *tail;
    struct usbh_urb zlp_urb;
};

struct nx_dev {
    struct usbh_hubport *hport;     /* NULL: free */
    uint16_t vid, pid;
    const struct aicwf_k230_class_info *cls[CONFIG_USBHOST_MAX_INTERFACES];
    uint8_t connected[CONFIG_USBHOST_MAX_INTERFACES];
    volatile uint8_t reenumerating;
};

static struct nx_req g_req[NX_MAX_REQ];
static struct nx_ep g_ep[NX_MAX_EP];
static struct nx_dev g_dev[NX_MAX_DEV];
static struct nx_req *g_done_head, *g_done_tail;
static rtems_counting_semaphore g_done_sem = RTEMS_COUNTING_SEMAPHORE_INITIALIZER("aicgb", 0);
static struct usbh_urb *volatile g_delivering;
static pthread_t g_gb_tid;
static volatile int g_gb_running;

static volatile int g_started;
static pthread_mutex_t g_start_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned g_attach_count;

/* counters (`usbh` command) */
static unsigned g_st_submit, g_st_complete, g_st_bounce, g_st_zlp, g_st_kill, g_st_noreq,
    g_st_maxq, g_st_reenum, g_st_requeue, g_st_append_busy, g_st_kill_busy;

#define CRIT_ENTER() size_t crit_flags_ = usb_osal_enter_critical_section()
#define CRIT_LEAVE() usb_osal_leave_critical_section(crit_flags_)

/* ---- helpers (critical section held) ---- */

static struct nx_req *req_alloc(void)
{
    int i;

    for (i = 0; i < NX_MAX_REQ; i++) {
        if (g_req[i].state == REQ_FREE) {
            memset(&g_req[i], 0, sizeof(g_req[i]));
            g_req[i].state = REQ_QUEUED;
            return &g_req[i];
        }
    }
    return NULL;
}

static struct nx_req *req_find(struct usbh_urb *urb)
{
    int i;

    for (i = 0; i < NX_MAX_REQ; i++) {
        if (g_req[i].state != REQ_FREE && g_req[i].urb == urb) {
            return &g_req[i];
        }
    }
    return NULL;
}

static struct nx_ep *ep_get(struct usbh_hubport *hport, uint8_t addr, int create)
{
    struct nx_ep *free_ep = NULL;
    int i;

    for (i = 0; i < NX_MAX_EP; i++) {
        if (g_ep[i].hport == hport && g_ep[i].addr == addr) {
            return &g_ep[i];
        }
        if (g_ep[i].hport == NULL && free_ep == NULL) {
            free_ep = &g_ep[i];
        }
    }
    if (!create || free_ep == NULL) {
        return NULL;
    }
    memset(free_ep, 0, sizeof(*free_ep));
    free_ep->hport = hport;
    free_ep->addr = addr;
    return free_ep;
}

/* forget the endpoints of a device that left or arrived (none may be busy) */
static void ep_purge(struct usbh_hubport *hport)
{
    int i;

    for (i = 0; i < NX_MAX_EP; i++) {
        if (g_ep[i].hport == hport && g_ep[i].inflight == 0 && g_ep[i].head == NULL) {
            g_ep[i].hport = NULL;
        }
    }
}

static void done_push(struct nx_req *req)
{
    if (req->waiter) {
        req->state = REQ_DONE;
        rtems_binary_semaphore_post(req->waiter);
        return;
    }
    req->state = REQ_DONE;
    req->next = NULL;
    if (g_done_tail) {
        g_done_tail->next = req;
    } else {
        g_done_head = req;
    }
    g_done_tail = req;
    rtems_counting_semaphore_post(&g_done_sem);
}

static void done_unqueue(struct nx_req *req)
{
    struct nx_req **link = &g_done_head, *prev = NULL;

    while (*link && *link != req) {
        prev = *link;
        link = &(*link)->next;
    }
    if (*link == req) {
        *link = req->next;
        if (g_done_tail == req) {
            g_done_tail = prev;
        }
    }
    req->next = NULL;
}

static void ep_unqueue(struct nx_ep *ep, struct nx_req *req)
{
    struct nx_req **link = &ep->head, *prev = NULL;

    while (*link && *link != req) {
        prev = *link;
        link = &(*link)->next;
    }
    if (*link == req) {
        *link = req->next;
        if (ep->tail == req) {
            ep->tail = prev;
        }
    }
    req->next = NULL;
}

static void nx_urb_done(void *arg, int nbytes);
static void nx_zlp_done(void *arg, int nbytes);

/*
 * Bulk traffic health for the station self-heal (wlan_dev_shim.c): tick of the last bulk IN
 * that completed, and of the first bulk OUT that completed after it (0: none). Commands go
 * out and their confirmations come back on bulk IN; OUT completing while IN stays silent for
 * long means the receive side is gone.
 */
static volatile rtems_interval g_io_last_in, g_io_out_since_in;

static void ep_leave_hw(struct nx_ep *ep, struct nx_req *req)
{
    if (req->in_hw) {
        req->in_hw = 0;
        ep->inflight--;
        if (ep->inflight == 0) {
            ep->streaming = 0;
        }
    }
}

static void ep_push_head(struct nx_ep *ep, struct nx_req *req)
{
    req->state = REQ_QUEUED;
    req->next = ep->head;
    ep->head = req;
    if (ep->tail == NULL) {
        ep->tail = req;
    }
}

/* a bulk IN request that can stay armed on the endpoint's EHCI stream QH */
static int req_stream_ok(const struct nx_req *req)
{
    return NX_IN_URBS_PER_EP > 1 && req->is_in && req->len != 0 &&
           req->len <= USBH_EHCI_STREAM_MAX_LEN && req->urb->ep &&
           USB_GET_ENDPOINT_TYPE(req->urb->ep->bmAttributes) == USB_ENDPOINT_TYPE_BULK;
}

static void ep_finish(struct nx_ep *ep, struct nx_req *req, int result)
{
    ep_leave_hw(ep, req);
    if (result >= 0 && req->urb && req->urb->ep &&
        USB_GET_ENDPOINT_TYPE(req->urb->ep->bmAttributes) == USB_ENDPOINT_TYPE_BULK) {
        rtems_interval now = rtems_clock_get_ticks_since_boot() | 1U;

        if (req->is_in) {
            g_io_last_in = now;
            g_io_out_since_in = 0;
        } else if (g_io_out_since_in == 0) {
            g_io_out_since_in = now;
        }
    }
    req->result = result;
    g_st_complete++;
    done_push(req);
}

/*
 * start queued requests: one in the controller per endpoint, or up to NX_IN_URBS_PER_EP bulk
 * IN requests on a stream QH (appended while the stream is open)
 */
static void ep_kick(struct nx_ep *ep)
{
    while (ep->head != NULL) {
        struct nx_req *req = ep->head;
        struct usbh_urb *urb = req->urb;
        int stream = req_stream_ok(req), append = ep->inflight != 0;
        int rc;

        if (append && (!stream || !ep->streaming || ep->inflight >= NX_IN_URBS_PER_EP)) {
            break;
        }
        ep->head = req->next;
        if (ep->head == NULL) {
            ep->tail = NULL;
        }
        req->next = NULL;
        urb->complete = nx_urb_done;
        urb->arg = req;
        urb->transfer_buffer = req->bounce ? req->bounce : req->buf;
        urb->transfer_buffer_length = req->len;
        urb->timeout = 0;
        urb->data_toggle = ep->toggle;
        urb->errorcode = 0;
        urb->hcpriv = NULL;
        urb->actual_length = 0;
        urb->transfer_flags = req->flags & ~(USBH_EHCI_URB_STREAM | USBH_EHCI_URB_APPEND_ONLY);
        if (stream) {
            urb->transfer_flags |= USBH_EHCI_URB_STREAM | (append ? USBH_EHCI_URB_APPEND_ONLY : 0);
            ep->streaming = 1;
        }
        req->stream = (uint8_t)stream;
        req->in_hw = 1;
        ep->inflight++;
        if (ep->inflight > ep->max_inflight) {
            ep->max_inflight = ep->inflight;
        }
        req->state = REQ_HW;
        rc = usbh_submit_urb(urb);
        if (rc == -USB_ERR_BUSY && append) {
            /* the stream halted and waits for its IAA cycle: retry when its URBs are back */
            ep_leave_hw(ep, req);
            ep_push_head(ep, req);
            g_st_append_busy++;
            break;
        }
        if (rc != 0) {
            ep_finish(ep, req, rc);
        }
    }
}

/* EHCI completion (interrupt server task or usbh_kill_urb(), critical section held) */
static void nx_urb_done(void *arg, int nbytes)
{
    struct nx_req *req = arg;
    struct nx_ep *ep = req->ep;
    struct usbh_urb *urb = req->urb;
    uint16_t mps;

    ep->toggle = urb->data_toggle;
    if (nbytes == -USB_ERR_SHUTDOWN && req->stream && !req->killing && req->state == REQ_HW &&
        urb->actual_length == 0) {
        /* its stream was torn down for another URB (kill or error) before it got data: requeue */
        ep_leave_hw(ep, req);
        ep_push_head(ep, req);
        g_st_requeue++;
        ep_kick(ep);
        return;
    }
    req->actual = urb->actual_length;
    mps = USB_GET_MAXPACKETSIZE(urb->ep->wMaxPacketSize);
    if (nbytes >= 0 && req->zlp && !req->is_in && req->len != 0 && mps != 0 &&
        (req->len % mps) == 0 && req->state == REQ_HW) {
        struct usbh_urb *z = &ep->zlp_urb;
        int rc;

        memset(z, 0, sizeof(*z));
        usbh_bulk_urb_fill(z, urb->hport, urb->ep, NULL, 0, 0, nx_zlp_done, req);
        z->data_toggle = ep->toggle;
        req->state = REQ_ZLP;
        g_st_zlp++;
        rc = usbh_submit_urb(z);
        if (rc == 0) {
            return;
        }
        LOG_W("ep 0x%02x: zero-length packet not sent (%d)", ep->addr, rc);
    }
    ep_finish(ep, req, nbytes < 0 ? nbytes : 0);
    ep_kick(ep);
}

static void nx_zlp_done(void *arg, int nbytes)
{
    struct nx_req *req = arg;
    struct nx_ep *ep = req->ep;

    ep->toggle = ep->zlp_urb.data_toggle;
    ep_finish(ep, req, nbytes < 0 ? nbytes : 0);
    ep_kick(ep);
}

/* thread context, no lock: hand the URB back to the driver's view */
static int req_restore(struct nx_req *req)
{
    struct usbh_urb *urb = req->urb;
    int result = req->result;

    urb->transfer_buffer = req->buf;
    urb->transfer_buffer_length = req->len;
    urb->transfer_flags = req->flags;
    urb->complete = req->complete;
    urb->arg = req->arg;
    urb->hcpriv = NULL;
    urb->actual_length = result < 0 ? 0 : req->actual;
    urb->errorcode = result;
    if (req->is_in && result >= 0 && req->actual) {
        if (req->bounce) {
            memcpy(req->buf, req->bounce, req->actual);
        } else {
            rtems_cache_invalidate_multiple_data_lines(req->buf, USB_ALIGN_UP(req->len, NX_LINE));
        }
    }
    if (req->bounce) {
        rtems_cache_coherent_free(req->bounce);
        req->bounce = NULL;
    }
    return result;
}

/* giveback: called with the critical section held, returns with it held */
static void deliver_locked(struct nx_req *req)
{
    struct usbh_urb *urb = req->urb, *previous = g_delivering;
    usbh_complete_callback_t complete;
    void *arg;
    int nbytes;

    req->state = REQ_DELIVERING;
    g_delivering = urb;
    usb_osal_leave_critical_section(0);
    nbytes = req_restore(req);
    complete = req->complete;
    arg = req->arg;
    if (nbytes >= 0) {
        nbytes = (int)urb->actual_length;
    }
    {
        CRIT_ENTER();
        req->state = REQ_FREE; /* the callback may resubmit the URB */
        CRIT_LEAVE();
    }
    if (complete) {
        complete(arg, nbytes);
    }
    (void)usb_osal_enter_critical_section();
    g_delivering = previous;
}

static void gb_thread(void *parameter)
{
    (void)parameter;
    g_gb_tid = pthread_self();
    g_gb_running = 1;
    for (;;) {
        struct nx_req *req;

        rtems_counting_semaphore_wait(&g_done_sem);
        CRIT_ENTER();
        req = g_done_head;
        if (req != NULL) {
            done_unqueue(req);
            deliver_locked(req);
        }
        CRIT_LEAVE();
    }
}

static int on_gb_thread(void)
{
    return g_gb_running && pthread_equal(pthread_self(), g_gb_tid);
}

/* ---- timeout diagnostics ---- */

struct nx_ep_snap {
    uint8_t state, toggle, streaming, zlp;
    uint16_t inflight, queued;
    unsigned timeouts;
};

static const char *const g_req_state[] = { "free", "queued", "in controller", "zero-length packet",
                                           "done", "delivering" };

/* the shim's view of the request and its endpoint, then the controller's (one lock) */
static void nx_snap(struct nx_req *req, struct nx_ep_snap *e, struct usbh_ehci_diag *d)
{
    struct nx_ep *ep = req->ep;
    struct usbh_urb *hw;
    struct nx_req *r;

    CRIT_ENTER();
    memset(e, 0, sizeof(*e));
    e->state = req->state;
    e->toggle = ep->toggle;
    e->streaming = ep->streaming;
    e->inflight = ep->inflight;
    e->timeouts = ep->timeouts;
    e->zlp = req->zlp;
    for (r = ep->head; r; r = r->next) {
        e->queued++;
    }
    hw = req->state == REQ_ZLP ? &ep->zlp_urb : req->urb;
    usbh_ehci_diag_capture(hw, d);
    CRIT_LEAVE();
}

static void nx_snap_print(const struct nx_req *req, const struct nx_ep_snap *e,
                          const struct usbh_ehci_diag *d, const char *when)
{
    char tag[32];

    printf("aic: ep 0x%02x %s: request %s, endpoint toggle DATA%u, %u in the controller%s, %u queued, "
           "%u timeout(s) so far\n",
           req->ep->addr, when, g_req_state[e->state % 6], e->toggle, e->inflight,
           e->streaming ? " (stream)" : "", e->queued, e->timeouts);
    snprintf(tag, sizeof(tag), "ep 0x%02x %s", req->ep->addr, when);
    usbh_ehci_diag_print(d, tag);
}

/*
 * A synchronous transfer timed out (-USB_ERR_TIMEOUT follows): before the cancel, capture the
 * shim and controller state twice 100 ms apart (rate limited: the first 4, then one per 10 s).
 * Returns 1 when the transfer completed within those 100 ms (no cancel needed).
 */
static int nx_timeout_report(struct nx_req *req, rtems_binary_semaphore *waiter, uint32_t timeout)
{
    struct nx_ep_snap e0, e1;
    struct usbh_ehci_diag d0, d1;
    int done;

    {
        CRIT_ENTER();
        req->ep->timeouts++;
        CRIT_LEAVE();
    }
    if (!usbh_ehci_diag_allowed()) {
        return 0;
    }
    nx_snap(req, &e0, &d0);
    done = rtems_binary_semaphore_wait_timed_ticks(waiter, RTEMS_MILLISECONDS_TO_TICKS(100) + 1) == 0;
    nx_snap(req, &e1, &d1);
    printf("aic: ep 0x%02x: %s of %u bytes timed out after %u ms%s\n", req->ep->addr,
           req->is_in ? "bulk IN" : "bulk OUT", (unsigned)req->len, (unsigned)timeout,
           done ? ", completed 100 ms later" : "; state before the cancel:");
    nx_snap_print(req, &e0, &d0, "t+0");
    nx_snap_print(req, &e1, &d1, "t+100ms");
    return done;
}

/* ---- the driver-facing API ---- */

int aicwf_cherry_submit_urb(struct usbh_urb *urb)
{
    rtems_binary_semaphore waiter;
    struct nx_req *req;
    struct nx_ep *ep;
    uint32_t timeout;
    uint8_t *bounce = NULL;
    int is_in, need_bounce, result, depth;

    if (urb == NULL || urb->hport == NULL || urb->ep == NULL) {
        return -USB_ERR_INVAL;
    }
    if (!urb->hport->connected) {
        return -USB_ERR_NOTCONN;
    }
    is_in = (urb->ep->bEndpointAddress & 0x80) != 0;
    need_bounce = urb->transfer_buffer_length != 0 &&
                  (((uintptr_t)urb->transfer_buffer & (NX_LINE - 1)) != 0 ||
                   (is_in && (urb->transfer_buffer_length & (NX_LINE - 1)) != 0));
    if (need_bounce) {
        bounce = rtems_cache_coherent_allocate(USB_ALIGN_UP(urb->transfer_buffer_length, NX_LINE),
                                               NX_LINE, 0);
        if (bounce == NULL) {
            return -USB_ERR_NOMEM;
        }
        if (!is_in) {
            memcpy(bounce, urb->transfer_buffer, urb->transfer_buffer_length);
        }
    }
    timeout = urb->timeout;
    if (timeout) {
        rtems_binary_semaphore_init(&waiter, "aicsync");
    }

    CRIT_ENTER();
    if (req_find(urb) != NULL) {
        CRIT_LEAVE();
        if (bounce) {
            rtems_cache_coherent_free(bounce);
        }
        if (timeout) {
            rtems_binary_semaphore_destroy(&waiter);
        }
        return -USB_ERR_BUSY;
    }
    req = req_alloc();
    ep = req ? ep_get(urb->hport, urb->ep->bEndpointAddress, 1) : NULL;
    if (req == NULL || ep == NULL) {
        if (req) {
            req->state = REQ_FREE;
        }
        g_st_noreq++;
        CRIT_LEAVE();
        if (bounce) {
            rtems_cache_coherent_free(bounce);
        }
        if (timeout) {
            rtems_binary_semaphore_destroy(&waiter);
        }
        return -USB_ERR_NOMEM;
    }
    req->urb = urb;
    req->ep = ep;
    req->complete = urb->complete;
    req->arg = urb->arg;
    req->buf = urb->transfer_buffer;
    req->len = urb->transfer_buffer_length;
    req->bounce = bounce;
    req->is_in = (uint8_t)is_in;
    req->zlp = (urb->transfer_flags & URB_ZERO_PACKET) != 0;
    req->flags = urb->transfer_flags;
    req->waiter = timeout ? &waiter : NULL;
    urb->errorcode = -USB_ERR_BUSY;
    urb->actual_length = 0;
    if (ep->tail) {
        ep->tail->next = req;
    } else {
        ep->head = req;
    }
    ep->tail = req;
    {
        struct nx_req *r;

        for (depth = 0, r = ep->head; r; r = r->next) {
            depth++;
        }
        if ((unsigned)depth > g_st_maxq) {
            g_st_maxq = (unsigned)depth;
        }
    }
    g_st_submit++;
    if (bounce) {
        g_st_bounce++;
    }
    ep_kick(ep);
    CRIT_LEAVE();

    if (!timeout) {
        return 0;
    }

    /* synchronous: the URB timeout is the transfer timeout, then cancel and wait for it */
    if (rtems_binary_semaphore_wait_timed_ticks(&waiter, RTEMS_MILLISECONDS_TO_TICKS(timeout) + 1) != 0 &&
        !nx_timeout_report(req, &waiter, timeout)) {
        int cancelled = 0;

        (void)aicwf_cherry_kill_urb(urb);
        cancelled = 1;
        while (rtems_binary_semaphore_wait_timed_ticks(&waiter,
                                                      RTEMS_MILLISECONDS_TO_TICKS(NX_SYNC_GRACE_MS)) != 0) {
            LOG_W("ep 0x%02x: still waiting for a cancelled transfer", urb->ep->bEndpointAddress);
            (void)aicwf_cherry_kill_urb(urb);
        }
        result = req_restore(req);
        if (cancelled && result == -USB_ERR_SHUTDOWN) {
            result = -USB_ERR_TIMEOUT;
            urb->errorcode = result;
        }
    } else {
        result = req_restore(req);
    }
    {
        CRIT_ENTER();
        req->state = REQ_FREE;
        CRIT_LEAVE();
    }
    rtems_binary_semaphore_destroy(&waiter);
    urb->timeout = 0;
    if (result == 0 && (urb->transfer_flags & URB_SHORT_NOT_OK) &&
        urb->actual_length < urb->transfer_buffer_length) {
        result = -USB_ERR_IO;
    }
    return result;
}

/*
 * Synchronous, as in the K230 port: when this returns the transfer is finished and its
 * completion callback has run (or the URB was idle); stack-allocated URBs may go away.
 */
int aicwf_cherry_kill_urb(struct usbh_urb *urb)
{
    int self = on_gb_thread();
    unsigned waits = 0;

    if (urb == NULL) {
        return -USB_ERR_INVAL;
    }
    CRIT_ENTER();
    for (;;) {
        struct nx_req *req = req_find(urb);

        if (req == NULL) {
            if (g_delivering == urb && !self) {
                goto wait;
            }
            break;
        }
        switch (req->state) {
        case REQ_QUEUED:
            ep_unqueue(req->ep, req);
            g_st_kill++;
            ep_finish(req->ep, req, -USB_ERR_SHUTDOWN);
            continue;
        case REQ_HW:
        case REQ_ZLP: {
            struct usbh_urb *hw = req->state == REQ_HW ? urb : &req->ep->zlp_urb;
            uint8_t state = req->state;
            int rc;

            g_st_kill++;
            req->killing = 1;
            rc = usbh_kill_urb(hw);
            if (rc == -USB_ERR_BUSY) {
                /*
                 * the controller still owns it (cancel deferred, controller reset pending):
                 * wait with the lock released at every level, never loop with it held
                 */
                size_t depth = usb_osal_crit_suspend();

                usb_osal_msleep(1);
                usb_osal_crit_resume(depth);
                g_st_kill_busy++;
                continue;
            }
            if (rc != 0 && req->state == state) {
                /* the controller side does not know it: end it here */
                ep_finish(req->ep, req, -USB_ERR_SHUTDOWN);
                ep_kick(req->ep);
            }
            continue;
        }
        case REQ_DONE:
            if (req->waiter) {
                break; /* the synchronous submitter finishes it */
            }
            if (self) {
                done_unqueue(req);
                deliver_locked(req);
                continue;
            }
            goto wait;
        case REQ_DELIVERING:
            if (self || req->waiter) {
                break;
            }
            goto wait;
        default:
            break;
        }
        break;
    wait:
        CRIT_LEAVE();
        usb_osal_msleep(1);
        if (++waits % 5000 == 0) {
            LOG_W("kill ep 0x%02x: waiting for the transfer to finish",
                  urb->ep ? urb->ep->bEndpointAddress : 0);
        }
        crit_flags_ = usb_osal_enter_critical_section();
    }
    CRIT_LEAVE();
    return 0;
}

int aicwf_cherry_control_transfer(struct usbh_hubport *hport, struct usb_setup_packet *setup,
                                  uint8_t *buffer)
{
    struct usb_setup_packet *nc;
    uint16_t len;
    uint8_t *data;
    int rc;

    if (hport == NULL || setup == NULL) {
        return -USB_ERR_INVAL;
    }
    if (!hport->connected) {
        return -USB_ERR_NOTCONN;
    }
    len = setup->wLength;
    nc = rtems_cache_coherent_allocate(NX_LINE + USB_ALIGN_UP(len, NX_LINE), NX_LINE, 0);
    if (nc == NULL) {
        return -USB_ERR_NOMEM;
    }
    data = (uint8_t *)nc + NX_LINE;
    memcpy(nc, setup, sizeof(*nc));
    if (len && buffer && (setup->bmRequestType & USB_REQUEST_DIR_MASK) == USB_REQUEST_DIR_OUT) {
        memcpy(data, buffer, len);
    }
    rc = usbh_control_transfer(hport, nc, len ? data : NULL);
    if (rc > 0 && buffer && (setup->bmRequestType & USB_REQUEST_DIR_MASK) == USB_REQUEST_DIR_IN) {
        memcpy(buffer, data, (uint32_t)rc < len ? (uint32_t)rc : len);
    }
    rtems_cache_coherent_free(nc);
    return rc;
}

int usbh_dwc2_reset_endpoint(struct usbh_hubport *hport, uint8_t endpoint)
{
    struct nx_ep *ep;

    CRIT_ENTER();
    ep = ep_get(hport, endpoint, 0);
    if (ep) {
        ep->toggle = 0;
    }
    CRIT_LEAVE();
    return 0;
}

/* ---- re-enumeration (K230 usbh_hubport_reenumerate) ---- */

static struct nx_dev *dev_find(struct usbh_hubport *hport)
{
    int i;

    for (i = 0; i < NX_MAX_DEV; i++) {
        if (g_dev[i].hport == hport) {
            return &g_dev[i];
        }
    }
    return NULL;
}

static int hub_port_feature(struct usbh_hub *hub, uint8_t port, uint8_t request, uint8_t feature)
{
    struct usb_setup_packet *setup;
    int rc;

    if (hub->is_roothub) {
        return request == HUB_REQUEST_SET_FEATURE ? usbh_hub_set_feature(hub, port, feature) :
                                                    usbh_hub_clear_feature(hub, port, feature);
    }
    /* own non-cacheable setup packet: the hub thread fills the parent's one without a lock */
    setup = rtems_cache_coherent_allocate(NX_LINE, NX_LINE, 0);
    if (setup == NULL) {
        return -USB_ERR_NOMEM;
    }
    setup->bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_OTHER;
    setup->bRequest = request;
    setup->wValue = feature;
    setup->wIndex = port;
    setup->wLength = 0;
    rc = usbh_control_transfer(hub->parent, setup, NULL);
    rtems_cache_coherent_free(setup);
    return rc;
}

static void *reenum_thread(void *arg)
{
    struct usbh_hubport *hport = arg;
    struct usbh_hub *hub = hport->parent;
    struct usbh_bus *bus = hport->bus;
    uint8_t port = hport->port, speed = hport->speed;
    struct nx_dev *dev;
    int rc;

    /* the request may come from a connect() that is still running on the hub thread */
    usb_osal_msleep(500);
    if (!hport->connected || hub == NULL) {
        goto out;
    }
    LOG_W("re-enumerating %04x:%04x on hub %u port %u", hport->device_desc.idVendor,
          hport->device_desc.idProduct, hub->index, port);
    usbh_hubport_release(hport);
    rc = hub_port_feature(hub, port, HUB_REQUEST_SET_FEATURE, HUB_PORT_FEATURE_RESET);
    if (rc < 0) {
        LOG_E("port %u reset failed: %d", port, rc);
        goto out;
    }
    usb_osal_msleep(200);
    if (!hub->is_roothub) {
        (void)hub_port_feature(hub, port, HUB_REQUEST_CLEAR_FEATURE, HUB_PORT_FEATURE_C_RESET);
    }
    if (hport->connected) {
        /* the hub thread saw a real reconnect meanwhile and enumerated it */
        goto out;
    }
    memset(hport, 0, sizeof(*hport));
    hport->parent = hub;
    hport->depth = (hub->parent ? hub->parent->depth : 0) + 1;
    hport->connected = true;
    hport->port = port;
    hport->speed = speed;
    hport->bus = bus;
    hport->mutex = usb_osal_mutex_create();
    if (hport->mutex == NULL || usbh_enumerate(hport) < 0) {
        LOG_E("port %u: re-enumeration failed", port);
        usbh_hubport_release(hport);
    }
out:
    {
        CRIT_ENTER();
        dev = dev_find(hport);
        if (dev) {
            dev->reenumerating = 0;
        }
        CRIT_LEAVE();
    }
    return NULL;
}

int usbh_hubport_reenumerate(struct usbh_hubport *hport)
{
    pthread_attr_t attr;
    pthread_t tid;
    struct nx_dev *dev;
    int rc;

    if (hport == NULL || !hport->connected || hport->parent == NULL) {
        return -USB_ERR_NOTCONN;
    }
    {
        CRIT_ENTER();
        dev = dev_find(hport);
        if (dev && dev->reenumerating) {
            CRIT_LEAVE();
            return 0;
        }
        if (dev) {
            dev->reenumerating = 1;
        }
        CRIT_LEAVE();
    }
    g_st_reenum++;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    rc = pthread_create(&tid, &attr, reenum_thread, hport);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        if (dev) {
            dev->reenumerating = 0;
        }
        return -USB_ERR_NOMEM;
    }
    return 0;
}

/* ---- the CherryUSB class driver ---- */

static const struct aicwf_k230_class_info *k230_match(struct usbh_hubport *hport,
                                                     const struct usb_interface_descriptor *idesc)
{
    uint16_t vid = hport->device_desc.idVendor, pid = hport->device_desc.idProduct;
    size_t i;

    for (i = 0; i < sizeof(g_class_table) / sizeof(g_class_table[0]); i++) {
        const struct aicwf_k230_class_info *c = g_class_table[i];

        if ((c->match_flags & USB_CLASS_MATCH_VENDOR) && c->vid != vid)
            continue;
        if ((c->match_flags & USB_CLASS_MATCH_PRODUCT) && c->pid != pid)
            continue;
        if ((c->match_flags & USB_CLASS_MATCH_INTF_CLASS) && c->class != idesc->bInterfaceClass)
            continue;
        if ((c->match_flags & USB_CLASS_MATCH_INTF_SUBCLASS) && c->subclass != idesc->bInterfaceSubClass)
            continue;
        if ((c->match_flags & USB_CLASS_MATCH_INTF_PROTOCOL) && c->protocol != idesc->bInterfaceProtocol)
            continue;
        return c;
    }
    return NULL;
}

static struct nx_dev *dev_get(struct usbh_hubport *hport)
{
    struct nx_dev *dev = dev_find(hport);
    int i;

    if (dev) {
        return dev;
    }
    for (i = 0; i < NX_MAX_DEV; i++) {
        if (g_dev[i].hport == NULL) {
            memset(&g_dev[i], 0, sizeof(g_dev[i]));
            g_dev[i].hport = hport;
            g_dev[i].vid = hport->device_desc.idVendor;
            g_dev[i].pid = hport->device_desc.idProduct;
            return &g_dev[i];
        }
    }
    return NULL;
}

static int aicwf_cherry_connect(struct usbh_hubport *hport, uint8_t intf)
{
    const struct usb_interface_descriptor *idesc = &hport->config.intf[intf].altsetting[0].intf_desc;
    uint16_t vid = hport->device_desc.idVendor, pid = hport->device_desc.idProduct;
    const struct aicwf_k230_class_info *cls;
    struct nx_dev *dev;
    unsigned waited;
    int rc;

    if (idesc->bInterfaceClass == USB_DEVICE_CLASS_MASS_STORAGE && aicwf_cherry_is_storage_mode(vid, pid)) {
        return aicwf_cherry_modeswitch_connect(hport, intf);
    }
    cls = k230_match(hport, idesc);
    if (cls == NULL) {
        LOG_I("%04x:%04x interface %u (%02x/%02x/%02x): no RT-Smart class driver", vid, pid, intf,
              idesc->bInterfaceClass, idesc->bInterfaceSubClass, idesc->bInterfaceProtocol);
        return -USB_ERR_NODEV;
    }
    if (intf >= CONFIG_USBHOST_MAX_INTERFACES) {
        return -USB_ERR_NODEV;
    }
    g_attach_count++;
    /* the class drivers need aicwf_usbh_start() (firmware files installed, driver init) */
    for (waited = 0; !g_started && waited < NX_START_WAIT_MS; waited += 50) {
        usb_osal_msleep(50);
    }
    if (!g_started) {
        LOG_E("%04x:%04x: AIC driver not started, interface %u ignored", vid, pid, intf);
        return -USB_ERR_NODEV;
    }
    {
        CRIT_ENTER();
        dev = dev_get(hport);
        if (dev && dev->connected[intf] == 0 && dev->cls[intf] == NULL) {
            /* a new device on this port: no stale toggles */
            int any = 0, k;

            for (k = 0; k < CONFIG_USBHOST_MAX_INTERFACES; k++) {
                any |= dev->connected[k];
            }
            if (!any) {
                ep_purge(hport);
                g_io_out_since_in = 0;
            }
        }
        CRIT_LEAVE();
    }
    if (dev == NULL) {
        LOG_E("no device slot for %04x:%04x", vid, pid);
        return -USB_ERR_NOMEM;
    }
    LOG_I("%04x:%04x interface %u -> %s", vid, pid, intf, cls->class_driver->driver_name);
    rc = cls->class_driver->connect(hport, intf);
    if (rc == 0) {
        dev->cls[intf] = cls;
        dev->connected[intf] = 1;
        /* bring the station up here, serialised with the disconnects (hub thread) */
        aicwf_wlan_attach_done();
    } else {
        LOG_W("%s connect on interface %u returned %d", cls->class_driver->driver_name, intf, rc);
    }
    return rc;
}

static int aicwf_cherry_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    struct nx_dev *dev;
    const struct aicwf_k230_class_info *cls = NULL;
    int k, any = 0;

    {
        CRIT_ENTER();
        dev = dev_find(hport);
        if (dev && intf < CONFIG_USBHOST_MAX_INTERFACES && dev->connected[intf]) {
            cls = dev->cls[intf];
        }
        CRIT_LEAVE();
    }
    if (cls && cls->class_driver->disconnect) {
        cls->class_driver->disconnect(hport, intf);
    }
    CRIT_ENTER();
    if (cls) {
        int left = 0;

        for (k = 0; k < NX_MAX_REQ; k++) {
            if (g_req[k].state != REQ_FREE && g_req[k].ep && g_req[k].ep->hport == hport) {
                left++;
            }
        }
        if (left) {
            LOG_W("%s left %d transfer(s) after disconnect", cls->class_driver->driver_name, left);
        }
    }
    if (dev && intf < CONFIG_USBHOST_MAX_INTERFACES) {
        dev->connected[intf] = 0;
        dev->cls[intf] = NULL;
        for (k = 0; k < CONFIG_USBHOST_MAX_INTERFACES; k++) {
            any |= dev->connected[k];
        }
        if (!any && !dev->reenumerating) {
            ep_purge(hport);
            dev->hport = NULL;
        }
    }
    CRIT_LEAVE();
    return 0;
}

const struct usbh_class_driver aicwf_cherry_class_driver = {
    .driver_name = "aic8800",
    .connect = aicwf_cherry_connect,
    .disconnect = aicwf_cherry_disconnect,
};

/* ---- diagnostics used by the shim (wlan_dev_shim.c, bt_hci_shim.c) ---- */

unsigned aicwf_usbh_attach_count(void)
{
    return g_attach_count;
}

void aicwf_usbh_dump(void)
{
    int i, k;

    printf("usb: CherryUSB host, %u interface attach(es) since start\n", g_attach_count);
    CRIT_ENTER();
    for (i = 0; i < NX_MAX_DEV; i++) {
        struct nx_dev *dev = &g_dev[i];

        if (dev->hport == NULL) {
            continue;
        }
        printf("usb: %04x:%04x speed %u addr %u %s\n", dev->vid, dev->pid, dev->hport->speed,
               dev->hport->dev_addr, dev->hport->connected ? "connected" : "gone");
        for (k = 0; k < CONFIG_USBHOST_MAX_INTERFACES; k++) {
            if (dev->cls[k]) {
                printf("usb:   iface %d -> %s%s\n", k, dev->cls[k]->class_driver->driver_name,
                       dev->connected[k] ? " (connected)" : "");
            }
        }
    }
    CRIT_LEAVE();
}

int aicwf_usbh_first_id(rt_uint16_t *vid, rt_uint16_t *pid)
{
    int i, found = 0;

    CRIT_ENTER();
    for (i = 0; i < NX_MAX_DEV && !found; i++) {
        if (g_dev[i].hport && g_dev[i].hport->connected) {
            *vid = g_dev[i].vid;
            *pid = g_dev[i].pid;
            found = 1;
        }
    }
    CRIT_LEAVE();
    return found;
}

/* ms since the first bulk OUT that completed after the last bulk IN; 0 while none waits */
unsigned aicwf_usbh_in_silence_ms(void)
{
    rtems_interval since = g_io_out_since_in;

    if (since == 0) {
        return 0;
    }
    return (unsigned)((uint64_t)(rtems_clock_get_ticks_since_boot() - since) * 1000U /
                      rtems_clock_get_ticks_per_second());
}

int aicwf_usbh_reprobe(void)
{
    struct usbh_hubport *hport = NULL;
    int i;

    CRIT_ENTER();
    for (i = 0; i < NX_MAX_DEV && hport == NULL; i++) {
        if (g_dev[i].hport && g_dev[i].hport->connected) {
            hport = g_dev[i].hport;
        }
    }
    g_io_out_since_in = 0;
    CRIT_LEAVE();
    return hport ? usbh_hubport_reenumerate(hport) : -USB_ERR_NODEV;
}

void aicwf_cherry_stats(void)
{
    int i, inflight = 0, queued = 0, done = 0;

    CRIT_ENTER();
    for (i = 0; i < NX_MAX_REQ; i++) {
        switch (g_req[i].state) {
        case REQ_HW:
        case REQ_ZLP:
            inflight++;
            break;
        case REQ_QUEUED:
            queued++;
            break;
        case REQ_DONE:
        case REQ_DELIVERING:
            done++;
            break;
        default:
            break;
        }
    }
    CRIT_LEAVE();
    printf("aic: urbs submitted %u completed %u killed %u, bounced %u, zlp %u, no slot %u, "
           "max queue %u, re-enumerations %u, stream requeues %u append-busy %u kill-busy %u; now in "
           "controller %d queued %d giveback %d\n",
           g_st_submit, g_st_complete, g_st_kill, g_st_bounce, g_st_zlp, g_st_noreq, g_st_maxq,
           g_st_reenum, g_st_requeue, g_st_append_busy, g_st_kill_busy, inflight, queued, done);
    {
        CRIT_ENTER();
        for (i = 0; i < NX_MAX_EP; i++) {
            if (g_ep[i].hport) {
                printf("aic:   ep 0x%02x: %u in the controller (max %u, limit %u)%s, toggle DATA%u, "
                       "%u timeout(s)\n", g_ep[i].addr, g_ep[i].inflight, g_ep[i].max_inflight,
                       (g_ep[i].addr & 0x80) ? NX_IN_URBS_PER_EP : 1,
                       g_ep[i].streaming ? " stream" : "", g_ep[i].toggle, g_ep[i].timeouts);
            }
        }
        CRIT_LEAVE();
    }
}

/* ---- start (wlan_dev_shim.c aicwf_wifi_start, after the firmware files are installed) ---- */

void aicwf_usbh_start(void)
{
    rt_thread_t thread;
    rt_err_t result;

    pthread_mutex_lock(&g_start_lock);
    if (g_started) {
        pthread_mutex_unlock(&g_start_lock);
        return;
    }
    thread = rt_thread_create("aic-usbc", gb_thread, RT_NULL, 16384, 10, 0);
    if (!thread || rt_thread_startup(thread) != RT_EOK) {
        LOG_E("giveback thread failed to start");
        pthread_mutex_unlock(&g_start_lock);
        return;
    }
#ifdef AIC8800_WIFI_BLE
    result = aic8800_btusb_driver_init();
    if (result != RT_EOK) {
        LOG_W("Bluetooth HCI transport not available (%d)", result);
    }
#endif
    result = aic8800_usb_driver_init();
    if (result != RT_EOK) {
        LOG_E("AIC8800 USB driver initialisation failed (%d)", result);
        pthread_mutex_unlock(&g_start_lock);
        return;
    }
    g_started = 1;
    pthread_mutex_unlock(&g_start_lock);
}
