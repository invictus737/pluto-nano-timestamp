/*
 * BTstack configuration for the Nexus-BS RTOS image (RTEMS 6, Zynq-7010): Classic BR/EDR only,
 * A2DP source + AVRCP target over the AIC8800 USB controller (hci_transport_aicwf.c), run loop
 * btstack_run_loop_rtems.c, link keys in a TLV file under /data/bt.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

/* port features */
#define HAVE_ASSERT
#define HAVE_MALLOC /* connection/channel objects at connect time only, never per media packet */

/* BTstack features */
#define ENABLE_CLASSIC
#define ENABLE_LOG_ERROR
#ifdef NEXUS_BT_VERBOSE
#define ENABLE_LOG_INFO
#endif

/* buffers: the controller reports its own ACL size (AIC8800: about 1021); the host side holds
 * the largest A2DP media SDU in one buffer like the BTstack USB ports */
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_INCOMING_PRE_BUFFER_SIZE 0

/* paired devices kept in the TLV link key store */
#define NVM_NUM_LINK_KEYS 16
#define NVM_NUM_DEVICE_DB_ENTRIES 16

#endif
