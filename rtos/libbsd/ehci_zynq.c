/*
 * Zynq-7000 USB0 (ChipIdea core, ULPI PHY) as an EHCI host for RTEMS libbsd.
 *
 * Nexus device without FDT, for the Pluto Nano: USB0 at 0xe0002000, GIC IRQ 53,
 * ULPI PHY reset on MIO 46 (active low). Register layout and the ULPI VBUS write follow
 * FreeBSD sys/arm/xilinx/zy7_ehci.c; the attach sequence follows the RTEMS QorIQ glue
 * rtemsbsd/sys/dev/usb/controller/ehci_mpc83xx.c (same ChipIdea controller family).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <machine/rtems-bsd-kernel-space.h>
#include <machine/rtems-bsd-support.h>

#include <bsp.h>

#if defined(LIBBSP_ARM_XILINX_ZYNQ_BSP_H)

#include <bsp/irq.h>

#include <sys/cdefs.h>
#include <sys/stdint.h>
#include <sys/stddef.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/linker_set.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sysctl.h>
#include <sys/sx.h>
#include <rtems/bsd/sys/unistd.h>
#include <sys/callout.h>
#include <sys/malloc.h>
#include <sys/priv.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_util.h>

#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_hub.h>
#include <dev/usb/controller/ehci.h>
#include <dev/usb/controller/ehcireg.h>

#define ZY7_USB0_BASE			0xe0002000U
#define ZY7_USB_CAPLENGTH		0x0100
#define ZY7_USB_TXFILLTUNING		0x0164
#define ZY7_USB_ULPI_VIEWPORT		0x0170
#define   ULPI_VP_WU			(1U << 31)
#define   ULPI_VP_RUN			(1U << 30)
#define   ULPI_VP_RW			(1U << 29)
#define   ULPI_VP_ADDR(a)		((uint32_t)(a) << 16)
#define ZY7_USB_PORTSC1			0x0184
#define   PORTSC_PTS_MASK		(3U << 30)
#define   PORTSC_PTS_ULPI		(2U << 30)
#define   PORTSC_PTW			(1U << 28)
#define   PORTSC_PTS2			(1U << 25)

/* ULPI OTG Control (set alias 0x0b): DP/DM pull-downs (host), DrvVbus, DrvVbusExternal. */
#define ULPI_OTG_CTRL_SET		0x0b
#define ULPI_OTG_HOST_VBUS		0x66

/* PS GPIO bank 1 (MIO 32..53); MIO 46 is bit 14. */
#define ZY7_GPIO_MASK_DATA_1_LSW	0xe000a008U
#define ZY7_GPIO_DIRM_1			0xe000a244U
#define ZY7_GPIO_OEN_1			0xe000a248U
#define PHY_RESET_BIT			14

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define SLCR_UNLOCK			0xf8000008U
#define SLCR_UNLOCK_KEY			0xdf0dU
#define SLCR_USB_RST_CTRL		0xf8000210U

/* U-Boot last ran USB0 as the DFU gadget: reset the controller before host init. */
static void
zynq_usb0_controller_reset(void)
{
	REG32(SLCR_UNLOCK) = SLCR_UNLOCK_KEY;
	REG32(SLCR_USB_RST_CTRL) |= 1U;
	DELAY(10);
	REG32(SLCR_USB_RST_CTRL) &= ~1U;
	DELAY(100);
}

static void
zynq_ulpi_phy_reset(void)
{
	uint32_t bit = 1U << PHY_RESET_BIT;

	REG32(ZY7_GPIO_DIRM_1) |= bit;
	REG32(ZY7_GPIO_OEN_1) |= bit;
	REG32(ZY7_GPIO_MASK_DATA_1_LSW) = ((~bit & 0xffffU) << 16) | 0;
	DELAY(1000);
	REG32(ZY7_GPIO_MASK_DATA_1_LSW) = ((~bit & 0xffffU) << 16) | bit;
	DELAY(5000);
}

static int
zynq_ulpi_write(uint8_t addr, uint8_t val)
{
	int tries = 1000;

	REG32(ZY7_USB0_BASE + ZY7_USB_ULPI_VIEWPORT) =
	    ULPI_VP_RUN | ULPI_VP_RW | ULPI_VP_ADDR(addr) | val;
	while ((REG32(ZY7_USB0_BASE + ZY7_USB_ULPI_VIEWPORT) & ULPI_VP_RUN) != 0) {
		if (--tries < 0)
			return (-1);
		DELAY(1);
	}
	return (0);
}

static int
zynq_ulpi_wait(uint32_t bit)
{
	int tries = 1000;

	while ((REG32(ZY7_USB0_BASE + ZY7_USB_ULPI_VIEWPORT) & bit) != 0) {
		if (--tries < 0)
			return (-1);
		DELAY(1);
	}
	return (0);
}

static int
zynq_ulpi_read(uint8_t addr)
{
	REG32(ZY7_USB0_BASE + ZY7_USB_ULPI_VIEWPORT) = ULPI_VP_WU;
	(void)zynq_ulpi_wait(ULPI_VP_WU);
	REG32(ZY7_USB0_BASE + ZY7_USB_ULPI_VIEWPORT) = ULPI_VP_RUN | ULPI_VP_ADDR(addr);
	if (zynq_ulpi_wait(ULPI_VP_RUN) != 0)
		return (-1);
	return ((REG32(ZY7_USB0_BASE + ZY7_USB_ULPI_VIEWPORT) >> 8) & 0xff);
}

static void
zynq_ehci_post_reset(struct ehci_softc *e)
{
	uint32_t usbmode, portsc, fill;

	usbmode = EOREAD4(e, EHCI_USBMODE_NOLPM);
	usbmode &= ~EHCI_UM_CM;
	usbmode |= EHCI_UM_CM_HOST;
	/*
	 * SDIS (stream disable, USBMODE bit 4) in host mode: ChipIdea erratum TAR 9000378958 hangs
	 * the host on an OUT retry when the buffer is not double-word aligned (Linux sets it for
	 * host mode for the same reason). Seen here as bulk/control OUT stuck in PING/XACTERR
	 * retries, IAAD never acknowledged and new QHs never fetched.
	 */
	usbmode |= (1U << 4);
	EOWRITE4(e, EHCI_USBMODE_NOLPM, usbmode);

	portsc = REG32(ZY7_USB0_BASE + ZY7_USB_PORTSC1);
	portsc &= ~(PORTSC_PTS_MASK | PORTSC_PTW | PORTSC_PTS2);
	portsc |= PORTSC_PTS_ULPI;
	REG32(ZY7_USB0_BASE + ZY7_USB_PORTSC1) = portsc;

	/* TX FIFO threshold 8 bursts: avoids TX underruns on the ULPI path (zy7_ehci.c). */
	fill = REG32(ZY7_USB0_BASE + ZY7_USB_TXFILLTUNING);
	fill = (fill & ~(0x3fU << 16)) | (8U << 16);
	REG32(ZY7_USB0_BASE + ZY7_USB_TXFILLTUNING) = fill;
}

static int
zynq_ehci_probe(device_t self)
{
	device_set_desc(self, "Zynq-7000 EHCI");
	return (0);
}

static int zynq_ehci_detach(device_t self);

/*
 * Port-change watchdog. On the board the root-hub status-change interrupt can get lost (seen
 * after unplugging a high-current device: PORTSC1 showed CCS=1 CSC=1 while USBSTS.PCI stayed 0
 * and no attach followed). Every 500 ms look at the change bits of PORTSC1 and kick a bus explore
 * when any is set; explore reads the port itself, so attach/detach no longer depends on the
 * interrupt. Costs one register read per half second.
 */
#define PORTSC_CHANGE_BITS ((1U << 1) | (1U << 3) | (1U << 5)) /* CSC, PEC, OCC */
static struct callout zynq_ehci_poll;
static ehci_softc_t *zynq_ehci_sc;

int ehci_zynq_poll(ehci_softc_t *sc); /* ehci.c (Pluto patch) */
static unsigned zynq_ehci_rescued, zynq_ehci_ticks, zynq_ehci_repairs_seen;
extern unsigned ehci_zynq_ring_repairs; /* ehci.c (Pluto patch) */

/*
 * ChipIdea (Zynq USB) host: a completed qTD with IOC sets USBSTS.UAI (async) or UPI (periodic),
 * and on this controller not always the standard USBINT, so the stock EHCI handler (which only
 * looks at the six standard bits) misses completions and the transfers time out. UAIE/UPIE are
 * enabled after ehci_init; this handler acknowledges those two bits and completes the finished
 * transfers, then lets the stock handler do the rest.
 */
#define ZY7_USBSTS_UAI (1U << 18)
#define ZY7_USBSTS_UPI (1U << 19)
static void
zynq_ehci_intr(void *arg)
{
	ehci_softc_t *e = arg;
	uint32_t sts = REG32(ZY7_USB0_BASE + 0x144) & (ZY7_USBSTS_UAI | ZY7_USBSTS_UPI);

	if (sts != 0)
		REG32(ZY7_USB0_BASE + 0x144) = sts; /* write 1 to clear */
	ehci_interrupt(e);
	if (sts != 0)
		(void)ehci_zynq_poll(e);
}

/*
 * Every 10 ms: completed transfers whose interrupt never arrived are finished here (counted and
 * reported every 10 s), and pending port changes kick an explore.
 */
/*
 * Async engine watchdog. The ChipIdea core was seen parked on one QH (ASYNCLISTADDR constant,
 * IAAD never acknowledged) while that QH's next qTD sat active in memory: the controller kept a
 * stale copy and never fetched it, so every later transfer timed out. Turning the async schedule
 * off and on (EHCI 4.8, with the AS status handshake) makes it re-read the list from memory.
 */
unsigned zynq_ehci_async_kicks;
static uint32_t zynq_ehci_last_alist;
static unsigned zynq_ehci_stuck_ticks;

static void
zynq_ehci_async_kick(void)
{
	volatile uint32_t *cmd = (volatile uint32_t *)(ZY7_USB0_BASE + 0x140);
	volatile uint32_t *sts = (volatile uint32_t *)(ZY7_USB0_BASE + 0x144);
	int i;

	*cmd &= ~(1U << 5);
	for (i = 0; i < 2000 && (*sts & (1U << 15)) != 0; ++i)
		DELAY(1);
	*sts = 1U << 5; /* stale IAA */
	*cmd |= 1U << 5;
	for (i = 0; i < 2000 && (*sts & (1U << 15)) == 0; ++i)
		DELAY(1);
	zynq_ehci_async_kicks++;
}

void zynq_ehci_kick_async(void); /* also called from ehci_timeout (Pluto patch) */
void
zynq_ehci_kick_async(void)
{
	zynq_ehci_async_kick();
}

static void
zynq_ehci_poll_tick(void *arg)
{
	(void)arg;
	if (zynq_ehci_sc != NULL) {
		uint32_t c = REG32(ZY7_USB0_BASE + 0x140);
		uint32_t al = REG32(ZY7_USB0_BASE + 0x158);
		if ((c & (1U << 6)) != 0 && al == zynq_ehci_last_alist) {
			if (++zynq_ehci_stuck_ticks >= 5) {
				zynq_ehci_async_kick();
				zynq_ehci_stuck_ticks = 0;
			}
		} else {
			zynq_ehci_stuck_ticks = 0;
		}
		zynq_ehci_last_alist = al;
		zynq_ehci_rescued += (unsigned)ehci_zynq_poll(zynq_ehci_sc);
		if ((REG32(ZY7_USB0_BASE + ZY7_USB_PORTSC1) & PORTSC_CHANGE_BITS) != 0 &&
		    (zynq_ehci_ticks % 50) == 0)
			usb_needs_explore(&zynq_ehci_sc->sc_bus, 0);
		if (++zynq_ehci_ticks % 1000 == 0 &&
		    (zynq_ehci_rescued != 0 || ehci_zynq_ring_repairs != zynq_ehci_repairs_seen ||
		    zynq_ehci_async_kicks != 0)) {
			printf("ehci: %u transfer(s) completed by the poll, async ring repairs %u, async kicks %u\n",
			    zynq_ehci_rescued, ehci_zynq_ring_repairs, zynq_ehci_async_kicks);
			zynq_ehci_rescued = 0;
			zynq_ehci_repairs_seen = ehci_zynq_ring_repairs;
		}
	}
	callout_reset(&zynq_ehci_poll, hz / 100 > 0 ? hz / 100 : 1, zynq_ehci_poll_tick, NULL);
}

static int
zynq_ehci_attach(device_t self)
{
	rtems_status_code sc;
	ehci_softc_t *e = device_get_softc(self);
	usb_error_t ue;
	uint32_t caplength;

	memset(e, 0, sizeof(*e));

	e->sc_bus.parent = self;
	e->sc_bus.devices = e->sc_devices;
	e->sc_bus.devices_max = EHCI_MAX_DEVICES;
	e->sc_bus.usbrev = USB_REV_2_0;
	e->sc_bus.dma_bits = 32;

	if (usb_bus_mem_alloc_all(&e->sc_bus, USB_GET_DMA_TAG(self),
	    &ehci_iterate_hw_softc)) {
		return (ENOMEM);
	}

	e->sc_bus.bdev = device_add_child(self, "usbus", -1);
	if (e->sc_bus.bdev == NULL) {
		device_printf(self, "could not add USB device\n");
		goto error;
	}
	device_set_ivars(e->sc_bus.bdev, &e->sc_bus);
	device_set_desc(e->sc_bus.bdev, "EHCI");
	snprintf(e->sc_vendor, sizeof(e->sc_vendor), "Xilinx");

	zynq_usb0_controller_reset();
	zynq_ulpi_phy_reset();
	{
		/* ULPI vendor/product ID: SMSC/Microchip USB33x0 reads 0424/0007. -1 = PHY not answering. */
		int v0 = zynq_ulpi_read(0), v1 = zynq_ulpi_read(1);
		int p0 = zynq_ulpi_read(2), p1 = zynq_ulpi_read(3);
		device_printf(self, "ULPI PHY vendor %02x%02x product %02x%02x\n",
		    v1 & 0xff, v0 & 0xff, p1 & 0xff, p0 & 0xff);
		if (v0 < 0)
			device_printf(self, "ULPI PHY does not answer (reset polarity?)\n");
	}

	/* Capability registers start at +0x100 (CAPLENGTH points to the operational ones). */
	e->sc_io_tag = 0;
	e->sc_io_hdl = ZY7_USB0_BASE + ZY7_USB_CAPLENGTH;
	e->sc_io_size = 0x100;
	caplength = REG32(ZY7_USB0_BASE + ZY7_USB_CAPLENGTH) & 0xff;
	device_printf(self, "id 0x%08x caplength 0x%x\n",
	    (unsigned)REG32(ZY7_USB0_BASE), (unsigned)caplength);

	e->sc_flags |= EHCI_SCFLG_NORESTERM | EHCI_SCFLG_TT;
	e->sc_vendor_post_reset = zynq_ehci_post_reset;
	e->sc_vendor_get_port_speed = ehci_get_port_speed_portsc;

	sc = rtems_interrupt_server_handler_install(RTEMS_INTERRUPT_SERVER_DEFAULT,
	    ZYNQ_IRQ_USB_0, "USB0", RTEMS_INTERRUPT_UNIQUE,
	    zynq_ehci_intr, e);
	BSD_ASSERT_SC(sc);

	ue = ehci_init(e);
	if (ue != USB_ERR_NORMAL_COMPLETION) {
		device_printf(self, "ehci_init failed: %d\n", (int)ue);
		goto error;
	}
	/* ChipIdea: transfer completions also (and sometimes only) arrive as UAI/UPI */
	e->sc_eintrs |= ZY7_USBSTS_UAI | ZY7_USBSTS_UPI;
	EOWRITE4(e, EHCI_USBINTR, e->sc_eintrs);

	/* Host mode is set now: let the PHY drive VBUS and enable the host pull-downs. */
	if (zynq_ulpi_write(ULPI_OTG_CTRL_SET, ULPI_OTG_HOST_VBUS) != 0)
		device_printf(self, "ULPI OTG control write timed out\n");

	if (device_probe_and_attach(e->sc_bus.bdev) != 0)
		goto error;

	zynq_ehci_sc = e;
	callout_init(&zynq_ehci_poll, 1);
	callout_reset(&zynq_ehci_poll, hz / 2, zynq_ehci_poll_tick, NULL);

	return (0);

error:
	zynq_ehci_detach(self);
	return (ENXIO);
}

static int
zynq_ehci_detach(device_t self)
{
	(void)self;
	return (0);
}

static device_method_t zynq_ehci_methods[] = {
	DEVMETHOD(device_probe, zynq_ehci_probe),
	DEVMETHOD(device_attach, zynq_ehci_attach),
	DEVMETHOD(device_detach, zynq_ehci_detach),
	DEVMETHOD(device_suspend, bus_generic_suspend),
	DEVMETHOD(device_resume, bus_generic_resume),
	DEVMETHOD(device_shutdown, bus_generic_shutdown),
	DEVMETHOD(bus_print_child, bus_generic_print_child),
	{0, 0}
};

static driver_t zynq_ehci_driver = {
	.name = "ehci",
	.methods = zynq_ehci_methods,
	.size = sizeof(struct ehci_softc)
};

DRIVER_MODULE(ehci, nexus, zynq_ehci_driver, 0, 0);
MODULE_DEPEND(ehci, usb, 1, 1, 1);

#endif /* LIBBSP_ARM_XILINX_ZYNQ_BSP_H */
