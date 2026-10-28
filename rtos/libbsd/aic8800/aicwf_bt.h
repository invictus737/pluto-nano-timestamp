/*
 * Bluetooth HCI (H:4) access to the AIC8800 controller through the RT-Smart USB
 * Bluetooth transport (aic8800_btusb.c). For a host stack such as BTstack:
 * open, send command/ACL packets, read the H:4 byte stream (type byte + packet).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AICWF_BT_H
#define AICWF_BT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AICWF_BT_H4_CMD 0x01
#define AICWF_BT_H4_ACL 0x02
#define AICWF_BT_H4_EVT 0x04

/* 0 when a controller is attached and its transport started */
int aicwf_bt_open(void);
int aicwf_bt_close(void);
/* one packet without the H:4 type byte */
int aicwf_bt_send(uint8_t packet_type, const uint8_t *data, size_t length);
/* up to length bytes of the received H:4 stream; 0 on timeout, < 0 on error */
int aicwf_bt_read(uint8_t *buffer, size_t length, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* AICWF_BT_H */
