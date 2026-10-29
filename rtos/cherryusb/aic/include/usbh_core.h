/*
 * CherryUSB host API for the RT-Smart AIC8800 driver in the USB_STACK=cherry image.
 *
 * The driver (rtos/libbsd/aic8800/rtsmart, unchanged) was written against the K230 CherryUSB
 * snapshot with a DWC2 host. This header sits first on the driver's include path, includes the
 * real CherryUSB core header and bridges the differences:
 *  - struct usbh_class_info: the driver's tables use the K230 layout (class/subclass/protocol,
 *    vid/pid). They become plain constant tables of that layout here; aicwf_cherry.c matches
 *    them at connect time behind one CherryUSB class driver (CLASS_INFO_DEFINE is empty).
 *  - usbh_submit_urb / usbh_kill_urb / usbh_control_transfer go to aicwf_cherry.c, which keeps
 *    the semantics the driver relies on: completions on a giveback thread (never in the
 *    interrupt handler or the caller), a synchronous kill, any number of URBs queued per
 *    endpoint (the EHCI port gets one at a time, data toggle carried over), URB_ZERO_PACKET,
 *    cache maintenance and bounce buffers for unaligned buffers.
 *  - usbh_hubport_reenumerate() and usbh_dwc2_reset_endpoint() (K230 additions).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AICWF_CHERRY_USBH_CORE_H
#define AICWF_CHERRY_USBH_CORE_H

#include_next <usbh_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* K230 transfer flags (the EHCI port has none; aicwf_cherry.c implements them) */
#ifndef URB_SHORT_NOT_OK
#define URB_SHORT_NOT_OK 0x0001
#endif
#ifndef URB_ZERO_PACKET
#define URB_ZERO_PACKET 0x0040
#endif

/* K230 class-info layout of the driver's tables, matched by aicwf_cherry.c */
struct aicwf_k230_class_info {
    uint8_t match_flags;
    uint8_t class;
    uint8_t subclass;
    uint8_t protocol;
    uint16_t vid;
    uint16_t pid;
    const struct usbh_class_driver *class_driver;
};

#undef CLASS_INFO_DEFINE
#define CLASS_INFO_DEFINE

#undef USBH_EP_INIT
#define USBH_EP_INIT(ep, ep_desc) do { (ep) = (ep_desc); } while (0)

int aicwf_cherry_submit_urb(struct usbh_urb *urb);
int aicwf_cherry_kill_urb(struct usbh_urb *urb);
int aicwf_cherry_control_transfer(struct usbh_hubport *hport, struct usb_setup_packet *setup,
                                  uint8_t *buffer);
/* queue a port reset + re-enumeration of the device (returns at once) */
int usbh_hubport_reenumerate(struct usbh_hubport *hport);
/* host side of an endpoint reset: next transfer starts with DATA0 */
int usbh_dwc2_reset_endpoint(struct usbh_hubport *hport, uint8_t endpoint);

/* RT-Smart entry point that the RTEMS shim starts instead of INIT_COMPONENT_EXPORT */
void aicwf_usbh_start(void);

#ifndef AICWF_CHERRY_ADAPTER
#define usbh_class_info       aicwf_k230_class_info
#define usbh_submit_urb       aicwf_cherry_submit_urb
#define usbh_kill_urb         aicwf_cherry_kill_urb
#define usbh_control_transfer aicwf_cherry_control_transfer
#endif

#ifdef __cplusplus
}
#endif

#endif /* AICWF_CHERRY_USBH_CORE_H */
