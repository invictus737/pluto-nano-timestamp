/*
 * RT-Thread Bluetooth HCI device API (drivers/bt_hci.h) used by the RT-Smart AIC8800 USB
 * Bluetooth transport, implemented in shim/bt_hci_shim.c without the RT-Thread device
 * framework. The host side (a future BTstack port) uses aicwf_bt_*() in aicwf_bt.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AICWF_BT_HCI_H
#define AICWF_BT_HCI_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_BT_HCI_H4_CMD 0x01
#define RT_BT_HCI_H4_ACL 0x02
#define RT_BT_HCI_H4_SCO 0x03
#define RT_BT_HCI_H4_EVT 0x04
#define RT_BT_HCI_H4_ISO 0x05

struct rt_bt_hci_device;

/* Packet data passed through this API excludes the leading H:4 type byte. */
struct rt_bt_hci_ops
{
    rt_err_t (*open)(struct rt_bt_hci_device *hci);
    rt_err_t (*close)(struct rt_bt_hci_device *hci);
    rt_err_t (*send)(struct rt_bt_hci_device *hci, rt_uint8_t packet_type,
                     const rt_uint8_t *data, rt_size_t length);
};

struct rt_bt_hci_stats
{
    rt_uint32_t tx_packets;
    rt_uint32_t tx_bytes;
    rt_uint32_t tx_errors;
    rt_uint32_t rx_packets;
    rt_uint32_t rx_bytes;
    rt_uint32_t rx_dropped;
    rt_uint32_t rx_invalid;
};

struct rt_bt_hci_device
{
    struct rt_device parent;
    const struct rt_bt_hci_ops *ops;
    /* H:4 receive ring (type byte + packet), filled by rt_bt_hci_receive() */
    rt_uint8_t *rx_buffer;
    rt_size_t rx_size;
    rt_size_t rx_head;
    rt_size_t rx_count;
    struct rt_shim_wait rx_wait;
    struct rt_mutex tx_lock;
    struct rt_bt_hci_stats stats;
    rt_uint8_t opened;   /* host side (aicwf_bt_open) */
    rt_uint8_t link_up;  /* USB interface bound (driver connect/disconnect) */
};

rt_err_t rt_bt_hci_register(struct rt_bt_hci_device *hci, const char *name,
                            const struct rt_bt_hci_ops *ops,
                            rt_uint8_t *rx_buffer, rt_uint16_t rx_buffer_size,
                            void *user_data);
rt_err_t rt_bt_hci_register_auto(struct rt_bt_hci_device *hci,
                                 const struct rt_bt_hci_ops *ops,
                                 rt_uint8_t *rx_buffer, rt_uint16_t rx_buffer_size,
                                 void *user_data);
rt_err_t rt_bt_hci_send(struct rt_bt_hci_device *hci, rt_uint8_t packet_type,
                        const rt_uint8_t *data, rt_size_t length);
rt_err_t rt_bt_hci_receive(struct rt_bt_hci_device *hci, rt_uint8_t packet_type,
                           const rt_uint8_t *data, rt_size_t length);
void rt_bt_hci_flush(struct rt_bt_hci_device *hci);
rt_size_t rt_bt_hci_rx_length(struct rt_bt_hci_device *hci);
void rt_bt_hci_get_stats(struct rt_bt_hci_device *hci, struct rt_bt_hci_stats *stats);
/* driver: USB interface bound (1) or gone (0); going down fails the host's reads */
void rt_bt_hci_link(struct rt_bt_hci_device *hci, rt_bool_t up);
/* driver: the controller was reset under the host (firmware/patch stage ran again): the host's
 * reads fail until it closes and reopens */
void rt_bt_hci_reset_notify(struct rt_bt_hci_device *hci);

#ifdef __cplusplus
}
#endif

#endif /* AICWF_BT_HCI_H */
