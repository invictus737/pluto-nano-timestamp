/*
 * Nexus-BS RTOS Bluetooth: BTstack (Classic) on the AIC8800 USB combo stick (a69c:8d81 runtime;
 * the DC variant a69c:88dc has no Bluetooth), A2DP source + AVRCP target towards headphones and
 * speakers, pairing (SSP Just Works, legacy PIN 0000), link keys and known devices under
 * /data/bt, auto-reconnect to the last sink.
 *
 * Threads (all pinned to CPU0, below the radio on CPU1): BTstack run loop, HCI reader, test
 * tone. Configuration (/data/nexus.conf): bt.enabled (1/0; when unset the [SYSTEM] bluetooth
 * on/off key decides, default off), bt.autoconnect (1/0, default 1), bt.bitpool (SBC max
 * bitpool 19..53, default 53), bt.afh (1/0, default 1: keep the Wi-Fi 2.4 GHz channel out of
 * Bluetooth hopping via the AFH host channel classification).
 *
 * Shell: bt on|off|status|scan [secs]|pair <addr>|connect <addr>|disconnect [addr]|devices|
 *        forget <addr>|play tone|stop
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NEXUS_BT_H
#define NEXUS_BT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* once, after nexus_config_init(): starts the Bluetooth thread, powers the controller on when
 * enabled (waiting for the stick if it is not attached yet) and reconnects the last sink */
void nexus_bt_start(void);
void nexus_bt_shell_register(void);

/* Station audio towards the connected A2DP sink: 16-bit PCM, any rate 8000..96000 Hz,
 * resampled to the negotiated 44.1/48 kHz. Lock-free, never blocks, never allocates: callable
 * from the radio threads. One producer thread at a time. Returns the frames accepted (0 when
 * no stream is up, while `bt play tone` runs, or when the ring is full). */
size_t nexus_bt_audio_write(const int16_t *pcm, size_t frames, unsigned rate);           /* mono */
size_t nexus_bt_audio_write_ch(const int16_t *pcm, size_t frames, unsigned rate, unsigned channels);

/* 1 when an A2DP stream is up (audio written now is played) */
int nexus_bt_audio_active(void);

#ifdef __cplusplus
}
#endif

#endif
