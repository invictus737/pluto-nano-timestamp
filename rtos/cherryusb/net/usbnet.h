/*
 * USB Ethernet for the USB_STACK=cherry image: CherryUSB's own class drivers (RTL8152, CDC-ECM,
 * CDC-NCM, ASIX AX88772) glued to libbsd Ethernet ifnets ue0..ue7 (net/usbnet.c).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NEXUS_USBNET_H
#define NEXUS_USBNET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One line per attached adapter ("ue0 rtl8152 0bda:8152 link up ..."), for `net status`.
 * Returns the bytes written (0: none attached). Safe from any thread.
 */
size_t nexus_usbnet_status(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_USBNET_H */
