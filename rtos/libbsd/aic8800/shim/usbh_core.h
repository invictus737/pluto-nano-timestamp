/*
 * CherryUSB host API subset used by the RT-Smart AIC8800 driver, implemented on the libbsd
 * USB host stack through aicwf_usbif.h (shim/usbh_shim.c). Structure members keep the
 * CherryUSB names; only those the driver uses exist.
 *
 * Semantics kept from the K230 CherryUSB/DWC2 port the driver was written for:
 *  - usbh_submit_urb() with urb->timeout != 0 waits for the transfer and returns its
 *    result; with timeout 0 it queues it and urb->complete(arg, nbytes or -USB_ERR_x) runs
 *    later on the completion thread (never in the caller's context).
 *  - usbh_kill_urb() cancels a queued transfer; its completion reports -USB_ERR_SHUTDOWN.
 *  - usbh_control_transfer() returns 8 + data bytes on success (DWC2 counts SETUP).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AICWF_USBH_CORE_H
#define AICWF_USBH_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#ifndef __PACKED
#define __PACKED __attribute__((packed, aligned(1)))
#endif
#ifndef __ALIGNED
#define __ALIGNED(x) __attribute__((aligned(x)))
#endif

#include "usb_def.h"
#include "usb_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_USB_ALIGN_SIZE               64
#define USB_MEM_ALIGNX                      __attribute__((aligned(CONFIG_USB_ALIGN_SIZE)))
#define USB_NOCACHE_RAM_SECTION
#define USB_ALIGN_UP(size, align)           (((size) + (align)-1) & ~((align)-1))

#define CONFIG_USBHOST_MAX_INTERFACES       8
#define CONFIG_USBHOST_MAX_INTF_ALTSETTINGS 1
#define CONFIG_USBHOST_MAX_ENDPOINTS        16
#define CONFIG_USBHOST_DEV_NAMELEN          16

#define USB_CLASS_MATCH_VENDOR        0x0001
#define USB_CLASS_MATCH_PRODUCT       0x0002
#define USB_CLASS_MATCH_INTF_CLASS    0x0004
#define USB_CLASS_MATCH_INTF_SUBCLASS 0x0008
#define USB_CLASS_MATCH_INTF_PROTOCOL 0x0010

#define URB_SHORT_NOT_OK    0x0001
#define URB_ZERO_PACKET     0x0040

/* class descriptors are collected by an explicit table in usbh_shim.c */
#define CLASS_INFO_DEFINE

#define USB_LOG_INFO(...)   ((void)0)

#define USBH_EP_INIT(ep, ep_desc)   do { (ep) = (ep_desc); } while (0)

typedef void (*usbh_complete_callback_t)(void *arg, int nbytes);

struct usbh_hubport;

struct usbh_urb
{
    void *hcpriv;
    struct usbh_hubport *hport;
    struct usb_endpoint_descriptor *ep;
    uint8_t data_toggle;
    uint8_t interval;
    struct usb_setup_packet *setup;
    uint8_t *transfer_buffer;
    uint32_t transfer_buffer_length;
    int transfer_flags;
    uint32_t actual_length;
    uint32_t timeout;
    int errorcode;
    usbh_complete_callback_t complete;
    void *arg;
    /* shim state */
    uint32_t shim_token;
    volatile uint32_t shim_state;
    void *shim_usbif;
    void *volatile shim_waiter;
};

struct usbh_class_driver
{
    const char *driver_name;
    int (*connect)(struct usbh_hubport *hport, uint8_t intf);
    int (*disconnect)(struct usbh_hubport *hport, uint8_t intf);
};

struct usbh_class_info
{
    uint8_t match_flags;
    uint8_t class;
    uint8_t subclass;
    uint8_t protocol;
    uint16_t vid;
    uint16_t pid;
    const struct usbh_class_driver *class_driver;
};

struct usbh_endpoint
{
    struct usb_endpoint_descriptor ep_desc;
};

struct usbh_interface_altsetting
{
    struct usb_interface_descriptor intf_desc;
    struct usbh_endpoint ep[CONFIG_USBHOST_MAX_ENDPOINTS];
};

struct usbh_interface
{
    char devname[CONFIG_USBHOST_DEV_NAMELEN];
    struct usbh_class_driver *class_driver;
    void *priv;
    struct usbh_interface_altsetting altsetting[CONFIG_USBHOST_MAX_INTF_ALTSETTINGS];
    uint8_t altsetting_num;
    /* shim state */
    void *shim_usbif;
    const struct usbh_class_info *shim_class;
    uint8_t shim_connected;
};

struct usbh_configuration
{
    struct usb_configuration_descriptor config_desc;
    struct usbh_interface intf[CONFIG_USBHOST_MAX_INTERFACES];
};

struct usbh_hubport
{
    volatile bool connected;
    uint8_t port;
    uint8_t dev_addr;
    uint8_t speed;
    struct usb_device_descriptor device_desc;
    struct usbh_configuration config;
    struct usb_setup_packet *setup;
    /* shim state */
    uintptr_t shim_key;
    int shim_refs;
};

int usbh_submit_urb(struct usbh_urb *urb);
int usbh_kill_urb(struct usbh_urb *urb);
int usbh_control_transfer(struct usbh_hubport *hport, struct usb_setup_packet *setup,
                          uint8_t *buffer);
int usbh_get_string_desc(struct usbh_hubport *hport, uint8_t index, uint8_t *output,
                         uint32_t output_len);
int usbh_hubport_reenumerate(struct usbh_hubport *hport);
int usbh_dwc2_reset_endpoint(struct usbh_hubport *hport, uint8_t endpoint);

static inline void usbh_bulk_urb_fill(struct usbh_urb *urb, struct usbh_hubport *hport,
                                      struct usb_endpoint_descriptor *ep,
                                      uint8_t *transfer_buffer,
                                      uint32_t transfer_buffer_length, uint32_t timeout,
                                      usbh_complete_callback_t complete, void *arg)
{
    urb->hport = hport;
    urb->ep = ep;
    urb->setup = NULL;
    urb->transfer_buffer = transfer_buffer;
    urb->transfer_buffer_length = transfer_buffer_length;
    urb->timeout = timeout;
    urb->complete = complete;
    urb->arg = arg;
    urb->transfer_flags = 0;
}

static inline void usbh_int_urb_fill(struct usbh_urb *urb, struct usbh_hubport *hport,
                                     struct usb_endpoint_descriptor *ep,
                                     uint8_t *transfer_buffer,
                                     uint32_t transfer_buffer_length, uint32_t timeout,
                                     usbh_complete_callback_t complete, void *arg)
{
    usbh_bulk_urb_fill(urb, hport, ep, transfer_buffer, transfer_buffer_length, timeout,
                       complete, arg);
}

/* RT-Smart entry points that the RTEMS shim starts instead of INIT_COMPONENT_EXPORT */
void aicwf_usbh_start(void);

#ifdef __cplusplus
}
#endif

#endif /* AICWF_USBH_CORE_H */
