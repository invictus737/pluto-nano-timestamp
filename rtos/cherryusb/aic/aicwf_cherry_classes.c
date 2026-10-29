/*
 * CherryUSB class table of the USB_STACK=cherry image.
 *
 * CherryUSB finds its class drivers between the linker symbols __usbh_class_info_start__ and
 * __usbh_class_info_end__ (objects placed in .usbh_class_info by CLASS_INFO_DEFINE). The RTEMS
 * linker command file has no such output section, so the table is an ordinary array here and
 * the two symbols are defined on it; the hub class's own CLASS_INFO_DEFINE object is unused.
 * Order matters (first match wins): hubs, then everything else to the AIC driver adapter,
 * which matches the driver's tables and the storage-mode IDs itself (aicwf_cherry.c).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbh_core.h"

extern const struct usbh_class_driver hub_class_driver;          /* class/hub/usbh_hub.c */
extern const struct usbh_class_driver aicwf_cherry_class_driver; /* aicwf_cherry.c */

#define NX_CLASS_COUNT 2

__attribute__((used)) const struct usbh_class_info nexus_usbh_class_table[NX_CLASS_COUNT] = {
    {
        .match_flags = USB_CLASS_MATCH_INTF_CLASS,
        .bInterfaceClass = USB_DEVICE_CLASS_HUB,
        .class_driver = &hub_class_driver,
    },
    {
        .match_flags = 0, /* any interface */
        .class_driver = &aicwf_cherry_class_driver,
    },
};

/* the end symbol below assumes this size: a layout change must fail the build */
_Static_assert(sizeof(nexus_usbh_class_table) == 32, "struct usbh_class_info is not 16 bytes");

__asm__(".globl __usbh_class_info_start__\n\t"
        ".set __usbh_class_info_start__, nexus_usbh_class_table\n\t"
        ".globl __usbh_class_info_end__\n\t"
        ".set __usbh_class_info_end__, nexus_usbh_class_table + 32\n\t");
