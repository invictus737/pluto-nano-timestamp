/*
 * Host fakes for the usbnet glue test (test_usbnet.c): the four CherryUSB USB Ethernet class
 * drivers as seen by net/usbnet.c, an Ethernet device model behind each, and the libbsd
 * aicwf_ifnet_* ifnet API. Real pthreads, so the glue's locking, thread start/exit and
 * detach ordering run as on the target (ASan/LSan or TSan catch what they get wrong).
 *
 * Driver model (mirrors CherryUSB v1.6.1 + net/patches):
 *  - connect(): class object cleared and filled, devname set, then usbh_X_run()
 *  - disconnect(): hport->connected = false (usbh_hubport_release does it first), the class
 *    URBs killed, usbh_X_stop(), class object cleared
 *  - rx thread: find_class (gone -> usb_osal_thread_delete(NULL) = pthread_exit), wait for the
 *    link on the interrupt endpoint, then one blocking bulk IN at a time -> usbh_X_eth_input
 *  - get_connect_status: CDC blocks until a notification (or kill), RTL/ASIX answer at once
 *  - eth_output: refuses when !connect_status or disconnected; records every transfer
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "usbh_core.h"
#include "usbh_rtl8152.h"
#include "usbh_cdc_ecm.h"
#include "usbh_cdc_ncm.h"
#include "usbh_asix.h"
#include "aicwf_usbif.h"

#include "fake_usbnet.h"

static void fdie(const char *what)
{
    fprintf(stderr, "FATAL: %s\n", what);
    abort();
}

void usb_osal_thread_delete(usb_osal_thread_t thread)
{
    if (thread == NULL) {
        pthread_exit(NULL);
    }
}

void usb_osal_msleep(uint32_t ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- device models ---- */

#define RXQ 64
struct fdev {
    const char *name;
    int kind;          /* FK_* */
    pthread_mutex_t m;
    pthread_cond_t c;
    struct usbh_hubport hp;
    struct usb_setup_packet setup;
    struct usb_endpoint_descriptor bulkout_ep;
    bool devname_set;
    bool link;         /* cable */
    int notif;         /* CDC notifications pending */
    unsigned kill_gen; /* bumped by disconnect's usbh_kill_urb */
    uint16_t rxlen[RXQ];
    uint32_t rxseq[RXQ];
    unsigned rx_head, rx_n;
    /* counters */
    unsigned rx_threads_alive, rx_threads_max, rx_threads_started;
    unsigned tx_ok, tx_refused, tx_bad_pad, tx_bad_data;
    uint8_t txbuf[2048 + 64];
};

static struct fdev g_fdev[FK_N] = {
    [FK_RTL] = { .name = "rtl8152", .kind = FK_RTL, .m = PTHREAD_MUTEX_INITIALIZER, .c = PTHREAD_COND_INITIALIZER },
    [FK_ECM] = { .name = "cdc_ecm", .kind = FK_ECM, .m = PTHREAD_MUTEX_INITIALIZER, .c = PTHREAD_COND_INITIALIZER },
    [FK_NCM] = { .name = "cdc_ncm", .kind = FK_NCM, .m = PTHREAD_MUTEX_INITIALIZER, .c = PTHREAD_COND_INITIALIZER },
    [FK_ASIX] = { .name = "asix", .kind = FK_ASIX, .m = PTHREAD_MUTEX_INITIALIZER, .c = PTHREAD_COND_INITIALIZER },
};

static const uint8_t fk_hdr[FK_N] = { 8, 0, 16, 4 };
static const bool fk_blocking_status[FK_N] = { false, true, true, false };

/* the glue's hooks (net/usbnet.c) */
#define HOOKS(id, T)                                    \
    void usbh_##id##_run(T *c);                         \
    void usbh_##id##_stop(T *c);                        \
    void usbh_##id##_eth_input(uint8_t *b, uint32_t n);
HOOKS(rtl8152, struct usbh_rtl8152)
HOOKS(cdc_ecm, struct usbh_cdc_ecm)
HOOKS(cdc_ncm, struct usbh_cdc_ncm)
HOOKS(asix, struct usbh_asix)

static int f_status(struct fdev *d, bool *connect_status)
{
    int rc = 0;

    pthread_mutex_lock(&d->m);
    unsigned kg = d->kill_gen;
    if (!d->hp.connected) {
        rc = -USB_ERR_NOTCONN;
    } else if (fk_blocking_status[d->kind]) {
        while (d->notif == 0 && d->hp.connected && d->kill_gen == kg) {
            pthread_cond_wait(&d->c, &d->m);
        }
        if (d->notif > 0) {
            d->notif--;
            *connect_status = d->link;
        } else {
            rc = d->hp.connected ? -USB_ERR_SHUTDOWN : -USB_ERR_NOTCONN;
        }
    } else {
        *connect_status = d->link;
    }
    pthread_mutex_unlock(&d->m);
    return rc;
}

/* blocking bulk IN: a frame, or < 0 when killed / disconnected */
static int f_bulkin(struct fdev *d, uint8_t *buf, uint32_t *len)
{
    int rc = -USB_ERR_NOTCONN;

    pthread_mutex_lock(&d->m);
    unsigned kg = d->kill_gen;
    while (d->hp.connected && d->kill_gen == kg && d->rx_n == 0) {
        pthread_cond_wait(&d->c, &d->m);
    }
    if (d->hp.connected && d->kill_gen == kg && d->rx_n > 0) {
        unsigned i = d->rx_head;
        uint32_t n = d->rxlen[i];

        fake_frame(buf, n, d->rxseq[i]);
        d->rx_head = (d->rx_head + 1) % RXQ;
        d->rx_n--;
        *len = n;
        rc = 0;
        pthread_cond_broadcast(&d->c);
    }
    pthread_mutex_unlock(&d->m);
    return rc;
}

static int f_output(struct fdev *d, bool connect_status, uint32_t len)
{
    pthread_mutex_lock(&d->m);
    if (!connect_status || !d->hp.connected) {
        d->tx_refused++;
        pthread_mutex_unlock(&d->m);
        return -USB_ERR_NOTCONN;
    }
    uint32_t xfer = d->kind == FK_NCM ? 32 + ((len + 3) & ~3U) : fk_hdr[d->kind] + len;
    if (d->kind != FK_ASIX && (xfer % 512) == 0) {
        d->tx_bad_pad++;
    }
    if (fake_frame_check(d->txbuf + fk_hdr[d->kind], len) != 0) {
        d->tx_bad_data++;
    }
    d->tx_ok++;
    pthread_mutex_unlock(&d->m);
    return 0;
}

#define DRIVER(id, T, K)                                                                       \
    static T g_##id;                                                                           \
    int usbh_##id##_get_connect_status(T *c)                                                   \
    {                                                                                          \
        bool cs = false;                                                                       \
        int rc = f_status(&g_fdev[K], &cs);                                                    \
        if (rc == 0) {                                                                         \
            c->connect_status = cs;                                                            \
        }                                                                                      \
        return rc;                                                                             \
    }                                                                                          \
    uint8_t *usbh_##id##_get_eth_txbuf(void) { return g_fdev[K].txbuf + fk_hdr[K]; }           \
    int usbh_##id##_eth_output(uint32_t len) { return f_output(&g_fdev[K], g_##id.connect_status, len); } \
    void usbh_##id##_rx_thread(CONFIG_USB_OSAL_THREAD_SET_ARGV)                               \
    {                                                                                          \
        struct fdev *d = &g_fdev[K];                                                           \
        static uint8_t buf[2048];                                                              \
        (void)argument;                                                                        \
        pthread_mutex_lock(&d->m);                                                             \
        d->rx_threads_alive++;                                                                 \
        d->rx_threads_started++;                                                               \
        if (d->rx_threads_alive > d->rx_threads_max)                                           \
            d->rx_threads_max = d->rx_threads_alive;                                           \
        pthread_mutex_unlock(&d->m);                                                           \
    find_class:                                                                                \
        g_##id.connect_status = false;                                                         \
        pthread_mutex_lock(&d->m);                                                             \
        if (!(d->hp.connected && d->devname_set)) {                                            \
            d->rx_threads_alive--;                                                             \
            pthread_cond_broadcast(&d->c);                                                     \
            pthread_mutex_unlock(&d->m);                                                       \
            usb_osal_thread_delete(NULL);                                                      \
        }                                                                                      \
        pthread_mutex_unlock(&d->m);                                                           \
        while (g_##id.connect_status == false) {                                               \
            if (usbh_##id##_get_connect_status(&g_##id) < 0) {                                 \
                usb_osal_msleep(2);                                                            \
                goto find_class;                                                               \
            }                                                                                  \
            if (!g_##id.connect_status)                                                        \
                usb_osal_msleep(2);                                                            \
        }                                                                                      \
        for (;;) {                                                                             \
            uint32_t n;                                                                        \
            if (f_bulkin(d, buf, &n) < 0)                                                      \
                goto find_class;                                                               \
            usbh_##id##_eth_input(buf, n + (K == FK_RTL ? 4 : 0)); /* RTL: + FCS */            \
        }                                                                                      \
    }                                                                                          \
    static void id##_connect(void)                                                             \
    {                                                                                          \
        struct fdev *d = &g_fdev[K];                                                           \
        pthread_mutex_lock(&d->m);                                                             \
        memset(&g_##id, 0, sizeof(g_##id));                                                    \
        d->hp.connected = true;                                                                \
        d->hp.port = 1;                                                                        \
        d->hp.setup = &d->setup;                                                               \
        d->bulkout_ep.wMaxPacketSize = 512;                                                    \
        g_##id.hport = &d->hp;                                                                 \
        g_##id.bulkout = &d->bulkout_ep;                                                       \
        for (int i = 0; i < 6; i++)                                                            \
            g_##id.mac[i] = (uint8_t)(0x10 * K + i + 2);                                       \
        d->devname_set = true;                                                                 \
        d->rx_head = d->rx_n = 0;                                                              \
        d->notif = 0;                                                                          \
        pthread_mutex_unlock(&d->m);                                                           \
        usbh_##id##_run(&g_##id);                                                              \
    }                                                                                          \
    static void id##_disconnect(void)                                                          \
    {                                                                                          \
        struct fdev *d = &g_fdev[K];                                                           \
        pthread_mutex_lock(&d->m);                                                             \
        d->hp.connected = false;                                                               \
        d->kill_gen++;                                                                         \
        pthread_cond_broadcast(&d->c);                                                         \
        bool named = d->devname_set;                                                           \
        pthread_mutex_unlock(&d->m);                                                           \
        sched_yield();                                                                         \
        if (named)                                                                             \
            usbh_##id##_stop(&g_##id);                                                         \
        pthread_mutex_lock(&d->m);                                                             \
        memset(&g_##id, 0, sizeof(g_##id));                                                    \
        d->devname_set = false;                                                                \
        pthread_mutex_unlock(&d->m);                                                           \
    }

DRIVER(rtl8152, struct usbh_rtl8152, FK_RTL)
DRIVER(cdc_ecm, struct usbh_cdc_ecm, FK_ECM)
DRIVER(cdc_ncm, struct usbh_cdc_ncm, FK_NCM)
DRIVER(asix, struct usbh_asix, FK_ASIX)

void fake_plug(int k)
{
    switch (k) {
    case FK_RTL: rtl8152_connect(); break;
    case FK_ECM: cdc_ecm_connect(); break;
    case FK_NCM: cdc_ncm_connect(); break;
    default: asix_connect(); break;
    }
}

void fake_unplug(int k)
{
    switch (k) {
    case FK_RTL: rtl8152_disconnect(); break;
    case FK_ECM: cdc_ecm_disconnect(); break;
    case FK_NCM: cdc_ncm_disconnect(); break;
    default: asix_disconnect(); break;
    }
}

void fake_cable(int k, bool up)
{
    struct fdev *d = &g_fdev[k];

    pthread_mutex_lock(&d->m);
    d->link = up;
    d->notif++;
    pthread_cond_broadcast(&d->c);
    pthread_mutex_unlock(&d->m);
}

int fake_rx_push(int k, uint32_t len, uint32_t seq)
{
    struct fdev *d = &g_fdev[k];
    int rc = -1;

    pthread_mutex_lock(&d->m);
    if (d->rx_n < RXQ) {
        unsigned i = (d->rx_head + d->rx_n) % RXQ;
        d->rxlen[i] = (uint16_t)len;
        d->rxseq[i] = seq;
        d->rx_n++;
        rc = 0;
        pthread_cond_broadcast(&d->c);
    }
    pthread_mutex_unlock(&d->m);
    return rc;
}

unsigned fake_rx_pending(int k)
{
    struct fdev *d = &g_fdev[k];
    pthread_mutex_lock(&d->m);
    unsigned n = d->rx_n;
    pthread_mutex_unlock(&d->m);
    return n;
}

void fake_dev_stats(int k, struct fake_dev_stats *st)
{
    struct fdev *d = &g_fdev[k];

    pthread_mutex_lock(&d->m);
    st->rx_threads_alive = d->rx_threads_alive;
    st->rx_threads_max = d->rx_threads_max;
    st->rx_threads_started = d->rx_threads_started;
    st->tx_ok = d->tx_ok;
    st->tx_refused = d->tx_refused;
    st->tx_bad_pad = d->tx_bad_pad;
    st->tx_bad_data = d->tx_bad_data;
    pthread_mutex_unlock(&d->m);
}

/* wait until the device's rx threads are all gone (ms bound); 0 if they are */
int fake_wait_rx_gone(int k, unsigned ms)
{
    struct fdev *d = &g_fdev[k];
    int rc;

    for (unsigned t = 0; t < ms; t++) {
        pthread_mutex_lock(&d->m);
        rc = d->rx_threads_alive == 0 ? 0 : -1;
        pthread_mutex_unlock(&d->m);
        if (rc == 0) {
            return 0;
        }
        usb_osal_msleep(1);
    }
    return -1;
}

struct usbh_hubport *fake_hport(int k)
{
    return &g_fdev[k].hp;
}

/* ---- frames ---- */

void fake_frame(uint8_t *buf, uint32_t len, uint32_t seq)
{
    memset(buf, 0xff, 6);                  /* broadcast */
    memset(buf + 6, 0x02, 6);
    buf[12] = 0x88;                        /* local experimental ethertype */
    buf[13] = 0xb5;
    buf[14] = (uint8_t)(len >> 8);
    buf[15] = (uint8_t)len;
    buf[16] = (uint8_t)(seq >> 24);
    buf[17] = (uint8_t)(seq >> 16);
    buf[18] = (uint8_t)(seq >> 8);
    buf[19] = (uint8_t)seq;
    for (uint32_t i = 20; i < len; i++) {
        buf[i] = (uint8_t)(seq * 7U + i);
    }
}

int fake_frame_check(const uint8_t *buf, uint32_t len)
{
    if (len < 20) {
        return -1;
    }
    uint32_t flen = ((uint32_t)buf[14] << 8) | buf[15];
    uint32_t seq = ((uint32_t)buf[16] << 24) | ((uint32_t)buf[17] << 16) | ((uint32_t)buf[18] << 8) | buf[19];
    if (flen > len || len > flen + 4) { /* at most the pad byte(s) after the frame */
        return -1;
    }
    for (uint32_t i = 20; i < flen; i++) {
        if (buf[i] != (uint8_t)(seq * 7U + i)) {
            return -1;
        }
    }
    for (uint32_t i = flen; i < len; i++) {
        if (buf[i] != 0) {
            return -1;
        }
    }
    return 0;
}

/* ---- control transfers (configuration choice test) ---- */

static const uint8_t *g_cfg_desc[4];
static uint16_t g_cfg_len[4];
unsigned g_ctrl_count;

void fake_set_configs(int k, uint16_t vid, uint16_t pid, int n, const uint8_t *const desc[], const uint16_t len[])
{
    struct fdev *d = &g_fdev[k];

    d->hp.device_desc.idVendor = vid;
    d->hp.device_desc.idProduct = pid;
    d->hp.device_desc.bNumConfigurations = (uint8_t)n;
    d->hp.setup = &d->setup;
    d->hp.connected = true;
    for (int i = 0; i < 4; i++) {
        g_cfg_desc[i] = i < n ? desc[i] : NULL;
        g_cfg_len[i] = i < n ? len[i] : 0;
    }
    g_ctrl_count = 0;
}

int usbh_control_transfer(struct usbh_hubport *hport, struct usb_setup_packet *setup, uint8_t *buffer)
{
    uint8_t idx = setup->wValue & 0xff;

    if (!hport->connected) {
        return -USB_ERR_NODEV;
    }
    g_ctrl_count++;
    if ((setup->wValue >> 8) != USB_DESCRIPTOR_TYPE_CONFIGURATION || idx >= 4 || g_cfg_desc[idx] == NULL) {
        return -USB_ERR_STALL;
    }
    uint16_t n = g_cfg_len[idx] < setup->wLength ? g_cfg_len[idx] : setup->wLength;
    memcpy(buffer, g_cfg_desc[idx], n);
    return n + 8; /* this port's ehci counts the setup stage (usbh_rtl8152.c reads ret - 8) */
}

/* ---- libbsd ifnet (aicwf_ifnet_*) ---- */

#define IFN_LIVE 0x1f7e11feU
#define IFN_DEAD 0xdeadbeefU
#define TXQ 64

struct aicwf_ifnet {
    uint32_t magic;
    char name[16];
    uint8_t mac[6];
    aicwf_ifnet_tx_t tx;
    void *arg;
    pthread_mutex_t m;
    pthread_cond_t c;
    pthread_t worker;
    bool dying;
    uint8_t q[TXQ][1600];
    uint16_t qlen[TXQ];
    unsigned qh, qn;
    int link;
    struct aicwf_ifnet *next;
};

static pthread_mutex_t g_ifs_lock = PTHREAD_MUTEX_INITIALIZER;
static struct aicwf_ifnet *g_ifs;
static struct fake_if_stats g_ifstat;

static void *if_worker(void *a)
{
    struct aicwf_ifnet *ifn = a;
    static __thread uint8_t frame[1600];

    pthread_mutex_lock(&ifn->m);
    for (;;) {
        while (!ifn->dying && ifn->qn == 0) {
            pthread_cond_wait(&ifn->c, &ifn->m);
        }
        if (ifn->dying) {
            break;
        }
        uint16_t n = ifn->qlen[ifn->qh];
        memcpy(frame, ifn->q[ifn->qh], n);
        ifn->qh = (ifn->qh + 1) % TXQ;
        ifn->qn--;
        pthread_mutex_unlock(&ifn->m);
        int rc = ifn->tx(ifn->arg, frame, n); /* the glue's transmit, as the taskqueue does */
        pthread_mutex_lock(&g_ifs_lock);
        if (rc == 0) {
            g_ifstat.tx_ok++;
        } else {
            g_ifstat.tx_err++;
        }
        pthread_mutex_unlock(&g_ifs_lock);
        pthread_mutex_lock(&ifn->m);
    }
    pthread_mutex_unlock(&ifn->m);
    return NULL;
}

struct aicwf_ifnet *aicwf_ifnet_attach(const char *name, const uint8_t mac[6], aicwf_ifnet_tx_t tx, void *arg)
{
    struct aicwf_ifnet *ifn = calloc(1, sizeof(*ifn));

    if (ifn == NULL || tx == NULL) {
        fdie("attach");
    }
    pthread_mutex_lock(&g_ifs_lock);
    for (struct aicwf_ifnet *i = g_ifs; i != NULL; i = i->next) {
        if (strcmp(i->name, name) == 0) {
            fdie("duplicate interface name");
        }
    }
    ifn->magic = IFN_LIVE;
    snprintf(ifn->name, sizeof(ifn->name), "%s", name);
    memcpy(ifn->mac, mac, 6);
    ifn->tx = tx;
    ifn->arg = arg;
    ifn->link = -1;
    pthread_mutex_init(&ifn->m, NULL);
    pthread_cond_init(&ifn->c, NULL);
    pthread_create(&ifn->worker, NULL, if_worker, ifn);
    ifn->next = g_ifs;
    g_ifs = ifn;
    g_ifstat.attaches++;
    pthread_mutex_unlock(&g_ifs_lock);
    return ifn;
}

int aicwf_ifnet_input(struct aicwf_ifnet *ifn, const void *frame, uint32_t len)
{
    if (ifn == NULL || ifn->magic != IFN_LIVE) {
        fdie("input on a detached ifnet");
    }
    pthread_mutex_lock(&g_ifs_lock);
    if (fake_frame_check(frame, len) != 0 || (((const uint8_t *)frame)[14] << 8 | ((const uint8_t *)frame)[15]) != len) {
        g_ifstat.rx_bad++;
    } else {
        g_ifstat.rx_ok++;
    }
    pthread_mutex_unlock(&g_ifs_lock);
    return 0;
}

void aicwf_ifnet_link(struct aicwf_ifnet *ifn, int up)
{
    if (ifn == NULL || ifn->magic != IFN_LIVE) {
        fdie("link change on a detached ifnet");
    }
    pthread_mutex_lock(&g_ifs_lock);
    ifn->link = up;
    if (up) {
        g_ifstat.link_ups++;
    } else {
        g_ifstat.link_downs++;
    }
    pthread_mutex_unlock(&g_ifs_lock);
}

void aicwf_ifnet_detach(struct aicwf_ifnet *ifn)
{
    if (ifn == NULL || ifn->magic != IFN_LIVE) {
        fdie("detach of a detached ifnet");
    }
    pthread_mutex_lock(&g_ifs_lock);
    for (struct aicwf_ifnet **p = &g_ifs; *p != NULL; p = &(*p)->next) {
        if (*p == ifn) {
            *p = ifn->next;
            break;
        }
    }
    g_ifstat.detaches++;
    pthread_mutex_unlock(&g_ifs_lock);
    pthread_mutex_lock(&ifn->m);
    ifn->dying = true;
    pthread_cond_broadcast(&ifn->c);
    pthread_mutex_unlock(&ifn->m);
    pthread_join(ifn->worker, NULL); /* taskqueue_drain */
    ifn->magic = IFN_DEAD;
    pthread_mutex_destroy(&ifn->m);
    pthread_cond_destroy(&ifn->c);
    free(ifn);
}

int fake_if_transmit(const char *name, uint32_t len, uint32_t seq)
{
    int rc = -1;

    pthread_mutex_lock(&g_ifs_lock);
    for (struct aicwf_ifnet *i = g_ifs; i != NULL; i = i->next) {
        if (strcmp(i->name, name) == 0) {
            pthread_mutex_lock(&i->m);
            if (!i->dying && i->qn < TXQ) {
                unsigned k = (i->qh + i->qn) % TXQ;
                fake_frame(i->q[k], len, seq);
                i->qlen[k] = (uint16_t)len;
                i->qn++;
                pthread_cond_broadcast(&i->c);
                rc = 0;
            }
            pthread_mutex_unlock(&i->m);
            break;
        }
    }
    pthread_mutex_unlock(&g_ifs_lock);
    return rc;
}

int fake_if_link(const char *name)
{
    int link = -2;

    pthread_mutex_lock(&g_ifs_lock);
    for (struct aicwf_ifnet *i = g_ifs; i != NULL; i = i->next) {
        if (strcmp(i->name, name) == 0) {
            link = i->link;
        }
    }
    pthread_mutex_unlock(&g_ifs_lock);
    return link;
}

int fake_if_count(void)
{
    int n = 0;

    pthread_mutex_lock(&g_ifs_lock);
    for (struct aicwf_ifnet *i = g_ifs; i != NULL; i = i->next) {
        n++;
    }
    pthread_mutex_unlock(&g_ifs_lock);
    return n;
}

void fake_if_stats(struct fake_if_stats *st)
{
    pthread_mutex_lock(&g_ifs_lock);
    *st = g_ifstat;
    pthread_mutex_unlock(&g_ifs_lock);
}
