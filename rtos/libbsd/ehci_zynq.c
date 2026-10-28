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
#include <dev/usb/controller/ehci.h>
#include <dev/usb/controller/ehcireg.h>

#define ZY7_USB0_BASE			0xe0002000U
#define ZY7_USB_CAPLENGTH		0x0100
#define ZY7_USB_TXFILLTUNING		0x0164
#define ZY7_USB_ULPI_VIEWPORT		0x0170
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

static void
zynq_ehci_post_reset(struct ehci_softc *e)
{
	uint32_t usbmode, portsc, fill;

	usbmode = EOREAD4(e, EHCI_USBMODE_NOLPM);
	usbmode &= ~EHCI_UM_CM;
	usbmode |= EHCI_UM_CM_HOST;
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

	zynq_ulpi_phy_reset();

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
	    (rtems_interrupt_handler)ehci_interrupt, e);
	BSD_ASSERT_SC(sc);

	ue = ehci_init(e);
	if (ue != USB_ERR_NORMAL_COMPLETION) {
		device_printf(self, "ehci_init failed: %d\n", (int)ue);
		goto error;
	}

	/* Host mode is set now: let the PHY drive VBUS and enable the host pull-downs. */
	if (zynq_ulpi_write(ULPI_OTG_CTRL_SET, ULPI_OTG_HOST_VBUS) != 0)
		device_printf(self, "ULPI OTG control write timed out\n");

	if (device_probe_and_attach(e->sc_bus.bdev) != 0)
		goto error;

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
