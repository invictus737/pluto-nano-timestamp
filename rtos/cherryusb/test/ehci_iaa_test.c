/*
 * Host simulation of the CherryUSB EHCI port as patched for the Pluto Nano (patches/0001-0004):
 * Interrupt on Async Advance cycles, IAA watchdog, usbh_kill_urb(), barriers, toggle recovery.
 *
 * The driver source is included after run.sh turned its USBSTS/USBCMD stores into calls of the
 * register model below. The model answers a doorbell after a delay (or drops/loses it), keeps
 * USBSTS write-1-to-clear and IAAD impossible to clear by software, and records which QHs
 * were reachable in the asynchronous ring when each doorbell rang. Invariant checked at every
 * completion: the QH was already out of the ring when the doorbell whose IAA was last
 * answered rang (EHCI 4.8.2). The stock handler violates it when USBINT and IAA are handled
 * in one pass (test_stale_iaa).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_hc_ehci.h"

/* ---- register model ---- */

static struct ehci_hcor g_regs __attribute__((aligned(32)));
static uint32_t g_now_us;
enum iaa_mode { IAA_NORMAL, IAA_LOST, IAA_DROP };
static enum iaa_mode g_mode = IAA_NORMAL;
static uint32_t g_iaa_delay_us = 60;
static uint32_t g_rings, g_answered, g_ring_us, g_echo_writes, g_unbarriered_writes;
static uint32_t g_ring_snapshot, g_answered_snapshot;
static uint32_t g_barriers_since_cmd;
static int g_check_barriers;

static uint32_t ring_mask(void);

static void sim_usbsts_write(uint32_t v)
{
    g_regs.usbsts &= ~(v & 0x3fU);
}

static void sim_usbcmd_write(uint32_t v)
{
    uint32_t old = g_regs.usbcmd;

    if (g_check_barriers && g_barriers_since_cmd == 0) {
        g_unbarriered_writes++;
    }
    g_barriers_since_cmd = 0;
    if ((v & EHCI_USBCMD_IAAD) && (old & EHCI_USBCMD_IAAD)) {
        g_echo_writes++;
    }
    if ((v & EHCI_USBCMD_IAAD) && !(old & EHCI_USBCMD_IAAD)) {
        g_rings++;
        g_ring_us = g_now_us;
        g_ring_snapshot = ring_mask();
    }
    if (old & EHCI_USBCMD_IAAD) {
        v |= EHCI_USBCMD_IAAD; /* software cannot clear the doorbell */
    }
    g_regs.usbcmd = v;
}

static void model_tick(void)
{
    if ((g_regs.usbcmd & EHCI_USBCMD_IAAD) && g_now_us - g_ring_us >= g_iaa_delay_us) {
        if (g_mode == IAA_NORMAL) {
            g_regs.usbcmd &= ~EHCI_USBCMD_IAAD;
            g_regs.usbsts |= EHCI_USBSTS_IAA;
            g_answered++;
            g_answered_snapshot = g_ring_snapshot;
        } else if (g_mode == IAA_DROP) {
            g_regs.usbcmd &= ~EHCI_USBCMD_IAAD;
        }
    }
}

/* ---- OSAL / core stubs ---- */

struct sim_sem {
    int count;
};

usb_osal_sem_t usb_osal_sem_create(uint32_t initial_count)
{
    struct sim_sem *s = calloc(1, sizeof(*s));
    s->count = initial_count ? 1 : 0;
    return s;
}
void usb_osal_sem_delete(usb_osal_sem_t sem) { free(sem); }
int usb_osal_sem_take(usb_osal_sem_t sem, uint32_t timeout)
{
    struct sim_sem *s = sem;
    (void)timeout;
    if (s->count) {
        s->count = 0;
        return 0;
    }
    return -USB_ERR_TIMEOUT; /* single-threaded: the wait has elapsed */
}
int usb_osal_sem_give(usb_osal_sem_t sem)
{
    ((struct sim_sem *)sem)->count = 1;
    return 0;
}
void usb_osal_sem_reset(usb_osal_sem_t sem) { ((struct sim_sem *)sem)->count = 0; }

static int g_crit;
size_t usb_osal_enter_critical_section(void) { return ++g_crit; }
void usb_osal_leave_critical_section(size_t flag) { (void)flag; g_crit--; }
static uint32_t g_barriers;
void usb_osal_dma_barrier(void) { g_barriers++; g_barriers_since_cmd++; }
void usb_osal_msleep(uint32_t delay)
{
    for (uint32_t i = 0; i < delay * 1000U; i++) {
        g_now_us++;
        model_tick();
    }
}
uint32_t usb_ehci_time_us(void)
{
    g_now_us++; /* spinning costs time */
    model_tick();
    return g_now_us;
}
void usb_dcache_clean(uintptr_t addr, size_t size) { (void)addr; (void)size; }
void usb_dcache_invalidate(uintptr_t addr, size_t size) { (void)addr; (void)size; }
void usb_dcache_flush(uintptr_t addr, size_t size) { (void)addr; (void)size; }
void usbh_hub_thread_wakeup(struct usbh_hub *hub) { (void)hub; }
uint8_t usbh_get_port_speed(struct usbh_bus *bus, const uint8_t port) { (void)bus; (void)port; return USB_SPEED_HIGH; }
struct usbh_bus g_usbhost_bus[CONFIG_USBHOST_MAX_BUS];

#include "ehci_sim.c"

/* ---- harness ---- */

static int g_fail;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            printf("  FAIL %s:%d: %s\n", __func__, __LINE__, #c);             \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

static uint32_t ring_mask(void)
{
    uint32_t mask = 0;
    struct ehci_qh_hw *head = &g_async_qh_head[0], *qh = EHCI_ADDR2QH(head->hw.hlp);
    int guard = 0;

    while (qh && qh != head && guard++ < 64) {
        long idx = qh - &ehci_qh_pool[0][0];
        if (idx >= 0 && idx < 32) {
            mask |= 1U << idx;
        }
        qh = EHCI_ADDR2QH(qh->hw.hlp);
    }
    return mask;
}

static struct usbh_hub g_roothub;
static struct usbh_hubport g_hport;
static struct usb_endpoint_descriptor g_ep_out = { .bLength = 7, .bDescriptorType = 5,
    .bEndpointAddress = 0x02, .bmAttributes = USB_ENDPOINT_TYPE_BULK, .wMaxPacketSize = 512 };
static uint8_t g_buf[8][2048] __attribute__((aligned(32)));

struct xfer {
    struct usbh_urb urb;
    int qh_index;
    int done;
    int result;
    int order_ok;
};

static void xfer_done(void *arg, int nbytes)
{
    struct xfer *x = arg;

    x->done++;
    x->result = nbytes;
    /* EHCI 4.8.2: out of the ring before the doorbell whose IAA was answered last */
    x->order_ok = g_answered > 0 && !(g_answered_snapshot & (1U << x->qh_index));
}

static void sim_reset(void)
{
    memset(&g_regs, 0, sizeof(g_regs));
    memset(ehci_qh_pool, 0, sizeof(ehci_qh_pool));
    memset(ehci_qtd_pool, 0, sizeof(ehci_qtd_pool));
    memset(g_ehci_iaa, 0, sizeof(g_ehci_iaa));
    memset(g_ehci_iaa_stats, 0, sizeof(g_ehci_iaa_stats));
    g_mode = IAA_NORMAL;
    g_iaa_delay_us = 60;
    g_rings = g_answered = g_echo_writes = g_unbarriered_writes = 0;
    g_answered_snapshot = g_ring_snapshot = 0;
    g_check_barriers = 0;
    g_usbhost_bus[0].hcd.reg_base = (uintptr_t)&g_regs - CONFIG_USB_EHCI_HCCR_OFFSET;
    g_usbhost_bus[0].hcd.hcd_id = 0;
    g_ehci_hcd[0].hcor_offset = 0;
    for (int i = 0; i < CONFIG_USB_EHCI_QH_NUM; i++) {
        ehci_qh_pool[0][i].waitsem = usb_osal_sem_create(0);
    }
    memset(&g_async_qh_head[0], 0, sizeof(g_async_qh_head[0]));
    g_async_qh_head[0].hw.hlp = QH_HLP_QH(&g_async_qh_head[0]);
    g_async_qh_head[0].hw.epchar = QH_EPCHAR_H;
    g_async_qh_head[0].hw.overlay.next_qtd = QTD_LIST_END;
    g_async_qh_head[0].hw.overlay.token = QTD_TOKEN_STATUS_HALTED;
    g_periodic_qh_head[0].hw.hlp = QH_HLP_END;
    g_regs.usbcmd = EHCI_USBCMD_RUN | EHCI_USBCMD_ASEN | EHCI_USBCMD_PSEN;
    g_regs.usbsts = EHCI_USBSTS_ASS;
    g_regs.usbintr = EHCI_USBIE_INT | EHCI_USBIE_ERR | EHCI_USBIE_PCD | EHCI_USBIE_FATAL | EHCI_USBIE_IAA;
    g_regs.portsc[0] = EHCI_PORTSC_CCS;
    memset(&g_roothub, 0, sizeof(g_roothub));
    g_roothub.is_roothub = true;
    memset(&g_hport, 0, sizeof(g_hport));
    g_hport.connected = true;
    g_hport.port = 1;
    g_hport.speed = USB_SPEED_HIGH;
    g_hport.dev_addr = 2;
    g_hport.parent = &g_roothub;
    g_hport.bus = &g_usbhost_bus[0];
}

static void advance(uint32_t us)
{
    for (uint32_t i = 0; i < us; i++) {
        g_now_us++;
        model_tick();
    }
}

static void isr(void)
{
    if (g_regs.usbsts & g_regs.usbintr) {
        USBH_IRQHandler(0);
    }
}

static int submit(struct xfer *x, int slot, uint32_t len, bool toggle)
{
    int rc;

    memset(x, 0, sizeof(*x));
    x->urb.hport = &g_hport;
    x->urb.ep = &g_ep_out;
    x->urb.transfer_buffer = g_buf[slot];
    x->urb.transfer_buffer_length = len;
    x->urb.complete = xfer_done;
    x->urb.arg = x;
    x->urb.data_toggle = toggle;
    g_check_barriers = 1;
    rc = usbh_submit_urb(&x->urb);
    x->qh_index = (int)((struct ehci_qh_hw *)x->urb.hcpriv - &ehci_qh_pool[0][0]);
    return rc;
}

/* the controller finishes every qTD of x; toggle = data toggle after the last packet */
static void hw_finish(struct xfer *x, bool toggle)
{
    struct ehci_qh_hw *qh = &ehci_qh_pool[0][x->qh_index];
    struct ehci_qtd_hw *qtd = EHCI_ADDR2QTD(qh->first_qtd);

    while (qtd) {
        uint32_t t = qtd->hw.token & ~(QTD_TOKEN_STATUS_ACTIVE | QTD_TOKEN_NBYTES_MASK | QTD_TOKEN_TOGGLE);
        qtd->hw.token = t | (toggle ? QTD_TOKEN_TOGGLE : 0);
        qtd = EHCI_ADDR2QTD(qtd->hw.next_qtd);
    }
    qh->hw.overlay.token = (toggle ? QTD_TOKEN_TOGGLE : 0);
    g_regs.usbsts |= EHCI_USBSTS_INT;
}

/* the root cause on the board: USBINT and IAA in one handler pass (handler running late) */
static void test_stale_iaa(void)
{
    struct xfer a, b;

    sim_reset();
    CHECK(submit(&a, 0, 64, false) == 0);
    CHECK(submit(&b, 1, 64, false) == 0);
    hw_finish(&a, true);
    isr();                          /* A unlinked, doorbell 1 */
    CHECK(g_rings == 1 && !a.done);
    advance(100);                   /* doorbell 1 answered, handler not yet run */
    hw_finish(&b, true);            /* B finishes meanwhile */
    isr();                          /* IAA + USBINT together */
    CHECK(a.done == 1 && a.order_ok);
    CHECK(!b.done);                 /* stock code completed (and freed) B here */
    CHECK(g_rings == 2);
    advance(100);
    isr();
    CHECK(b.done == 1 && b.order_ok && b.result == 64);
    CHECK(b.urb.data_toggle == true);
    CHECK(usbh_ehci_iaa_pending(0) == 0);
    CHECK(g_echo_writes == 0 && g_unbarriered_writes == 0);
}

/* several completions in one pass share one doorbell */
static void test_batch(void)
{
    struct xfer x[4];

    sim_reset();
    for (int i = 0; i < 4; i++) {
        CHECK(submit(&x[i], i, 512, false) == 0);
        hw_finish(&x[i], true);
    }
    isr();
    CHECK(g_rings == 1);
    advance(100);
    isr();
    for (int i = 0; i < 4; i++) {
        CHECK(x[i].done == 1 && x[i].order_ok);
    }
}

/* a doorbell the controller never answers: the transfer completes after 10 ms, no error */
static void test_lost_and_dropped(void)
{
    struct xfer c;

    for (int mode = IAA_LOST; mode <= IAA_DROP; mode++) {
        sim_reset();
        g_mode = (enum iaa_mode)mode;
        CHECK(submit(&c, 0, 64, false) == 0);
        hw_finish(&c, false);
        isr();
        advance(9000);
        CHECK(usbh_ehci_iaa_watchdog(0) == 0 && !c.done);
        advance(1100);
        CHECK(usbh_ehci_iaa_watchdog(0) == 1);
        CHECK(c.done == 1 && c.result == 64);
        CHECK(g_ehci_iaa_stats[0].watchdog_ends == 1);
        CHECK(mode == IAA_LOST ? g_ehci_iaa_stats[0].late == 1 : g_ehci_iaa_stats[0].lost == 1);
    }
}

/* kill of a NAKing OUT: one packet acknowledged before; toggle comes from the overlay */
static void test_kill_toggle(void)
{
    struct xfer d;
    struct ehci_qh_hw *qh;

    sim_reset();
    CHECK(submit(&d, 0, 1024, false) == 0);
    qh = &ehci_qh_pool[0][d.qh_index];
    qh->hw.overlay.token = QTD_TOKEN_TOGGLE | QTD_TOKEN_STATUS_ACTIVE; /* DATA1 next */
    CHECK(usbh_kill_urb(&d.urb) == 0);
    CHECK(d.done == 1 && d.result == -USB_ERR_SHUTDOWN && d.order_ok);
    CHECK(d.urb.data_toggle == true);
    CHECK(!qh->inuse);
}

/* kill with a controller that never answers: bounded by the watchdog, still returns 0 */
static void test_kill_lost(void)
{
    struct xfer e;
    uint32_t t0;

    sim_reset();
    g_mode = IAA_LOST;
    CHECK(submit(&e, 0, 64, false) == 0);
    t0 = g_now_us;
    CHECK(usbh_kill_urb(&e.urb) == 0);
    CHECK(e.done == 1 && e.result == -USB_ERR_SHUTDOWN);
    CHECK(g_now_us - t0 <= 12000);
    CHECK(g_ehci_iaa_stats[0].watchdog_ends == 1);
}

/* kill of a transfer that already finished and waits for its IAA: its own result */
static void test_kill_finished(void)
{
    struct xfer f;

    sim_reset();
    CHECK(submit(&f, 0, 64, false) == 0);
    hw_finish(&f, true);
    isr();
    CHECK(!f.done);
    CHECK(usbh_kill_urb(&f.urb) == 0);
    CHECK(f.done == 1 && f.result == 64 && f.order_ok);
    CHECK(usbh_kill_urb(&f.urb) == -USB_ERR_INVAL); /* handed back: nothing left */
}

/* a kill while another cycle runs: that cycle ends first, then a new one for the killed QH */
static void test_kill_during_cycle(void)
{
    struct xfer g, h;

    sim_reset();
    g_iaa_delay_us = 300;
    CHECK(submit(&g, 0, 64, false) == 0);
    CHECK(submit(&h, 1, 64, false) == 0);
    hw_finish(&g, false);
    isr(); /* cycle 1 for G */
    CHECK(usbh_kill_urb(&h.urb) == 0);
    CHECK(g.done == 1 && g.order_ok);
    CHECK(h.done == 1 && h.order_ok && h.result == -USB_ERR_SHUTDOWN);
    CHECK(g_rings == 2 && g_echo_writes == 0);
}

/* submitting while a doorbell is pending writes USBCMD without IAAD */
static void test_no_doorbell_echo(void)
{
    struct xfer i1, i2;

    sim_reset();
    g_iaa_delay_us = 500;
    CHECK(submit(&i1, 0, 64, false) == 0);
    hw_finish(&i1, false);
    isr();
    CHECK(g_regs.usbcmd & EHCI_USBCMD_IAAD);
    CHECK(submit(&i2, 1, 64, false) == 0);
    CHECK(g_echo_writes == 0 && g_rings == 1);
    CHECK(g_unbarriered_writes == 0);
}

/* halted controller: QHs are handed back at once (Linux start_iaa_cycle, rh_state) */
static void test_halted(void)
{
    struct xfer j;

    sim_reset();
    CHECK(submit(&j, 0, 64, false) == 0);
    hw_finish(&j, false);
    g_regs.usbsts |= EHCI_USBSTS_HALTED;
    isr();
    CHECK(j.done == 1 && g_rings == 0 && g_ehci_iaa_stats[0].not_running == 1);
}

/* a semaphore give that raced with its owner's timeout does not end the next owner's URB */
static void test_sem_reset(void)
{
    struct ehci_qh_hw *qh;

    sim_reset();
    qh = ehci_qh_alloc(&g_usbhost_bus[0]);
    usb_osal_sem_give(qh->waitsem);
    ehci_qh_free(&g_usbhost_bus[0], qh);
    qh = ehci_qh_alloc(&g_usbhost_bus[0]);
    CHECK(usb_osal_sem_take(qh->waitsem, 1) != 0);
}

/* synchronous URB that times out: the kill hands it back, -USB_ERR_TIMEOUT, QH free */
static void test_sync_timeout(void)
{
    struct xfer k;
    int rc;

    sim_reset();
    memset(&k, 0, sizeof(k));
    k.urb.hport = &g_hport;
    k.urb.ep = &g_ep_out;
    k.urb.transfer_buffer = g_buf[0];
    k.urb.transfer_buffer_length = 64;
    k.urb.timeout = 100;
    rc = usbh_submit_urb(&k.urb);
    CHECK(rc == -USB_ERR_TIMEOUT);
    CHECK(k.urb.hcpriv == NULL);
    for (int i = 0; i < CONFIG_USB_EHCI_QH_NUM; i++) {
        CHECK(!ehci_qh_pool[0][i].inuse);
    }
}

int main(void)
{
    test_stale_iaa();
    test_batch();
    test_lost_and_dropped();
    test_kill_toggle();
    test_kill_lost();
    test_kill_finished();
    test_kill_during_cycle();
    test_no_doorbell_echo();
    test_halted();
    test_sem_reset();
    test_sync_timeout();
    printf("ehci_iaa_test: %s (%d failure(s))\n", g_fail ? "FAIL" : "ok", g_fail);
    return g_fail ? 1 : 0;
}
