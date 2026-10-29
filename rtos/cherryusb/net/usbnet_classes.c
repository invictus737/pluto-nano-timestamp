/*
 * CherryUSB class table of the USB_STACK=cherry image with USB Ethernet (CHERRY_USBNET=1).
 *
 * Replaces aic/aicwf_cherry_classes.c in the link (cherryusb.mk): CherryUSB finds its class
 * drivers between __usbh_class_info_start__ and __usbh_class_info_end__, the RTEMS link has
 * no .usbh_class_info section, so the table is an ordinary array with the two symbols on it
 * (same scheme as that file). First match wins: hubs, the USB Ethernet classes, a no-op
 * claim of CDC data interfaces (the ECM/NCM drivers drive interface intf + 1 themselves),
 * and last the AIC8800 adapter for everything else (it matches its own tables).
 * Keep the hub and AIC entries in step with aic/aicwf_cherry_classes.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbh_core.h"
#include "usb_cdc.h"

extern const struct usbh_class_driver hub_class_driver;          /* class/hub/usbh_hub.c */
extern const struct usbh_class_driver aicwf_cherry_class_driver; /* aic/aicwf_cherry.c */
extern const struct usbh_class_driver rtl8152_class_driver;      /* class/vendor/net (patch 0102) */
extern const struct usbh_class_driver asix_class_driver;         /* class/vendor/net (patch 0102) */
extern const struct usbh_class_driver cdc_ecm_class_driver;      /* class/cdc/usbh_cdc_ecm.c */
extern const struct usbh_class_driver cdc_ncm_class_driver;      /* class/cdc/usbh_cdc_ncm.c */

/* the upstream tables (static there): RTL8152 vendor mode, ASIX AX88772A/B */
static const uint16_t nx_rtl8152_ids[][2] = {
    { 0x0bda, 0x8152 },
    { 0, 0 },
};
static const uint16_t nx_asix_ids[][2] = {
    { 0x0b95, 0x772b },
    { 0x0b95, 0x7720 },
    { 0, 0 },
};

static int nx_cdc_data_connect(struct usbh_hubport *hport, uint8_t intf)
{
    (void)hport;
    (void)intf;
    return 0;
}

static int nx_cdc_data_disconnect(struct usbh_hubport *hport, uint8_t intf)
{
    (void)hport;
    (void)intf;
    return 0;
}

static const struct usbh_class_driver nx_cdc_data_driver = {
    .driver_name = "cdc_data",
    .connect = nx_cdc_data_connect,
    .disconnect = nx_cdc_data_disconnect,
};

#define NX_CLASS_COUNT 7

__attribute__((used)) const struct usbh_class_info nexus_usbh_class_table[NX_CLASS_COUNT] = {
    {
        .match_flags = USB_CLASS_MATCH_INTF_CLASS,
        .bInterfaceClass = USB_DEVICE_CLASS_HUB,
        .class_driver = &hub_class_driver,
    },
    {
        .match_flags = USB_CLASS_MATCH_VID_PID | USB_CLASS_MATCH_INTF_CLASS,
        .bInterfaceClass = 0xff,
        .id_table = nx_rtl8152_ids,
        .class_driver = &rtl8152_class_driver,
    },
    {
        .match_flags = USB_CLASS_MATCH_VID_PID | USB_CLASS_MATCH_INTF_CLASS,
        .bInterfaceClass = 0xff,
        .id_table = nx_asix_ids,
        .class_driver = &asix_class_driver,
    },
    {
        .match_flags = USB_CLASS_MATCH_INTF_CLASS | USB_CLASS_MATCH_INTF_SUBCLASS | USB_CLASS_MATCH_INTF_PROTOCOL,
        .bInterfaceClass = USB_DEVICE_CLASS_CDC,
        .bInterfaceSubClass = CDC_ETHERNET_NETWORKING_CONTROL_MODEL,
        .bInterfaceProtocol = CDC_COMMON_PROTOCOL_NONE,
        .class_driver = &cdc_ecm_class_driver,
    },
    {
        .match_flags = USB_CLASS_MATCH_INTF_CLASS | USB_CLASS_MATCH_INTF_SUBCLASS | USB_CLASS_MATCH_INTF_PROTOCOL,
        .bInterfaceClass = USB_DEVICE_CLASS_CDC,
        .bInterfaceSubClass = CDC_NETWORK_CONTROL_MODEL,
        .bInterfaceProtocol = CDC_COMMON_PROTOCOL_NONE,
        .class_driver = &cdc_ncm_class_driver,
    },
    {
        .match_flags = USB_CLASS_MATCH_INTF_CLASS,
        .bInterfaceClass = USB_DEVICE_CLASS_CDC_DATA,
        .class_driver = &nx_cdc_data_driver,
    },
    {
        .match_flags = 0, /* any interface */
        .class_driver = &aicwf_cherry_class_driver,
    },
};

/* the end symbol below assumes this size: a layout change must fail the build */
_Static_assert(sizeof(nexus_usbh_class_table) == NX_CLASS_COUNT * 16, "struct usbh_class_info is not 16 bytes");

__asm__(".globl __usbh_class_info_start__\n\t"
        ".set __usbh_class_info_start__, nexus_usbh_class_table\n\t"
        ".globl __usbh_class_info_end__\n\t"
        ".set __usbh_class_info_end__, nexus_usbh_class_table + 112\n\t");
