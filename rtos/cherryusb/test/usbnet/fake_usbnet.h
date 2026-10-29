/*
 * Host fakes for the usbnet glue test (fake_usbnet.c).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef FAKE_USBNET_H
#define FAKE_USBNET_H

#include <stdbool.h>
#include <stdint.h>

enum { FK_RTL, FK_ECM, FK_NCM, FK_ASIX, FK_N };

struct fake_dev_stats {
    unsigned rx_threads_alive, rx_threads_max, rx_threads_started;
    unsigned tx_ok, tx_refused, tx_bad_pad, tx_bad_data;
    unsigned refused; /* second adapters of the class refused (-USB_ERR_BUSY), still plugged */
};
struct fake_if_stats {
    unsigned long attaches, detaches, rx_ok, rx_bad, tx_ok, tx_err, link_ups, link_downs;
};

void fake_plug(int k);
void fake_unplug(int k);
void fake_cable(int k, bool up);
int fake_rx_push(int k, uint32_t len, uint32_t seq);
unsigned fake_rx_pending(int k);
void fake_dev_stats(int k, struct fake_dev_stats *st);
int fake_wait_rx_gone(int k, unsigned ms);
struct usbh_hubport *fake_hport(int k);

void fake_frame(uint8_t *buf, uint32_t len, uint32_t seq);
int fake_frame_check(const uint8_t *buf, uint32_t len);

void fake_set_configs(int k, uint16_t vid, uint16_t pid, int n, const uint8_t *const desc[], const uint16_t len[]);
extern unsigned g_ctrl_count;

int fake_if_transmit(const char *name, uint32_t len, uint32_t seq);
int fake_if_link(const char *name); /* 1 up, 0 down, -1 never set, -2 no such interface */
int fake_if_count(void);
void fake_if_stats(struct fake_if_stats *st);

#endif
