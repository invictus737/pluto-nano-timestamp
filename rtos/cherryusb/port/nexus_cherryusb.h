/*
 * CherryUSB host on the Pluto Nano (USB_STACK=cherry): application interface.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NEXUS_CHERRYUSB_H
#define NEXUS_CHERRYUSB_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up USB0 as the CherryUSB EHCI host (controller + ULPI PHY reset, host mode, VBUS) and
 * start enumeration. Needs the RTEMS interrupt server (rtems_bsd_initialize() starts it).
 * Waits up to one second for the controller to run; returns 0, or < 0 when it did not.
 */
int nexus_cherryusb_start(void);

/* USBMODE.SDIS (stream disable) in host mode; set before nexus_cherryusb_start(). Default 0
 * (Linux parity); config key usb_sdis=1 sets it. */
extern int nexus_cherryusb_sdis;
/* a69c:5721 driver CD: 0 leaves it alone (config key usb_aic_dc_switch=0). Default 1. */
extern int nexus_cherryusb_aic_dc_switch;

/* `usbh` shell command: controller registers and glue counters */
int nexus_cherryusb_shell(int argc, char **argv);

/* drains CPU and L2C-310 write buffers (osal/usb_osal_rtems.c) */
void usb_osal_dma_barrier(void);

/* CherryUSB core: `lsusb [-t|-v]` */
int lsusb(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_CHERRYUSB_H */
