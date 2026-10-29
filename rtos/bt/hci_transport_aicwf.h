/*
 * BTstack HCI transport over the AIC8800 USB Bluetooth controller (aicwf_bt.h, H:4 framing).
 * Sending is synchronous (USB control transfer for commands, bulk OUT for ACL), so
 * can_send_packet_now is NULL and hci.c emits the packet-sent events itself. A reader thread
 * splits the received H:4 stream into packets in a fixed slot queue and wakes the run loop; the
 * run loop thread delivers them to BTstack. No allocation after start.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HCI_TRANSPORT_AICWF_H
#define HCI_TRANSPORT_AICWF_H

#include "hci_transport.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const hci_transport_t *hci_transport_aicwf_instance(void);

/* reader thread scheduling (POSIX SCHED_FIFO priority, CPU index or -1 = any) before open */
void hci_transport_aicwf_set_thread(int priority, int cpu);

struct hci_transport_aicwf_stats {
  uint32_t rx_packets, rx_dropped, rx_resync, tx_packets, tx_errors;
  int failed; /* a send failed since open: the controller is gone or wedged */
};
void hci_transport_aicwf_get_stats(struct hci_transport_aicwf_stats *st);

#ifdef __cplusplus
}
#endif

#endif
