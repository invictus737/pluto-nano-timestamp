/*
 * CherryUSB EHCI glue for the Zynq-7000 USB0 controller on the Pluto Nano (RTEMS 6).
 *
 * USB0 at 0xE0002000 is a ChipIdea (Synopsys/ARC TDI) dual-role core with an integrated TT and
 * a ULPI PHY (Microchip USB3320, reset on MIO 46). The bring-up sequence is the one of the
 * libbsd glue (rtos/libbsd/ehci_zynq.c, FreeBSD zy7_ehci.c) with its Linux-parity settings:
 *   before CherryUSB's HCRESET (usb_hc_low_level_init): SLCR USB0 reset (U-Boot left the core
 *     as a DFU gadget), PHY reset through GPIO 46, ULPI ID read, interrupt handler;
 *   after HCRESET (usb_hc_low_level2_init): USBMODE host as Linux tdi_reset() (SDIS only with
 *     nexus_cherryusb_sdis, config usb_sdis=1; TXFILLTUNING left at its reset value, config
 *     usb_txfill sets it live), PORTSC PTS = ULPI only when it is not (write-1-to-clear bits
 *     never written back), asynchronous park mode off (Linux ehci-hcd park=0);
 *   once the controller runs (nexus_cherryusb_start): ULPI OTG control "set" register 0x0B =
 *     0x66 (DP/DM pull-downs, DrvVbus, DrvVbusExternal), so the PHY powers the port.
 * Before the stack starts, stale cache lines over the non-cacheable areas are discarded.
 *
 * Interrupt: the hard handler (GIC SPI 53, routed to NEXUS_USB_THREAD_CPU) only masks the
 * vector, timestamps and wakes the "usbh_irq" thread, which runs CherryUSB's handler inside
 * the OSAL critical section (so the schedule scans exclude the threads that submit and cancel)
 * and unmasks. The thread has its own SCHED_FIFO priority (nexus_cherryusb_irq_prio, default
 * NEXUS_USB_IRQ_PRIO_FROM_MAX below the maximum): the shared libbsd interrupt server runs at
 * classic priority 96 on CPU0 and was starved there by the station's real-time threads, which
 * delays every completion (each bulk transfer needs its USBINT and its IAA handled).
 * The same thread wakes at least every NEXUS_USB_POLL_MS: it handles status bits whose
 * interrupt was lost, runs the IAA watchdog (patches/0004, Linux ehci_iaa_watchdog) and kicks
 * the hub thread when PORTSC shows a connect change whose interrupt never came (seen with
 * libbsd after unplugging). Watchdog events are counted; one line per 10 s at most.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE /* pthread_attr_setaffinity_np, pthread_setname_np */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rtems.h>
#include <rtems/counter.h>
#include <rtems/irq-extension.h>
#include <rtems/thread.h>

#include <bsp.h>
#include <bsp/irq.h>
#include <bsp/linker-symbols.h>

#include "usbh_core.h"
#include "usbh_hub.h"
#include "usb_hc_ehci.h"
#include "nexus_cherryusb.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define ZY7_USB_CAPLENGTH      (NEXUS_USB0_BASE + 0x100)
#define ZY7_USB_USBCMD         (NEXUS_USB0_BASE + 0x140)
#define ZY7_USB_USBSTS         (NEXUS_USB0_BASE + 0x144)
#define ZY7_USB_USBINTR        (NEXUS_USB0_BASE + 0x148)
#define ZY7_USB_ASYNCLISTADDR  (NEXUS_USB0_BASE + 0x158)
#define ZY7_USB_BURSTSIZE      (NEXUS_USB0_BASE + 0x160)
#define ZY7_USB_TXFILLTUNING   (NEXUS_USB0_BASE + 0x164)
#define ZY7_USB_ULPI_VIEWPORT  (NEXUS_USB0_BASE + 0x170)
#define   ULPI_VP_WU           (1U << 31)
#define   ULPI_VP_RUN          (1U << 30)
#define   ULPI_VP_RW           (1U << 29)
#define   ULPI_VP_ADDR(a)      ((uint32_t)(a) << 16)
#define ZY7_USB_PORTSC1        (NEXUS_USB0_BASE + 0x184)
#define   PORTSC_CCS           (1U << 0)
#define   PORTSC_CSC           (1U << 1)
#define   PORTSC_PTS_MASK      (3U << 30)
#define   PORTSC_PTS_ULPI      (2U << 30)
#define   PORTSC_PTW           (1U << 28)
#define   PORTSC_PSPD_SHIFT    26
#define   PORTSC_PTS2          (1U << 25)
#define ZY7_USB_USBMODE        (NEXUS_USB0_BASE + 0x1a8)
#define   USBMODE_CM_MASK      3U
#define   USBMODE_CM_HOST      3U
#define   USBMODE_SDIS         (1U << 4)

#define USBCMD_ASP_MASK        (3U << 8)
#define USBCMD_ASPE            (1U << 11)
#define USBCMD_IAAD            (1U << 6)
#define USBSTS_IAA             (1U << 5)
#define USBSTS_HCH             (1U << 12)

/* ULPI OTG Control (set alias 0x0b): DP/DM pull-downs (host), DrvVbus, DrvVbusExternal */
#define ULPI_OTG_CTRL_SET      0x0b
#define ULPI_OTG_HOST_VBUS     0x66

/* PS GPIO bank 1 (MIO 32..53); MIO 46 is bit 14 */
#define ZY7_GPIO_MASK_DATA_1_LSW 0xe000a008U
#define ZY7_GPIO_DIRM_1          0xe000a244U
#define ZY7_GPIO_OEN_1           0xe000a248U
#define PHY_RESET_BIT            14

#define SLCR_UNLOCK            0xf8000008U
#define SLCR_UNLOCK_KEY        0xdf0dU
#define SLCR_USB_RST_CTRL      0xf8000210U

int nexus_cherryusb_sdis;
int nexus_cherryusb_irq_prio;

static rtems_binary_semaphore g_hc_ready = RTEMS_BINARY_SEMAPHORE_INITIALIZER("usbhc");
static volatile bool g_irq_installed;
static bool g_started;

static rtems_binary_semaphore g_irq_sem = RTEMS_BINARY_SEMAPHORE_INITIALIZER("usbirq");
static volatile rtems_counter_ticks g_irq_stamp;
static volatile bool g_irq_stamped;
static int g_irq_thread_prio;

/* counters (`usbh` command) */
static volatile uint32_t g_irqs, g_irq_iaa, g_poll_handled, g_port_kicks, g_async_stalls;
/* hard interrupt to handler start: maximum and count over 1 ms */
static volatile uint32_t g_irq_lat_max_us, g_irq_lat_over_1ms;

static void udelay(uint32_t us)
{
    rtems_counter_delay_nanoseconds((uint64_t)us * 1000U);
}

/* ---- controller / PHY bring-up ---- */

static void zynq_usb0_controller_reset(void)
{
    REG32(SLCR_UNLOCK) = SLCR_UNLOCK_KEY;
    REG32(SLCR_USB_RST_CTRL) |= 1U;
    udelay(10);
    REG32(SLCR_USB_RST_CTRL) &= ~1U;
    udelay(100);
}

static void zynq_ulpi_phy_reset(void)
{
    uint32_t bit = 1U << PHY_RESET_BIT;

    REG32(ZY7_GPIO_DIRM_1) |= bit;
    REG32(ZY7_GPIO_OEN_1) |= bit;
    REG32(ZY7_GPIO_MASK_DATA_1_LSW) = ((~bit & 0xffffU) << 16) | 0;
    usb_osal_msleep(1);
    REG32(ZY7_GPIO_MASK_DATA_1_LSW) = ((~bit & 0xffffU) << 16) | bit;
    usb_osal_msleep(5);
}

static int zynq_ulpi_wait(uint32_t bit)
{
    int tries = 1000;

    while ((REG32(ZY7_USB_ULPI_VIEWPORT) & bit) != 0) {
        if (--tries < 0) {
            return -1;
        }
        udelay(1);
    }
    return 0;
}

static int zynq_ulpi_read(uint8_t addr)
{
    REG32(ZY7_USB_ULPI_VIEWPORT) = ULPI_VP_WU;
    (void)zynq_ulpi_wait(ULPI_VP_WU);
    REG32(ZY7_USB_ULPI_VIEWPORT) = ULPI_VP_RUN | ULPI_VP_ADDR(addr);
    if (zynq_ulpi_wait(ULPI_VP_RUN) != 0) {
        return -1;
    }
    return (REG32(ZY7_USB_ULPI_VIEWPORT) >> 8) & 0xff;
}

static int zynq_ulpi_write(uint8_t addr, uint8_t val)
{
    REG32(ZY7_USB_ULPI_VIEWPORT) = ULPI_VP_RUN | ULPI_VP_RW | ULPI_VP_ADDR(addr) | val;
    return zynq_ulpi_wait(ULPI_VP_RUN);
}

/* ---- interrupt ---- */

uint32_t usb_ehci_time_us(void)
{
    /* uptime, not the CPU counter: rtems_counter_ticks is 32 bits and wraps within seconds */
    return (uint32_t)(rtems_clock_get_uptime_nanoseconds() / 1000U);
}

/* hard interrupt: mask the vector and wake the handler thread (it unmasks) */
static void zynq_usb_hard_isr(void *arg)
{
    (void)arg;
    (void)rtems_interrupt_vector_disable(ZYNQ_IRQ_USB_0);
    if (!g_irq_stamped) {
        g_irq_stamp = rtems_counter_read();
        g_irq_stamped = true;
    }
    rtems_binary_semaphore_post(&g_irq_sem);
}

void usb_hc_low_level_init(struct usbh_bus *bus)
{
    rtems_status_code sc;
    int v0, v1, p0, p1;

    zynq_usb0_controller_reset();
    zynq_ulpi_phy_reset();
    /* SMSC/Microchip USB33x0 reads 0424/0007; -1 = PHY not answering */
    v0 = zynq_ulpi_read(0);
    v1 = zynq_ulpi_read(1);
    p0 = zynq_ulpi_read(2);
    p1 = zynq_ulpi_read(3);
    printf("usbh: ULPI PHY vendor %02x%02x product %02x%02x, id 0x%08lx caplength 0x%lx\n",
           v1 & 0xff, v0 & 0xff, p1 & 0xff, p0 & 0xff, (unsigned long)REG32(NEXUS_USB0_BASE),
           (unsigned long)(REG32(ZY7_USB_CAPLENGTH) & 0xff));
    if (v0 < 0) {
        printf("usbh: ULPI PHY does not answer (reset polarity?)\n");
    }
    if (!g_irq_installed) {
        cpu_set_t cpus;

        (void)bus;
        sc = rtems_interrupt_handler_install(ZYNQ_IRQ_USB_0, "USB0", RTEMS_INTERRUPT_UNIQUE,
                                             zynq_usb_hard_isr, NULL);
        if (sc != RTEMS_SUCCESSFUL) {
            printf("usbh: USB0 interrupt handler: %s\n", rtems_status_text(sc));
        } else {
            g_irq_installed = true;
            /* the GIC default target is CPU0; say it, next to the handler thread */
            CPU_ZERO(&cpus);
            CPU_SET(NEXUS_USB_THREAD_CPU, &cpus);
            (void)rtems_interrupt_set_affinity(ZYNQ_IRQ_USB_0, sizeof(cpus), &cpus);
        }
    }
}

/* after CherryUSB's HCRESET, before it starts the schedules */
void usb_hc_low_level2_init(struct usbh_bus *bus)
{
    uint32_t v;
    int tries;

    (void)bus;
    v = REG32(ZY7_USB_USBMODE);
    v = (v & ~(USBMODE_CM_MASK | USBMODE_SDIS)) | USBMODE_CM_HOST;
    /* SDIS: ChipIdea erratum TAR 9000378958 (OUT retries with a buffer that is not double-word
     * aligned); Zynq Linux does not set it and every buffer here is 32-byte aligned */
    if (nexus_cherryusb_sdis) {
        v |= USBMODE_SDIS;
    }
    REG32(ZY7_USB_USBMODE) = v;
    for (tries = 0; tries < 1000 && (REG32(ZY7_USB_USBMODE) & USBMODE_CM_MASK) != USBMODE_CM_HOST; tries++) {
        udelay(1);
    }

    /* PTS = ULPI (Linux hw_phymode_configure); CSC/PEC/OCC are write-1-to-clear */
    v = REG32(ZY7_USB_PORTSC1);
    if ((v & (PORTSC_PTS_MASK | PORTSC_PTW | PORTSC_PTS2)) != PORTSC_PTS_ULPI) {
        v &= ~(PORTSC_PTS_MASK | PORTSC_PTW | PORTSC_PTS2 | 0x2aU);
        v |= PORTSC_PTS_ULPI;
        REG32(ZY7_USB_PORTSC1) = v;
    }

    /* asynchronous schedule park mode off (Linux ehci-hcd park=0); CherryUSB keeps the
     * remaining USBCMD bits it reads back here */
    REG32(ZY7_USB_USBCMD) &= ~(USBCMD_ASPE | USBCMD_ASP_MASK);

    usb_osal_dma_barrier();
    rtems_binary_semaphore_post(&g_hc_ready);
}

void usb_hc_low_level_deinit(struct usbh_bus *bus)
{
    if (g_irq_installed) {
        (void)rtems_interrupt_handler_remove(ZYNQ_IRQ_USB_0, zynq_usb_hard_isr, NULL);
        g_irq_installed = false;
    }
}

/* ChipIdea PORTSC.PSPD (bits 27:26): 0 full, 1 low, 2 high speed */
uint8_t usbh_get_port_speed(struct usbh_bus *bus, const uint8_t port)
{
    uint32_t pspd;

    (void)bus;
    (void)port;
    pspd = (REG32(ZY7_USB_PORTSC1) >> PORTSC_PSPD_SHIFT) & 3U;
    if (pspd == 2) {
        return USB_SPEED_HIGH;
    }
    if (pspd == 1) {
        return USB_SPEED_LOW;
    }
    return USB_SPEED_FULL;
}

#ifdef CONFIG_USB_DCACHE_ENABLE
void usb_dcache_clean(uintptr_t addr, size_t size)
{
    /* RTEMS has no clean-only range operation: clean + invalidate */
    if (size) {
        rtems_cache_flush_multiple_data_lines((const void *)addr, size);
    }
}

void usb_dcache_invalidate(uintptr_t addr, size_t size)
{
    if (size) {
        rtems_cache_invalidate_multiple_data_lines((const void *)addr, size);
    }
}

void usb_dcache_flush(uintptr_t addr, size_t size)
{
    /* on this BSP a range "flush" cleans and invalidates L1 and the L2C-310, then syncs */
    if (size) {
        rtems_cache_flush_multiple_data_lines((const void *)addr, size);
    }
}
#endif

/* ---- interrupt handler thread (also the lost-interrupt and IAA watchdog poll) ---- */

static void *zynq_usb_irq_thread(void *arg)
{
    struct usbh_bus *bus = arg;
    uint32_t last_alist = 0, stuck_alist = 0, last_port_kick = 0, last_log = 0;
    uint32_t logged_lost = 0, logged_late = 0, logged_kicks = 0;
    rtems_interval period = RTEMS_MILLISECONDS_TO_TICKS(NEXUS_USB_POLL_MS);

    if (period == 0) {
        period = 1;
    }
    for (;;) {
        size_t flags;
        uint32_t sts, now;
        bool woken;
        const struct usbh_ehci_iaa_stats *st = &g_ehci_iaa_stats[bus->hcd.hcd_id];

        /* while a cycle runs, wake at least every 2 ms so the 10 ms watchdog is not late */
        woken = rtems_binary_semaphore_wait_timed_ticks(
                    &g_irq_sem, usbh_ehci_iaa_pending(bus->hcd.hcd_id) ?
                                    RTEMS_MILLISECONDS_TO_TICKS(2) + 1 : period) == 0;
        now = usb_ehci_time_us();
        if (woken && g_irq_stamped) {
            uint32_t us = (uint32_t)(rtems_counter_ticks_to_nanoseconds(
                                         rtems_counter_read() - g_irq_stamp) / 1000U);

            g_irq_stamped = false;
            if (us > g_irq_lat_max_us) {
                g_irq_lat_max_us = us;
            }
            if (us > 1000) {
                g_irq_lat_over_1ms++;
            }
        }

        flags = usb_osal_enter_critical_section();
        sts = REG32(ZY7_USB_USBSTS) & REG32(ZY7_USB_USBINTR);
        if (sts != 0) {
            if (woken) {
                g_irqs++;
            } else {
                /* an enabled status bit nobody handled: the interrupt was lost */
                g_poll_handled++;
            }
            if (sts & USBSTS_IAA) {
                g_irq_iaa++;
            }
            USBH_IRQHandler(bus->hcd.hcd_id);
        }
        (void)usbh_ehci_iaa_watchdog(bus->hcd.hcd_id);
        /* a parked asynchronous engine: schedule off/on with the ASS handshake (patches/0006) */
        (void)usbh_ehci_async_watchdog(bus->hcd.hcd_id);

        /* async engine parked on one QH (diagnostic only: ASYNCLISTADDR frozen while busy) */
        {
            uint32_t alist = REG32(ZY7_USB_ASYNCLISTADDR);

            if (!woken && (REG32(ZY7_USB_USBCMD) & (1U << 5)) && alist == last_alist && alist != 0 &&
                (REG32(ZY7_USB_USBSTS) & (1U << 15))) {
                if (++stuck_alist == 100) {
                    g_async_stalls++;
                }
            } else if (!woken) {
                stuck_alist = 0;
            }
            last_alist = alist;
        }
        usb_osal_leave_critical_section(flags);
        if (woken) {
            (void)rtems_interrupt_vector_enable(ZYNQ_IRQ_USB_0);
        }

        /* port change without its interrupt: let the hub thread look (twice a second at most) */
        if ((uint32_t)(now - last_port_kick) >= 500000U && (REG32(ZY7_USB_PORTSC1) & PORTSC_CSC)) {
            last_port_kick = now;
            flags = usb_osal_enter_critical_section();
            bus->hcd.roothub.int_buffer[0] |= (1U << 1);
            usb_osal_leave_critical_section(flags);
            usbh_hub_thread_wakeup(&bus->hcd.roothub);
            g_port_kicks++;
        }

        /* IAA watchdog events: one line per 10 s at most (never in the per-transfer path) */
        if ((st->lost != logged_lost || st->late != logged_late || st->async_kicks != logged_kicks) &&
            (uint32_t)(now - last_log) >= 10000000U) {
            if (st->async_kicks != logged_kicks) {
                printf("usbh: async engine restarted %lu time(s) (never-fetched qTD %lu, doorbell "
                       "unanswered %lu, no ASS handshake %lu; before the last: USBCMD 0x%08lx "
                       "USBSTS 0x%08lx ASYNCLISTADDR 0x%08lx)\n",
                       (unsigned long)st->async_kicks, (unsigned long)st->async_kick_unfetched,
                       (unsigned long)st->async_kick_iaad, (unsigned long)st->async_kick_no_handshake,
                       (unsigned long)st->kick_cmd, (unsigned long)st->kick_sts,
                       (unsigned long)st->kick_async);
                logged_kicks = st->async_kicks;
            }
            printf("usbh: IAA watchdog: %lu lost, %lu late of %lu cycles, %lu QHs quarantined "
                   "(%lu released, doorbell stuck %lu) (USBCMD 0x%08lx USBSTS 0x%08lx "
                   "ASYNCLISTADDR 0x%08lx, longest cycle %lu us)\n",
                   (unsigned long)st->lost, (unsigned long)st->late, (unsigned long)st->cycles,
                   (unsigned long)st->quarantined, (unsigned long)st->released,
                   (unsigned long)st->stuck_ends,
                   (unsigned long)st->last_cmd, (unsigned long)st->last_sts,
                   (unsigned long)st->last_async, (unsigned long)st->max_cycle_us);
            logged_lost = st->lost;
            logged_late = st->late;
            last_log = now;
        }
    }
    return NULL;
}

static int zynq_usb_irq_thread_start(struct usbh_bus *bus)
{
    struct sched_param param;
    pthread_attr_t attr;
    pthread_t tid;
    cpu_set_t cpus;
    int rc, prio = nexus_cherryusb_irq_prio;

    if (prio <= 0) {
        prio = sched_get_priority_max(SCHED_FIFO) - NEXUS_USB_IRQ_PRIO_FROM_MAX;
    }
    if (prio > sched_get_priority_max(SCHED_FIFO)) {
        prio = sched_get_priority_max(SCHED_FIFO);
    }
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    param.sched_priority = prio;
    pthread_attr_setschedparam(&attr, &param);
    CPU_ZERO(&cpus);
    CPU_SET(NEXUS_USB_THREAD_CPU, &cpus);
    pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
    rc = pthread_create(&tid, &attr, zynq_usb_irq_thread, bus);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        return -rc;
    }
    (void)pthread_setname_np(tid, "usbh_irq");
    g_irq_thread_prio = prio;
    return 0;
}

/* ---- start ---- */

int nexus_cherryusb_start(void)
{
    struct usbh_bus *bus = &g_usbhost_bus[0];
    int tries;

    if (g_started) {
        return 0;
    }
    g_started = true;
    /* no stale L1/L2 lines may sit over the non-cacheable areas (CherryUSB descriptors and
     * buffers in .bsp_nocache, bounce buffers from the coherent heap): discard them; nothing
     * valid can be cached there, so nothing is written back */
    rtems_cache_invalidate_multiple_data_lines((const void *)bsp_section_nocache_begin,
                                               (size_t)bsp_section_nocache_size);
    /* CherryUSB's static descriptors/buffers live in the NOLOAD part before the coherent heap:
     * not loaded, not zeroed by the start code */
    memset((void *)bsp_section_nocachenoload_begin, 0,
           (size_t)(bsp_section_nocacheheap_begin - bsp_section_nocachenoload_begin));
    rtems_cache_invalidate_multiple_data_lines((const void *)bsp_section_nocacheheap_begin,
                                               (size_t)bsp_section_nocacheheap_size);
    if (usbh_initialize(0, NEXUS_USB0_BASE, NULL) != 0) {
        printf("usbh: CherryUSB host initialisation failed\n");
        return -1;
    }
    /* the hub thread runs usb_hc_init(): wait for the controller to be configured and running */
    if (rtems_binary_semaphore_wait_timed_ticks(&g_hc_ready, rtems_clock_get_ticks_per_second()) != 0) {
        printf("usbh: controller did not come up\n");
        return -1;
    }
    for (tries = 0; tries < 200 && ((REG32(ZY7_USB_USBSTS) & USBSTS_HCH) != 0 ||
                                   REG32(ZY7_USB_USBINTR) == 0); tries++) {
        usb_osal_msleep(1);
    }
    if ((REG32(ZY7_USB_USBSTS) & USBSTS_HCH) != 0) {
        printf("usbh: controller halted after init (USBCMD 0x%08lx USBSTS 0x%08lx)\n",
               (unsigned long)REG32(ZY7_USB_USBCMD), (unsigned long)REG32(ZY7_USB_USBSTS));
        return -1;
    }
    /* host mode runs: let the PHY drive VBUS and enable the host pull-downs */
    if (zynq_ulpi_write(ULPI_OTG_CTRL_SET, ULPI_OTG_HOST_VBUS) != 0) {
        printf("usbh: ULPI OTG control write timed out\n");
    }
    if (zynq_usb_irq_thread_start(bus) != 0) {
        printf("usbh: interrupt thread failed to start: no USB\n");
        return -1;
    }
    printf("usbh: CherryUSB %s EHCI host up (USBMODE 0x%lx, PORTSC1 0x%08lx)\n", CHERRYUSB_VERSION_STR,
           (unsigned long)REG32(ZY7_USB_USBMODE), (unsigned long)REG32(ZY7_USB_PORTSC1));
    return 0;
}

void aicwf_cherry_stats(void) __attribute__((weak));

int nexus_cherryusb_shell(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("USBCMD 0x%08lx USBSTS 0x%08lx USBINTR 0x%08lx ASYNCLISTADDR 0x%08lx\n",
           (unsigned long)REG32(ZY7_USB_USBCMD), (unsigned long)REG32(ZY7_USB_USBSTS),
           (unsigned long)REG32(ZY7_USB_USBINTR), (unsigned long)REG32(ZY7_USB_ASYNCLISTADDR));
    printf("PORTSC1 0x%08lx USBMODE 0x%08lx TXFILLTUNING 0x%08lx BURSTSIZE 0x%08lx\n",
           (unsigned long)REG32(ZY7_USB_PORTSC1), (unsigned long)REG32(ZY7_USB_USBMODE),
           (unsigned long)REG32(ZY7_USB_TXFILLTUNING), (unsigned long)REG32(ZY7_USB_BURSTSIZE));
    {
        const struct usbh_ehci_iaa_stats *st = &g_ehci_iaa_stats[0];

        printf("irqs %lu (IAA %lu), poll-handled %lu, port kicks %lu, async stalls %lu, "
               "XACTERR restarts %lu; irq thread prio %d, latency max %lu us, >1 ms %lu\n",
               (unsigned long)g_irqs, (unsigned long)g_irq_iaa, (unsigned long)g_poll_handled,
               (unsigned long)g_port_kicks, (unsigned long)g_async_stalls,
               (unsigned long)g_ehci_xacterr_retries, g_irq_thread_prio,
               (unsigned long)g_irq_lat_max_us, (unsigned long)g_irq_lat_over_1ms);
        printf("IAA cycles %lu: irq %lu polled %lu watchdog %lu (lost %lu late %lu), spurious %lu "
               "stale %lu doorbell-busy %lu not-running %lu, kill waits %lu, HSE %lu, longest "
               "%lu us; QHs waiting 0x%08lx\n",
               (unsigned long)st->cycles, (unsigned long)st->irq_ends,
               (unsigned long)st->polled_ends, (unsigned long)st->watchdog_ends,
               (unsigned long)st->lost, (unsigned long)st->late, (unsigned long)st->spurious,
               (unsigned long)st->stale_iaa, (unsigned long)st->iaad_busy,
               (unsigned long)st->not_running, (unsigned long)st->kill_waits,
               (unsigned long)st->fatal, (unsigned long)st->max_cycle_us,
               (unsigned long)usbh_ehci_iaa_pending(0));
        printf("QH quarantine %lu (released %lu, doorbell stuck %lu); bulk IN streams created %lu "
               "appended %lu torn down %lu; completions ok %lu\n",
               (unsigned long)st->quarantined, (unsigned long)st->released,
               (unsigned long)st->stuck_ends, (unsigned long)st->stream_creates,
               (unsigned long)st->stream_appends, (unsigned long)st->stream_teardowns,
               (unsigned long)st->ok_count);
        printf("async engine restarts %lu (never-fetched qTD %lu, doorbell unanswered %lu, no ASS "
               "handshake %lu; kick after %u us, 0 = off), bulk NAK reload %u\n",
               (unsigned long)st->async_kicks, (unsigned long)st->async_kick_unfetched,
               (unsigned long)st->async_kick_iaad, (unsigned long)st->async_kick_no_handshake,
               (unsigned)CONFIG_USB_EHCI_ASYNC_KICK_US, (unsigned)CONFIG_USB_EHCI_BULK_RL_HS);
    }
    if (aicwf_cherry_stats) {
        aicwf_cherry_stats();
    }
    return 0;
}
