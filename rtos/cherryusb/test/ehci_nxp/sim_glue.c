/*
 * OSAL, port glue and register hooks for the host test of the ehci_nxp driver.
 *
 * Single-threaded and deterministic: time is simulated (sim_hc.c). A thread that blocks
 * (semaphore wait, sleep) advances simulated time; the "usbh_irq" thread is modelled by
 * running USBH_IRQHandler whenever the line is up and no thread holds the OSAL critical
 * section: at microframe boundaries while a caller waits or sleeps, and when the outermost
 * critical section is left (cfg.preempt), never inside a register access.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usbh_core.h"
#include "usb_hc_ehci.h"
#include "sim_hc.h"

/* ---- register hooks (sim_regs.h) ---- */
static void reg_range(uintptr_t a)
{
    if (a < SIM_REG_BASE || a >= SIM_REG_BASE + 4096) {
        fprintf(stderr, "driver register access at %#lx outside the controller block\n", (unsigned long)a);
        abort();
    }
}

uint32_t sim_drv_reg_read(uintptr_t a)
{
    uint32_t v;
    reg_range(a);
    g_sim_in_mmio++;
    v = sim_reg_read((uint32_t)(a - SIM_REG_BASE), 4);
    g_sim_in_mmio--;
    return v;
}

void sim_drv_reg_write(uintptr_t a, uint32_t v)
{
    reg_range(a);
    g_sim_in_mmio++;
    sim_reg_write((uint32_t)(a - SIM_REG_BASE), v, 4);
    g_sim_in_mmio--;
}

/* ---- glue (port/usb_glue_zynq.c equivalents) ---- */
uint32_t g_sim_hub_wakeups;

uint32_t usb_ehci_time_us(void)
{
    g_sim_in_mmio++;
    sim_advance_ns(50);
    g_sim_in_mmio--;
    return sim_now_us();
}

void usb_hc_low_level_init(struct usbh_bus *bus) { (void)bus; }

void usb_hc_low_level2_init(struct usbh_bus *bus)
{
    uint32_t v;
    (void)bus;
    /* USBMODE host, PORTSC PTS = ULPI without writing the W1C bits back (as the Zynq glue) */
    v = sim_drv_reg_read(SIM_REG_BASE + 0x1a8);
    sim_drv_reg_write(SIM_REG_BASE + 0x1a8, (v & ~0x13U) | 3U);
    v = sim_drv_reg_read(SIM_REG_BASE + 0x184);
    if ((v & 0xD2000000U) != 0x80000000U) {
        v &= ~(0xD2000000U | 0x2aU);
        v |= 0x80000000U;
        sim_drv_reg_write(SIM_REG_BASE + 0x184, v);
    }
}

void usb_hc_low_level_deinit(struct usbh_bus *bus) { (void)bus; }

uint8_t usbh_get_port_speed(struct usbh_bus *bus, const uint8_t port)
{
    uint32_t pspd = (sim_drv_reg_read(SIM_REG_BASE + 0x184) >> 26) & 3U;
    (void)bus;
    (void)port;
    return pspd == 2 ? USB_SPEED_HIGH : pspd == 1 ? USB_SPEED_LOW : USB_SPEED_FULL;
}

void usbh_hub_thread_wakeup(struct usbh_hub *hub)
{
    (void)hub;
    g_sim_hub_wakeups++;
}

/* ---- critical section ---- */
uint32_t g_sim_barriers;

size_t usb_osal_enter_critical_section(void)
{
    return (size_t)++g_sim_crit;
}

void usb_osal_leave_critical_section(size_t flag)
{
    (void)flag;
    if (g_sim_crit <= 0) {
        sim_viol(V_MISC, "critical section left more often than entered");
        return;
    }
    if (--g_sim_crit == 0) {
        sim_preempt_point();
    }
}

size_t usb_osal_crit_suspend(void)
{
    size_t d = (size_t)g_sim_crit;
    g_sim_crit = 0;
    return d;
}

void usb_osal_crit_resume(size_t depth) { g_sim_crit = (int)depth; }

void usb_osal_dma_barrier(void) { g_sim_barriers++; }

/* ---- time ---- */
void usb_osal_msleep(uint32_t delay)
{
    if (g_sim_crit) {
        sim_viol(V_SLEEP_IN_CRIT, "usb_osal_msleep(%u) with the critical section held (depth %d)", delay, g_sim_crit);
    }
    if (g_sim_in_isr) {
        sim_viol(V_SLEEP_IN_CRIT, "usb_osal_msleep(%u) in the interrupt handler", delay);
    }
    sim_advance_ms(delay);
}

/* ---- semaphores ---- */
struct sim_sem {
    uint32_t count, max;
};
uint32_t g_sim_sems;

usb_osal_sem_t usb_osal_sem_create(uint32_t initial_count)
{
    struct sim_sem *s = calloc(1, sizeof(*s));
    s->count = initial_count ? 1 : 0;
    s->max = 1;
    g_sim_sems++;
    return s;
}

usb_osal_sem_t usb_osal_sem_create_counting(uint32_t max_count)
{
    struct sim_sem *s = calloc(1, sizeof(*s));
    s->max = max_count;
    g_sim_sems++;
    return s;
}

void usb_osal_sem_delete(usb_osal_sem_t sem)
{
    if (sem) {
        g_sim_sems--;
    }
    free(sem);
}

int usb_osal_sem_take(usb_osal_sem_t sem, uint32_t timeout)
{
    struct sim_sem *s = sem;
    uint64_t end;

    if (s->count) {
        s->count--;
        return 0;
    }
    if (g_sim_in_isr) {
        sim_viol(V_SLEEP_IN_CRIT, "semaphore wait in the interrupt handler");
        return -USB_ERR_TIMEOUT;
    }
    if (g_sim_crit) {
        sim_viol(V_SLEEP_IN_CRIT, "semaphore wait (%u ms) with the critical section held: the "
                 "interrupt thread cannot run", timeout);
    }
    if (timeout == USB_OSAL_WAITING_FOREVER || timeout > 60000) {
        timeout = 60000;
    }
    end = sim_now_ns() + (uint64_t)timeout * 1000000ULL;
    while (sim_now_ns() < end) {
        sim_advance_ns(125000);
        if (s->count) {
            s->count--;
            return 0;
        }
    }
    return -USB_ERR_TIMEOUT;
}

int usb_osal_sem_give(usb_osal_sem_t sem)
{
    struct sim_sem *s = sem;
    if (s->count < s->max) {
        s->count++;
    }
    return 0;
}

void usb_osal_sem_reset(usb_osal_sem_t sem) { ((struct sim_sem *)sem)->count = 0; }

/* ---- mutexes, queues, threads, timers: not used by the driver; minimal ---- */
usb_osal_mutex_t usb_osal_mutex_create(void) { return calloc(1, 8); }
void usb_osal_mutex_delete(usb_osal_mutex_t m) { free(m); }
int usb_osal_mutex_take(usb_osal_mutex_t m) { (void)m; return 0; }
int usb_osal_mutex_give(usb_osal_mutex_t m) { (void)m; return 0; }

struct sim_mq {
    uintptr_t q[64];
    uint32_t h, t;
};
usb_osal_mq_t usb_osal_mq_create(uint32_t max_msgs) { (void)max_msgs; return calloc(1, sizeof(struct sim_mq)); }
void usb_osal_mq_delete(usb_osal_mq_t mq) { free(mq); }
int usb_osal_mq_send(usb_osal_mq_t mq, uintptr_t addr)
{
    struct sim_mq *m = mq;
    if (m->t - m->h >= 64) {
        return -USB_ERR_BUSY;
    }
    m->q[m->t++ % 64] = addr;
    return 0;
}
int usb_osal_mq_recv(usb_osal_mq_t mq, uintptr_t *addr, uint32_t timeout)
{
    struct sim_mq *m = mq;
    (void)timeout;
    if (m->h == m->t) {
        return -USB_ERR_TIMEOUT;
    }
    *addr = m->q[m->h++ % 64];
    return 0;
}

usb_osal_thread_t usb_osal_thread_create(const char *name, uint32_t stack_size, uint32_t prio,
                                         usb_thread_entry_t entry, void *args)
{
    (void)name; (void)stack_size; (void)prio; (void)entry; (void)args;
    return (usb_osal_thread_t)1;
}
void usb_osal_thread_delete(usb_osal_thread_t thread) { (void)thread; }
void usb_osal_thread_schedule_other(void) { sim_advance_us(10); }

struct usb_osal_timer *usb_osal_timer_create(const char *name, uint32_t timeout_ms, usb_timer_handler_t handler,
                                             void *argument, bool is_period)
{
    (void)name; (void)timeout_ms; (void)handler; (void)argument; (void)is_period;
    return NULL;
}
void usb_osal_timer_delete(struct usb_osal_timer *timer) { (void)timer; }
void usb_osal_timer_start(struct usb_osal_timer *timer) { (void)timer; }
void usb_osal_timer_stop(struct usb_osal_timer *timer) { (void)timer; }

/* ---- memory ---- */
void *usb_osal_malloc(size_t size) { return sim_dma_alloc(size, 32); }
void usb_osal_free(void *ptr) { sim_dma_free(ptr); }

/* ---- cache maintenance ---- */
void usb_dcache_clean(uintptr_t addr, size_t size) { sim_cache_clean(addr, size); }
void usb_dcache_invalidate(uintptr_t addr, size_t size) { sim_cache_invalidate(addr, size); }
void usb_dcache_flush(uintptr_t addr, size_t size) { sim_cache_clean(addr, size); }

struct usbh_bus g_usbhost_bus[CONFIG_USBHOST_MAX_BUS];

void sim_irq_handler(void)
{
    USBH_IRQHandler(0);
}
