/*
 * Station audio ring and resampler for the A2DP source (see bt_audio.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "bt_audio.h"

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#define RING_BITS 16 /* 65536 stereo frames: 1.36 s at 48 kHz, 8 s at 8 kHz */
#define RING_FRAMES (1u << RING_BITS)
#define RING_MASK (RING_FRAMES - 1)

#define TAPS 32
#define PH_BITS 7
#define PHASES (1 << PH_BITS)

static int16_t ring[RING_FRAMES][2];
static atomic_uint ring_w, ring_r;

/* producer-published format (seqlock): fmt_pos = ring index where it starts */
static atomic_uint fmt_seq, fmt_pos, fmt_rate, fmt_ch;
static unsigned prod_rate, prod_ch; /* producer's private copy */

static atomic_ullong st_pushed, st_overflow, st_catchup, st_underrun;

/* ---- consumer state ---- */
static unsigned seen_seq;
static unsigned in_rate, in_ch;
static unsigned rs_in, rs_out, rs_ch;
static float coef[PHASES + 1][TAPS];
static float win[2][2 * TAPS];
static unsigned win_idx;
static uint64_t acc; /* 32.32 position past the window anchor */
static uint64_t base_step;

size_t bt_audio_push(const int16_t *pcm, size_t frames, unsigned rate, unsigned channels)
{
  if (pcm == NULL || frames == 0 || rate < 8000 || rate > 96000 || (channels != 1 && channels != 2)) {
    return 0;
  }
  unsigned w = atomic_load_explicit(&ring_w, memory_order_relaxed);
  if (rate != prod_rate || channels != prod_ch) {
    unsigned s = atomic_load_explicit(&fmt_seq, memory_order_relaxed);
    atomic_store_explicit(&fmt_seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&fmt_pos, w, memory_order_relaxed);
    atomic_store_explicit(&fmt_rate, rate, memory_order_relaxed);
    atomic_store_explicit(&fmt_ch, channels, memory_order_relaxed);
    atomic_store_explicit(&fmt_seq, s + 2, memory_order_release);
    prod_rate = rate;
    prod_ch = channels;
  }
  unsigned r = atomic_load_explicit(&ring_r, memory_order_acquire);
  size_t room = RING_FRAMES - (w - r);
  size_t n = frames < room ? frames : room;
  if (channels == 1) {
    for (size_t i = 0; i < n; ++i) {
      int16_t s = pcm[i];
      ring[(w + i) & RING_MASK][0] = s;
      ring[(w + i) & RING_MASK][1] = s;
    }
  } else {
    for (size_t i = 0; i < n; ++i) {
      ring[(w + i) & RING_MASK][0] = pcm[2 * i];
      ring[(w + i) & RING_MASK][1] = pcm[2 * i + 1];
    }
  }
  atomic_store_explicit(&ring_w, w + (unsigned)n, memory_order_release);
  atomic_fetch_add_explicit(&st_pushed, n, memory_order_relaxed);
  if (n < frames) {
    atomic_fetch_add_explicit(&st_overflow, frames - n, memory_order_relaxed);
  }
  return n;
}

/* windowed sinc, rows = fractional output positions, unity DC gain per row */
static void rs_configure(unsigned inr, unsigned outr)
{
  const double pi = 3.14159265358979323846;
  double fc = 0.45 * (double)(inr < outr ? inr : outr) / (double)inr; /* cycles per input sample */
  for (int p = 0; p <= PHASES; ++p) {
    double f = (double)p / PHASES;
    double sum = 0.0;
    double h[TAPS];
    for (int j = 0; j < TAPS; ++j) {
      double d = (double)(j - TAPS / 2 + 1) - f;
      double x = 2.0 * fc * d;
      double sinc = fabs(x) < 1e-9 ? 1.0 : sin(pi * x) / (pi * x);
      double t = d / (TAPS / 2);
      double w = fabs(t) >= 1.0 ? 0.0 : 0.42 + 0.5 * cos(pi * t) + 0.08 * cos(2.0 * pi * t);
      h[j] = 2.0 * fc * sinc * w;
      sum += h[j];
    }
    for (int j = 0; j < TAPS; ++j) {
      coef[p][j] = (float)(h[j] / sum);
    }
  }
  memset(win, 0, sizeof(win));
  win_idx = 0;
  acc = 1ull << 32; /* the first output waits for input */
  base_step = ((uint64_t)inr << 32) / outr;
  rs_in = inr;
  rs_out = outr;
  rs_ch = in_ch;
}

static inline int16_t clamp16(float y)
{
  int v = (int)(y >= 0.0f ? y + 0.5f : y - 0.5f);
  return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

/* consumer: pick up a format change published by the producer */
static void check_format(unsigned *r)
{
  unsigned s1 = atomic_load_explicit(&fmt_seq, memory_order_acquire);
  if (s1 == seen_seq || (s1 & 1u)) {
    return;
  }
  unsigned pos = atomic_load_explicit(&fmt_pos, memory_order_relaxed);
  unsigned rate = atomic_load_explicit(&fmt_rate, memory_order_relaxed);
  unsigned ch = atomic_load_explicit(&fmt_ch, memory_order_relaxed);
  atomic_thread_fence(memory_order_acquire);
  if (atomic_load_explicit(&fmt_seq, memory_order_relaxed) != s1) {
    return; /* changing right now: next pull */
  }
  seen_seq = s1;
  unsigned w = atomic_load_explicit(&ring_w, memory_order_acquire);
  if (pos - *r <= w - *r) {
    *r = pos; /* drop the unread frames of the previous format */
  }
  in_rate = rate;
  in_ch = ch;
}

unsigned bt_audio_pull(int16_t *out, unsigned n, unsigned out_rate, unsigned out_channels)
{
  unsigned r = atomic_load_explicit(&ring_r, memory_order_relaxed);
  check_format(&r);
  if (in_rate == 0 || out_rate == 0) {
    memset(out, 0, (size_t)n * out_channels * sizeof(int16_t));
    atomic_store_explicit(&ring_r, r, memory_order_release);
    atomic_fetch_add_explicit(&st_underrun, n, memory_order_relaxed);
    return 0;
  }
  if (rs_in != in_rate || rs_out != out_rate || rs_ch != in_ch) {
    rs_configure(in_rate, out_rate);
  }
  unsigned w = atomic_load_explicit(&ring_w, memory_order_acquire);
  unsigned avail = w - r;
  unsigned high = in_rate * BT_AUDIO_HIGH_MS / 1000u;
  if (avail > high) {
    unsigned keep = in_rate * BT_AUDIO_TARGET_MS / 1000u;
    atomic_fetch_add_explicit(&st_catchup, avail - keep, memory_order_relaxed);
    r = w - keep;
    avail = keep;
  }
  /* clock drift (two crystals: tens of ppm): outside +-50 ms of the target level consume up to
   * 0.1 % faster/slower, inaudible, until the level is back in the band */
  uint64_t step = base_step;
  {
    double target = (double)in_rate * BT_AUDIO_TARGET_MS / 1000.0;
    double band = (double)in_rate * 0.050;
    double dev = (double)avail - target;
    if (dev > band || dev < -band) {
      double err = (dev - (dev > 0 ? band : -band)) / target;
      err = err > 1.0 ? 1.0 : (err < -1.0 ? -1.0 : err);
      step = (uint64_t)((double)base_step * (1.0 + 0.001 * err));
    }
  }

  const bool mono = in_ch == 1;
  unsigned produced = 0;
  for (unsigned o = 0; o < n; ++o) {
    while (acc >= (1ull << 32)) {
      if (r == w) {
        goto underrun;
      }
      const int16_t *f = ring[r & RING_MASK];
      ++r;
      win[0][win_idx] = win[0][win_idx + TAPS] = (float)f[0];
      if (!mono) {
        win[1][win_idx] = win[1][win_idx + TAPS] = (float)f[1];
      }
      win_idx = (win_idx + 1) % TAPS;
      acc -= 1ull << 32;
    }
    {
      uint32_t frac = (uint32_t)acc;
      unsigned p = (unsigned)(((uint64_t)frac + (1ull << (31 - PH_BITS))) >> (32 - PH_BITS));
      const float *h = coef[p];
      const float *x0 = &win[0][win_idx];
      float y0 = 0.0f, y1 = 0.0f;
      for (int j = 0; j < TAPS; ++j) {
        y0 += x0[j] * h[j];
      }
      if (mono) {
        y1 = y0;
      } else {
        const float *x1 = &win[1][win_idx];
        for (int j = 0; j < TAPS; ++j) {
          y1 += x1[j] * h[j];
        }
      }
      if (out_channels == 1) {
        out[o] = clamp16(0.5f * (y0 + y1));
      } else {
        out[2 * o] = clamp16(y0);
        out[2 * o + 1] = clamp16(y1);
      }
      acc += step;
      ++produced;
    }
  }
underrun:
  if (produced < n) {
    memset(out + (size_t)produced * out_channels, 0, (size_t)(n - produced) * out_channels * sizeof(int16_t));
    atomic_fetch_add_explicit(&st_underrun, n - produced, memory_order_relaxed);
  }
  atomic_store_explicit(&ring_r, r, memory_order_release);
  return produced;
}

unsigned bt_audio_level_ms(void)
{
  unsigned r = atomic_load_explicit(&ring_r, memory_order_relaxed);
  check_format(&r);
  atomic_store_explicit(&ring_r, r, memory_order_release);
  unsigned w = atomic_load_explicit(&ring_w, memory_order_acquire);
  return in_rate ? (unsigned)((uint64_t)(w - r) * 1000u / in_rate) : 0;
}

unsigned bt_audio_fill_ms(void)
{
  unsigned w = atomic_load_explicit(&ring_w, memory_order_relaxed);
  unsigned r = atomic_load_explicit(&ring_r, memory_order_acquire);
  return prod_rate ? (unsigned)((uint64_t)(w - r) * 1000u / prod_rate) : 0;
}

void bt_audio_flush(void)
{
  unsigned r = atomic_load_explicit(&ring_r, memory_order_relaxed);
  check_format(&r);
  atomic_store_explicit(&ring_r, atomic_load_explicit(&ring_w, memory_order_acquire), memory_order_release);
  memset(win, 0, sizeof(win));
  acc = 1ull << 32;
}

void bt_audio_get_stats(struct bt_audio_stats *st)
{
  st->pushed = atomic_load(&st_pushed);
  st->overflow = atomic_load(&st_overflow);
  st->catchup = atomic_load(&st_catchup);
  st->underrun = atomic_load(&st_underrun);
  st->in_rate = in_rate;
  st->in_channels = in_ch;
  st->out_rate = rs_out;
}
