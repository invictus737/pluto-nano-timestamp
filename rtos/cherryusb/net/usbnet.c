/*
 * USB Ethernet glue: CherryUSB's upstream class drivers (class/vendor/net/usbh_rtl8152.c,
 * class/cdc/usbh_cdc_ecm.c, class/cdc/usbh_cdc_ncm.c, class/vendor/net/usbh_asix.c, built
 * unchanged apart from net/patches) to libbsd Ethernet interfaces ueN.
 *
 * The class drivers call back into four hooks per driver, all provided here:
 *   usbh_X_run(cls)          hub thread, end of connect(): ifnet attach, rx + link threads
 *   usbh_X_stop(cls)         hub thread, disconnect() after its URBs were killed and before
 *                            the class object is cleared: everything that can still touch the
 *                            class object or the ifnet is stopped before this returns
 *   usbh_X_eth_input(b, n)   the driver's rx thread, one Ethernet frame
 *   (transmit)               usbh_X_get_eth_txbuf() + usbh_X_eth_output(), called from the
 *                            ifnet's tx taskqueue thread (aicwf_ifnet_attach in libbsd.a,
 *                            if_aicwf_usb.c: the same Ethernet ifnet the Wi-Fi station uses)
 *
 * Threads per attached adapter, no polling while idle:
 *  - rx: the driver's own usbh_X_rx_thread(): one bulk IN URB in flight, the thread sleeps in
 *    the URB completion semaphore; each frame goes to ether_input through m_devget (one copy
 *    out of the non-cacheable class buffer). Started on a plain pthread (not the OSAL, whose
 *    handle leaks once per thread exit) with a cleanup handler: the driver ends it with
 *    usb_osal_thread_delete(NULL) = pthread_exit when its device is gone, and the handler
 *    records that, so a replug never runs two rx threads on one class object.
 *  - link: the driver's usbh_X_get_connect_status() on the interrupt endpoint. Until the
 *    driver's rx thread has seen the link and calls usbh_X_linkup() (net/patches/0103) that
 *    endpoint is the rx thread's and the link thread sleeps without a period; afterwards CDC
 *    ECM/NCM block until the next notification, RTL8152 and ASIX are read every
 *    NEXUS_USBNET_LINK_POLL_MS (twice that with the link down) and a link-down needs two
 *    consecutive reads. A failed bulk IN with the device attached is resubmitted by the
 *    driver (0103): no re-wait for the link, no chip re-init, no false link-down.
 *    The link thread also retries a failed ifnet attach or rx thread start (1 s).
 *  - priorities: rx and link threads at CherryUSB 15 (POSIX 135) on NEXUS_USB_THREAD_CPU
 *    (CPU0), below the hub thread and far below usbh_irq (max - 18); the radio runs on CPU1.
 * Link changes go to the ifnet (LINK_STATE_UP/DOWN): dhcpcd follows them, and the departing
 * interface (ether_ifdetach on unplug) drops its routes and DNS servers.
 *
 * Transmit: one frame per bulk OUT transfer; a transfer that would be a whole number of
 * max-packet sizes gets one pad byte (Linux usbnet without FLAG_SEND_ZLP does the same), so
 * the device sees its end without a zero-length packet.
 *
 * Configuration choice: CherryUSB enumerates configuration index 0 unless the weak
 * usbh_get_hport_active_config_index() says otherwise. The override below reads every
 * configuration of a multi-configuration device and selects, in this order, one whose vendor
 * interface a vendor driver here claims (RTL8152 0bda:8152, the ASIX table), then CDC-ECM, then
 * CDC-NCM. RTL8153/RTL8156 adapters (vendor configuration 1, which usbh_rtl8152.c v1.6.1 does
 * not support: its rtl_ops_init knows RTL_VER_01/02/07 only) thus come up in their CDC-ECM
 * configuration, deterministically, on every plug.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE /* pthread_attr_setaffinity_np */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "usbh_core.h"
#include "usbh_rtl8152.h"
#include "usbh_cdc_ecm.h"
#include "usbh_cdc_ncm.h"
#include "usbh_asix.h"

#include "aicwf_usbif.h" /* aicwf_ifnet_*: Ethernet ifnet in libbsd.a (if_aicwf_usb.c) */
#include "usbnet.h"

#define NX_UNITS    8 /* ue0..ue7 */
#define NX_POSIX_PRIO(cherry_prio) (150 - (int)(cherry_prio)) /* osal/usb_osal_rtems.c */

/* ---- driver table ---- */

struct nx_drv {
    const char *name;
    uint8_t tx_hdr;  /* bytes the driver puts before the frame in the bulk OUT transfer */
    uint8_t ncm;     /* NTB framing: transfer = 32 + ALIGN4(frame) */
    uint8_t rx_trim; /* trailing bytes of every received frame (FCS) */
    uint8_t pad_tx;  /* pad a transfer that is a multiple of wMaxPacketSize */
    bool periodic;   /* the interrupt endpoint answers every interval */
    void (*rx_thread)(CONFIG_USB_OSAL_THREAD_SET_ARGV);
    int (*status)(void *cls);
    bool (*link)(void *cls);
    const uint8_t *(*mac)(void *cls);
    struct usbh_hubport *(*hport)(void *cls);
    uint16_t (*out_mps)(void *cls);
    uint8_t *(*txbuf)(void);
    int (*output)(uint32_t len);
};

struct nx_slot {
    const struct nx_drv *drv;
    pthread_mutex_t lock;
    pthread_cond_t cond;  /* link thread: stop, rx exit, first frame */
    void *cls;            /* class object between run() and stop() */
    struct aicwf_ifnet *ifn;      /* receive path; NULL once stop() began */
    struct aicwf_ifnet *ifn_link; /* link thread's copy, detached after it is joined */
    volatile bool attached;       /* run() .. stop(): transmit allowed */
    bool stopping;
    bool rx_alive;   /* our rx pthread exists */
    bool rx_respawn; /* it exited while the device is attached */
    bool link_thread;
    pthread_t link_tid;
    bool link_up;    /* as reported to the ifnet */
    bool rx_linked;  /* usbh_X_linkup(): the interrupt endpoint is the link thread's */
    uint8_t down_reads; /* consecutive link-down reads (polled chips: debounce) */
    int unit;
    char ifname[16];
    uint8_t mac[6];
    uint16_t vid, pid;
    /* counters (approximate: not all updated under the lock) */
    uint32_t attaches, link_changes, rx_frames, rx_drops, tx_frames, tx_errors, rx_restarts;
    char last[64];
};

static void nx_run(struct nx_slot *s, void *cls);
static void nx_stop(struct nx_slot *s, void *cls);
static void nx_input(struct nx_slot *s, uint8_t *buf, uint32_t len);
static void nx_linkup(struct nx_slot *s, void *cls);
static int nx_tx(void *arg, const void *frame, uint32_t len);

#define NX_DRIVER(id, T, NAME, HDR, NCM, TRIM, PAD, PERIODIC)                                    \
    static int id##_status(void *c) { return usbh_##id##_get_connect_status((T *)c); }          \
    static bool id##_link(void *c) { return ((T *)c)->connect_status; }                         \
    static const uint8_t *id##_mac(void *c) { return ((T *)c)->mac; }                           \
    static struct usbh_hubport *id##_hport(void *c) { return ((T *)c)->hport; }                 \
    static uint16_t id##_mps(void *c)                                                           \
    {                                                                                           \
        T *k = c;                                                                               \
        return k->bulkout ? USB_GET_MAXPACKETSIZE(k->bulkout->wMaxPacketSize) : 512;            \
    }                                                                                           \
    static const struct nx_drv id##_drv = {                                                     \
        .name = NAME, .tx_hdr = HDR, .ncm = NCM, .rx_trim = TRIM, .pad_tx = PAD,                \
        .periodic = PERIODIC, .rx_thread = usbh_##id##_rx_thread, .status = id##_status,        \
        .link = id##_link, .mac = id##_mac, .hport = id##_hport, .out_mps = id##_mps,           \
        .txbuf = usbh_##id##_get_eth_txbuf, .output = usbh_##id##_eth_output,                   \
    };                                                                                          \
    static struct nx_slot id##_slot = { .drv = &id##_drv, .unit = -1 };                         \
    void usbh_##id##_run(T *c) { nx_run(&id##_slot, c); }                                       \
    void usbh_##id##_stop(T *c) { nx_stop(&id##_slot, c); }                                     \
    void usbh_##id##_eth_input(uint8_t *buf, uint32_t len) { nx_input(&id##_slot, buf, len); } \
    void usbh_##id##_linkup(T *c) { nx_linkup(&id##_slot, c); }

/*
 * rtl8152: tx_desc of 8 bytes before the frame; rx_desc lengths include the 4-byte FCS (Linux
 * r8152 rx_bottom subtracts ETH_FCS_LEN). asix: the driver pads itself (net/patches).
 */
NX_DRIVER(rtl8152, struct usbh_rtl8152, "rtl8152", 8, 0, 4, 1, true)
NX_DRIVER(cdc_ecm, struct usbh_cdc_ecm, "cdc_ecm", 0, 0, 0, 1, false)
NX_DRIVER(cdc_ncm, struct usbh_cdc_ncm, "cdc_ncm", 0, 1, 0, 1, false)
NX_DRIVER(asix, struct usbh_asix, "asix", 4, 0, 0, 0, true)

static struct nx_slot *const nx_slots[] = { &rtl8152_slot, &cdc_ecm_slot, &cdc_ncm_slot, &asix_slot };
#define NX_NSLOTS (sizeof(nx_slots) / sizeof(nx_slots[0]))

static pthread_mutex_t nx_units_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t nx_units_used;
static pthread_once_t nx_once = PTHREAD_ONCE_INIT;

static void nx_init_once(void)
{
    pthread_mutexattr_t ma;
    pthread_condattr_t ca;

    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    for (size_t i = 0; i < NX_NSLOTS; i++) {
        pthread_mutex_init(&nx_slots[i]->lock, &ma);
        pthread_cond_init(&nx_slots[i]->cond, &ca);
    }
    pthread_condattr_destroy(&ca);
    pthread_mutexattr_destroy(&ma);
}

static double nx_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void nx_event(struct nx_slot *s, const char *what)
{
    snprintf(s->last, sizeof(s->last), "%s at %.1f s", what, nx_now());
}

/* wait on the slot's condition for at most ms; called with the lock held */
static void nx_wait_ms(struct nx_slot *s, uint32_t ms)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ms / 1000U;
    ts.tv_nsec += (long)(ms % 1000U) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    (void)pthread_cond_timedwait(&s->cond, &s->lock, &ts);
}

static int nx_thread(pthread_t *tid, bool joinable, size_t stack, void *(*fn)(void *), void *arg)
{
    pthread_attr_t at;
    struct sched_param sp = { .sched_priority = NX_POSIX_PRIO(NEXUS_USBNET_RX_PRIO) };
    cpu_set_t cpus;
    int rc;

    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, stack);
    pthread_attr_setdetachstate(&at, joinable ? PTHREAD_CREATE_JOINABLE : PTHREAD_CREATE_DETACHED);
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, SCHED_FIFO);
    pthread_attr_setschedparam(&at, &sp);
    CPU_ZERO(&cpus);
    CPU_SET(NEXUS_USB_THREAD_CPU, &cpus);
    pthread_attr_setaffinity_np(&at, sizeof(cpus), &cpus);
    rc = pthread_create(tid, &at, fn, arg);
    if (rc == EINVAL || rc == EPERM) { /* one CPU online: no affinity */
        pthread_attr_destroy(&at);
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, stack);
        pthread_attr_setdetachstate(&at, joinable ? PTHREAD_CREATE_JOINABLE : PTHREAD_CREATE_DETACHED);
        rc = pthread_create(tid, &at, fn, arg);
    }
    pthread_attr_destroy(&at);
    return rc;
}

/* ---- rx thread: the driver's own loop on a pthread whose end we see ---- */

static void nx_rx_exit(void *arg)
{
    struct nx_slot *s = arg;

    /* runs inside pthread_exit (usb_osal_thread_delete(NULL)): only record and wake */
    pthread_mutex_lock(&s->lock);
    s->rx_alive = false;
    if (s->attached && !s->stopping) {
        s->rx_respawn = true;
        pthread_cond_broadcast(&s->cond);
    }
    pthread_mutex_unlock(&s->lock);
}

static void *nx_rx_main(void *arg)
{
    struct nx_slot *s = arg;

    pthread_cleanup_push(nx_rx_exit, s);
    s->drv->rx_thread(NULL); /* ends in pthread_exit() when the device is gone */
    pthread_cleanup_pop(1);
    return NULL;
}

/* lock held; false if the thread could not be created (the link thread retries) */
static bool nx_rx_start_locked(struct nx_slot *s)
{
    pthread_t t;
    char n[16];

    if (s->rx_alive) {
        return true; /* the previous device's thread re-finds the class object (find_class) */
    }
    if (nx_thread(&t, false, NEXUS_USBNET_RX_STACKSIZE, nx_rx_main, s) != 0) {
        printf("usbnet: %s rx thread not started, retrying\n", s->ifname);
        return false;
    }
    snprintf(n, sizeof(n), "%s_rx", s->drv->name);
    (void)pthread_setname_np(t, n);
    s->rx_alive = true;
    /* a new rx thread waits for the link on the interrupt endpoint itself */
    s->rx_linked = false;
    return true;
}

/* ---- link thread ---- */

/* report a link state to the ifnet (lock held, dropped around the ifnet call) */
static void nx_report_locked(struct nx_slot *s, bool up)
{
    struct aicwf_ifnet *ifn = s->ifn_link;

    if (up == s->link_up || ifn == NULL) {
        return;
    }
    s->link_up = up;
    s->link_changes++;
    nx_event(s, up ? "link up" : "link down");
    pthread_mutex_unlock(&s->lock);
    printf("usbnet: %s link %s\n", s->ifname, up ? "up" : "down");
    aicwf_ifnet_link(ifn, up);
    pthread_mutex_lock(&s->lock);
}

/*
 * Owns, per attached adapter: the ifnet (attach retried while it fails), rx thread restarts,
 * and the link state once the driver's rx thread has called usbh_X_linkup(). Until then it
 * sleeps without a period (woken by linkup, stop or an rx thread exit). Afterwards CDC
 * ECM/NCM block in the status read until the device notifies; RTL8152/ASIX are read every
 * 500 ms with the link up, 1 s with it down, and a link-down needs two consecutive reads.
 */
static void *nx_link_main(void *arg)
{
    struct nx_slot *s = arg;
    const struct nx_drv *d = s->drv;

    pthread_mutex_lock(&s->lock);
    while (!s->stopping) {
        bool retry = false;

        if (s->ifn_link == NULL) {
            struct aicwf_ifnet *ifn;

            pthread_mutex_unlock(&s->lock);
            ifn = aicwf_ifnet_attach(s->ifname, s->mac, nx_tx, s);
            pthread_mutex_lock(&s->lock);
            if (ifn != NULL) {
                s->ifn_link = ifn;
                s->ifn = ifn;
                nx_event(s, "ifnet attached");
                if (s->stopping) {
                    break; /* stop() detaches it after the join */
                }
            } else {
                retry = true;
            }
        }
        if (s->rx_respawn && !s->rx_alive) {
            s->rx_respawn = false;
            s->rx_restarts++;
            if (!nx_rx_start_locked(s)) {
                s->rx_respawn = true;
                retry = true;
            }
        }
        if (!s->rx_linked || s->ifn_link == NULL) {
            if (retry) {
                nx_wait_ms(s, 1000);
            } else {
                pthread_cond_wait(&s->cond, &s->lock);
            }
            continue;
        }

        /* the driver's own wait saw the link up; its class object says so */
        bool up = d->link(s->cls);

        if (s->link_up || up) {
            if (up) {
                s->down_reads = 0;
                nx_report_locked(s, true);
            }
        }
        {
            void *cls = s->cls;
            int rc;

            pthread_mutex_unlock(&s->lock);
            rc = d->status(cls); /* CDC: blocks until a notification or the URB is killed */
            pthread_mutex_lock(&s->lock);
            if (s->stopping) {
                break;
            }
            if (rc < 0) {
                /* device going away (stop() follows) or a transient endpoint error: keep the
                 * reported state */
                nx_wait_ms(s, 1000);
                continue;
            }
        }
        up = d->link(s->cls);
        if (up) {
            s->down_reads = 0;
            nx_report_locked(s, true);
        } else if (!d->periodic || ++s->down_reads >= 2) {
            nx_report_locked(s, false);
        }
        if (d->periodic && !s->stopping) {
            nx_wait_ms(s, s->link_up ? NEXUS_USBNET_LINK_POLL_MS : 2 * NEXUS_USBNET_LINK_POLL_MS);
        }
    }
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

/* ---- class driver hooks ---- */

static void nx_run(struct nx_slot *s, void *cls)
{
    struct usbh_hubport *hport = s->drv->hport(cls);
    int unit = -1;

    pthread_once(&nx_once, nx_init_once);
    pthread_mutex_lock(&nx_units_lock);
    for (int u = 0; u < NX_UNITS && unit < 0; u++) {
        if (!(nx_units_used & (1U << u))) {
            unit = u;
            nx_units_used |= 1U << u;
        }
    }
    pthread_mutex_unlock(&nx_units_lock);
    if (unit < 0) {
        printf("usbnet: %s: no free ueN unit\n", s->drv->name);
        return;
    }

    pthread_mutex_lock(&s->lock);
    if (s->attached) { /* the patched connect() refuses a second device first (0103) */
        pthread_mutex_unlock(&s->lock);
        printf("usbnet: %s already attached\n", s->drv->name);
        goto free_unit;
    }
    s->cls = cls;
    s->unit = unit;
    snprintf(s->ifname, sizeof(s->ifname), "ue%d", unit);
    memcpy(s->mac, s->drv->mac(cls), 6);
    s->vid = hport ? hport->device_desc.idVendor : 0;
    s->pid = hport ? hport->device_desc.idProduct : 0;
    s->stopping = false;
    s->link_up = false;
    s->down_reads = 0;
    s->rx_respawn = false;
    s->ifn = NULL;
    s->ifn_link = NULL;
    s->attaches++;
    if ((s->mac[0] & 1) || (s->mac[0] | s->mac[1] | s->mac[2] | s->mac[3] | s->mac[4] | s->mac[5]) == 0) {
        /* no usable MAC from the device: locally administered one from the port path */
        static const uint8_t base[6] = { 0x02, 0x4e, 0x58, 0x55, 0x00, 0x00 };

        memcpy(s->mac, base, 6);
        s->mac[4] = hport ? hport->port : 0;
        s->mac[5] = (uint8_t)unit;
    }
    s->attached = true;
    pthread_mutex_unlock(&s->lock);

    /* ifnet first (frames before it are dropped); on failure the link thread retries */
    struct aicwf_ifnet *ifn = aicwf_ifnet_attach(s->ifname, s->mac, nx_tx, s);

    pthread_mutex_lock(&s->lock);
    if (ifn == NULL) {
        printf("usbnet: %s: ifnet %s not attached, retrying\n", s->drv->name, s->ifname);
    }
    s->ifn = ifn;
    s->ifn_link = ifn;
    nx_event(s, "attached");
    if (s->rx_alive && s->drv->link(cls)) {
        s->rx_linked = true; /* the previous device's rx thread re-found the class and is past
                              * its link wait already (its linkup came before this run) */
    }
    if (!nx_rx_start_locked(s)) {
        s->rx_respawn = true;
    }
    if (nx_thread(&s->link_tid, true, 16 * 1024, nx_link_main, s) == 0) {
        char n[16];

        s->link_thread = true;
        snprintf(n, sizeof(n), "%s_lnk", s->drv->name);
        (void)pthread_setname_np(s->link_tid, n);
    } else {
        printf("usbnet: %s link thread not started: no link state, no retries\n", s->ifname);
    }
    pthread_mutex_unlock(&s->lock);
    printf("usbnet: %s = %s %04x:%04x, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", s->ifname, s->drv->name,
           s->vid, s->pid, s->mac[0], s->mac[1], s->mac[2], s->mac[3], s->mac[4], s->mac[5]);
    return;
free_unit:
    pthread_mutex_lock(&nx_units_lock);
    nx_units_used &= ~(1U << unit);
    pthread_mutex_unlock(&nx_units_lock);
}

static void nx_linkup(struct nx_slot *s, void *cls)
{
    pthread_once(&nx_once, nx_init_once);
    pthread_mutex_lock(&s->lock);
    if (s->attached && s->cls == cls) {
        s->rx_linked = true;
        pthread_cond_broadcast(&s->cond);
    }
    pthread_mutex_unlock(&s->lock);
}

static void nx_stop(struct nx_slot *s, void *cls)
{
    struct aicwf_ifnet *ifn;
    bool join;
    int unit;

    pthread_once(&nx_once, nx_init_once);
    pthread_mutex_lock(&s->lock);
    if (!s->attached || s->cls != cls) {
        pthread_mutex_unlock(&s->lock);
        return;
    }
    /* from here no frame reaches the ifnet (nx_input) and no new transmit starts (nx_tx) */
    s->attached = false;
    s->stopping = true;
    s->ifn = NULL;
    join = s->link_thread;
    s->link_thread = false;
    unit = s->unit;
    nx_event(s, "detached");
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);

    /* the driver killed its interrupt URB before calling stop(), and the hport is no longer
     * connected (later submits fail at once): the link thread ends promptly */
    if (join) {
        pthread_join(s->link_tid, NULL);
    }
    pthread_mutex_lock(&s->lock);
    ifn = s->ifn_link; /* read after the join: the link thread may have attached it */
    s->ifn_link = NULL;
    pthread_mutex_unlock(&s->lock);
    /* ether_ifdetach, then the tx taskqueue is drained (a transmit in flight fails with
     * NOTCONN: the hport is disconnected) and the ifnet freed; dhcpcd sees the departure */
    if (ifn != NULL) {
        aicwf_ifnet_detach(ifn);
    }
    pthread_mutex_lock(&s->lock);
    s->cls = NULL;
    s->link_up = false;
    s->rx_linked = false;
    s->unit = -1;
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_lock(&nx_units_lock);
    if (unit >= 0) {
        nx_units_used &= ~(1U << unit);
    }
    pthread_mutex_unlock(&nx_units_lock);
    printf("usbnet: %s (%s) detached\n", s->ifname, s->drv->name);
    /* the rx thread finds no class object any more and exits by itself (nx_rx_exit) */
}

static void nx_input(struct nx_slot *s, uint8_t *buf, uint32_t len)
{
    const struct nx_drv *d = s->drv;
    int rc = -1;

    if (len > d->rx_trim) {
        len -= d->rx_trim;
    }
    pthread_mutex_lock(&s->lock);
    if (s->ifn != NULL) {
        rc = aicwf_ifnet_input(s->ifn, buf, len); /* m_devget + if_input, inline */
    }
    if (rc == 0) {
        s->rx_frames++;
    } else {
        s->rx_drops++;
    }
    pthread_mutex_unlock(&s->lock);
}

/* ifnet transmit (tx taskqueue thread of the ifnet, one at a time per interface) */
static int nx_tx(void *arg, const void *frame, uint32_t len)
{
    struct nx_slot *s = arg;
    const struct nx_drv *d = s->drv;
    uint32_t n = len, xfer;
    uint16_t mps;
    uint8_t *b;
    int rc;

    if (!s->attached || len == 0 || len > 1514 + 4) {
        s->tx_errors++;
        return -1;
    }
    b = d->txbuf();
    memcpy(b, frame, len);
    if (d->pad_tx) {
        mps = d->out_mps(s->cls);
        if (d->ncm) {
            xfer = 32 + ((n + 3) & ~3U);
            if (mps != 0 && (xfer % mps) == 0) {
                n = ((n + 3) & ~3U) + 1; /* NTB length + 4: no longer a multiple */
            }
        } else {
            xfer = d->tx_hdr + n;
            if (mps != 0 && (xfer % mps) == 0) {
                n++;
            }
        }
        if (n != len) {
            memset(b + len, 0, n - len);
        }
    }
    rc = d->output(n);
    if (rc < 0) {
        s->tx_errors++;
        return -1;
    }
    s->tx_frames++;
    return 0;
}

size_t nexus_usbnet_status(char *buf, size_t cap)
{
    size_t o = 0;

    if (cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    pthread_once(&nx_once, nx_init_once);
    for (size_t i = 0; i < NX_NSLOTS && o + 1 < cap; i++) {
        struct nx_slot *s = nx_slots[i];
        int k;

        pthread_mutex_lock(&s->lock);
        if (s->attached) {
            k = snprintf(buf + o, cap - o,
                         "%s %s %04x:%04x link %s rx %lu (drop %lu) tx %lu (err %lu) rx-restarts %lu, %s\n",
                         s->ifname, s->drv->name, s->vid, s->pid, s->link_up ? "up" : "down",
                         (unsigned long)s->rx_frames, (unsigned long)s->rx_drops,
                         (unsigned long)s->tx_frames, (unsigned long)s->tx_errors,
                         (unsigned long)s->rx_restarts, s->last);
        } else if (s->attaches != 0) {
            k = snprintf(buf + o, cap - o, "(%s: %lu attach(es), last %s)\n", s->drv->name,
                         (unsigned long)s->attaches, s->last);
        } else {
            k = 0;
        }
        pthread_mutex_unlock(&s->lock);
        if (k > 0) {
            o += (size_t)k < cap - o ? (size_t)k : cap - o - 1;
        }
    }
    return o;
}

/* ---- configuration choice (CherryUSB weak hook, called during enumeration) ---- */

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t nx_cfg_buf[USB_ALIGN_UP(512, CONFIG_USB_ALIGN_SIZE)];

/* vendor-mode IDs a driver here supports (usbh_rtl8152.c rtl_id_table, usbh_asix.c table) */
static const uint16_t nx_vendor_ids[][2] = {
    { 0x0bda, 0x8152 },
    { 0x0b95, 0x772b },
    { 0x0b95, 0x7720 },
};

static int nx_cfg_score(struct usbh_hubport *hport, const uint8_t *p, uint32_t len)
{
    int best = 0;
    bool vendor_ok = false;

    for (size_t i = 0; i < sizeof(nx_vendor_ids) / sizeof(nx_vendor_ids[0]); i++) {
        vendor_ok |= hport->device_desc.idVendor == nx_vendor_ids[i][0] &&
                     hport->device_desc.idProduct == nx_vendor_ids[i][1];
    }
    for (uint32_t o = 0; o + 2 <= len && p[o] >= 2 && o + p[o] <= len; o += p[o]) {
        if (p[o + 1] != USB_DESCRIPTOR_TYPE_INTERFACE || p[o] < 9) {
            continue;
        }
        uint8_t cls = p[o + 5], sub = p[o + 6];
        int score = 0;

        if (cls == 0xff && vendor_ok) {
            score = 3;
        } else if (cls == USB_DEVICE_CLASS_CDC && sub == CDC_ETHERNET_NETWORKING_CONTROL_MODEL) {
            score = 2;
        } else if (cls == USB_DEVICE_CLASS_CDC && sub == CDC_NETWORK_CONTROL_MODEL) {
            score = 1;
        }
        best = score > best ? score : best;
    }
    return best;
}

uint8_t usbh_get_hport_active_config_index(struct usbh_hubport *hport)
{
    struct usb_setup_packet *setup = hport->setup;
    uint8_t n = hport->device_desc.bNumConfigurations, pick = 0;
    int best = -1, score0 = -1;

    if (n <= 1) {
        return 0;
    }
    if (n > 4) {
        n = 4;
    }
    for (uint8_t i = 0; i < n; i++) {
        int ret, score;

        setup->bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_DEVICE;
        setup->bRequest = USB_REQUEST_GET_DESCRIPTOR;
        setup->wValue = (uint16_t)((USB_DESCRIPTOR_TYPE_CONFIGURATION << 8) | i);
        setup->wIndex = 0;
        setup->wLength = sizeof(nx_cfg_buf);
        memset(nx_cfg_buf, 0, sizeof(nx_cfg_buf)); /* nothing of the previous index survives */
        ret = usbh_control_transfer(hport, setup, nx_cfg_buf);
        if (ret < 8 + 9 || nx_cfg_buf[1] != USB_DESCRIPTOR_TYPE_CONFIGURATION) {
            continue;
        }
        /* ret counts the 8-byte setup stage on the EHCI ports (usbh_rtl8152.c reads ret - 8);
         * also bounded by wTotalLength, and the cleared buffer ends a short read's walk */
        uint32_t len = (uint32_t)ret - 8U < sizeof(nx_cfg_buf) ? (uint32_t)ret - 8U : sizeof(nx_cfg_buf);
        uint32_t total = (uint32_t)nx_cfg_buf[2] | ((uint32_t)nx_cfg_buf[3] << 8);

        if (total < len) {
            len = total;
        }
        score = nx_cfg_score(hport, nx_cfg_buf, len);
        if (i == 0) {
            score0 = score;
        }
        if (score > best) {
            best = score;
            pick = i;
        }
    }
    if (pick != 0) {
        printf("usbnet: %04x:%04x: configuration index %u (%s) instead of 0 (score %d)\n",
               hport->device_desc.idVendor, hport->device_desc.idProduct, pick,
               best == 3 ? "vendor" : best == 2 ? "CDC-ECM" : "CDC-NCM", score0);
    }
    return pick;
}
