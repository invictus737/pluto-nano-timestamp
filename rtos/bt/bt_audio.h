/*
 * Audio path from the station to the A2DP encoder: a lock-free single-producer/single-consumer
 * ring of 16-bit stereo frames at the producer's rate, and on the consumer side a polyphase
 * windowed-sinc resampler to the negotiated A2DP rate (44.1/48 kHz) with slow drift
 * compensation (producer and headset clocks are independent) and latency catch-up.
 * Static buffers only. No Bluetooth dependency (host-testable: rtos/bt/test).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NEXUS_BT_AUDIO_H
#define NEXUS_BT_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* producer (one thread at a time): interleaved 16-bit, channels 1 or 2, rate 8000..96000.
 * A format change drops what the consumer has not read of the previous format. Returns the
 * frames stored (fewer when the ring is full). */
size_t bt_audio_push(const int16_t *pcm, size_t frames, unsigned rate, unsigned channels);

/* consumer: n output frames at out_rate with out_channels (1 or 2) into out. Returns how many
 * came from data; the rest is silence (underrun). */
unsigned bt_audio_pull(int16_t *out, unsigned n, unsigned out_rate, unsigned out_channels);

/* consumer: buffered input in milliseconds (0 when nothing has been written yet) */
unsigned bt_audio_level_ms(void);

/* producer: buffered input in milliseconds at the producer's current rate */
unsigned bt_audio_fill_ms(void);

/* consumer: drop everything buffered */
void bt_audio_flush(void);

struct bt_audio_stats {
  uint64_t pushed;     /* input frames accepted */
  uint64_t overflow;   /* input frames refused: ring full */
  uint64_t catchup;    /* input frames dropped to bound latency */
  uint64_t underrun;   /* output frames of silence */
  unsigned in_rate, in_channels, out_rate;
};
void bt_audio_get_stats(struct bt_audio_stats *st);

/* latency policy (ms of input): steady-state target, catch-up threshold */
#define BT_AUDIO_TARGET_MS 150
#define BT_AUDIO_HIGH_MS 500

#ifdef __cplusplus
}
#endif

#endif
