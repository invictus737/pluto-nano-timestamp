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
 * The interrupt is handled in the RTEMS interrupt server task, inside the OSAL critical
 * section, so CherryUSB's schedule scans exclude the threads that submit and cancel. A poll
 * thread (NEXUS_USB_POLL_MS) completes what a lost interrupt left behind, re-rings or ends a
 * missing Interrupt on Async Advance (patch 0001) and kicks the hub thread when PORTSC shows a
 * connect change whose interrupt never came (seen with libbsd after unplugging).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
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

static rtems_binary_semaphore g_hc_ready = RTEMS_BINARY_SEMAPHORE_INITIALIZER("usbhc");
static volatile bool g_irq_installed;
static bool g_started;

/* counters (`usbh` command) */
static volatile uint32_t g_irqs, g_irq_iaa, g_poll_handled, g_iaa_rings, g_iaa_forced,
    g_port_kicks, g_async_stalls;

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

/* ---- interrupt (interrupt server task) ---- */

static void zynq_usb_isr(void *arg)
{
    struct usbh_bus *bus = arg;
    size_t flags = usb_osal_enter_critical_section();
    uint32_t sts = REG32(ZY7_USB_USBSTS) & REG32(ZY7_USB_USBINTR);

    g_irqs++;
    if (sts & USBSTS_IAA) {
        g_irq_iaa++;
    }
    USBH_IRQHandler(bus->hcd.hcd_id);
    usb_osal_leave_critical_section(flags);
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
        sc = rtems_interrupt_server_handler_install(RTEMS_INTERRUPT_SERVER_DEFAULT, ZYNQ_IRQ_USB_0,
                                                    "USB0", RTEMS_INTERRUPT_UNIQUE, zynq_usb_isr, bus);
        if (sc != RTEMS_SUCCESSFUL) {
            printf("usbh: USB0 interrupt handler: %s\n", rtems_status_text(sc));
        } else {
            g_irq_installed = true;
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
        (void)rtems_interrupt_server_handler_remove(RTEMS_INTERRUPT_SERVER_DEFAULT, ZYNQ_IRQ_USB_0,
                                                    zynq_usb_isr, bus);
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

/* ---- watchdog poll ---- */

static void zynq_usb_poll_thread(CONFIG_USB_OSAL_THREAD_SET_ARGV)
{
    struct usbh_bus *bus = (struct usbh_bus *)CONFIG_USB_OSAL_THREAD_GET_ARGV;
    uint32_t last_pending = 0, last_iaa = 0, last_alist = 0, stuck_alist = 0;
    unsigned ticks = 0, iaad_stuck = 0;

    for (;;) {
        size_t flags;
        uint32_t sts, pending, both;

        usb_osal_msleep(NEXUS_USB_POLL_MS);
        ticks++;
        flags = usb_osal_enter_critical_section();

        /* an enabled status bit nobody handled: the interrupt was lost */
        sts = REG32(ZY7_USB_USBSTS) & REG32(ZY7_USB_USBINTR);
        if (sts != 0) {
            g_poll_handled++;
            if (sts & USBSTS_IAA) {
                g_irq_iaa++;
            }
            USBH_IRQHandler(bus->hcd.hcd_id);
        }

        /* QHs still waiting for an IAA one poll period later, with no IAA seen meanwhile */
        pending = usbh_ehci_iaa_pending(bus->hcd.hcd_id);
        both = (g_irq_iaa == last_iaa) ? (pending & last_pending) : 0;
        if (both != 0) {
            if ((REG32(ZY7_USB_USBCMD) & USBCMD_IAAD) == 0) {
                /* the IAA was consumed by usbh_kill_urb(): ring the doorbell again */
                REG32(ZY7_USB_USBCMD) |= USBCMD_IAAD;
                g_iaa_rings++;
                iaad_stuck = 0;
            } else if (++iaad_stuck >= 2) {
                /* doorbell ignored for two periods: end the unlink as Linux does */
                (void)usbh_ehci_iaa_complete(bus->hcd.hcd_id, both);
                g_iaa_forced++;
                iaad_stuck = 0;
                pending = usbh_ehci_iaa_pending(bus->hcd.hcd_id);
            }
        } else {
            iaad_stuck = 0;
        }
        last_pending = pending;
        last_iaa = g_irq_iaa;

        /* async engine parked on one QH (diagnostic only: ASYNCLISTADDR frozen while busy) */
        {
            uint32_t alist = REG32(ZY7_USB_ASYNCLISTADDR);

            if ((REG32(ZY7_USB_USBCMD) & (1U << 5)) && pending == 0 && alist == last_alist &&
                alist != 0 && (REG32(ZY7_USB_USBSTS) & (1U << 15))) {
                if (++stuck_alist == 100) {
                    g_async_stalls++;
                }
            } else {
                stuck_alist = 0;
            }
            last_alist = alist;
        }
        usb_osal_leave_critical_section(flags);

        /* port change without its interrupt: let the hub thread look (twice a second at most) */
        if ((ticks % (500 / NEXUS_USB_POLL_MS)) == 0 && (REG32(ZY7_USB_PORTSC1) & PORTSC_CSC)) {
            flags = usb_osal_enter_critical_section();
            bus->hcd.roothub.int_buffer[0] |= (1U << 1);
            usb_osal_leave_critical_section(flags);
            usbh_hub_thread_wakeup(&bus->hcd.roothub);
            g_port_kicks++;
        }
    }
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
    if (usb_osal_thread_create("usbh_poll", 8192, 2, zynq_usb_poll_thread, bus) == NULL) {
        printf("usbh: poll thread failed to start\n");
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
    printf("irqs %lu (IAA %lu), poll-handled %lu, IAA re-rung %lu forced %lu, port kicks %lu, "
           "async stalls %lu, XACTERR restarts %lu, QHs waiting for IAA 0x%08lx\n",
           (unsigned long)g_irqs, (unsigned long)g_irq_iaa, (unsigned long)g_poll_handled,
           (unsigned long)g_iaa_rings, (unsigned long)g_iaa_forced, (unsigned long)g_port_kicks,
           (unsigned long)g_async_stalls, (unsigned long)g_ehci_xacterr_retries,
           (unsigned long)usbh_ehci_iaa_pending(0));
    if (aicwf_cherry_stats) {
        aicwf_cherry_stats();
    }
    return 0;
}
