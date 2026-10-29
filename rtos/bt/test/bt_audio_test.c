/*
 * Host test of rtos/bt/bt_audio.c: rate conversion accuracy (tone frequency, level, residual),
 * stereo pass-through, latency catch-up, format change, underrun accounting.
 * usage: rtos/bt/test/run.sh
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "../bt_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...)                                                                              \
  do {                                                                                             \
    if (!(c)) {                                                                                    \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                  \
      printf(__VA_ARGS__);                                                                         \
      printf("\n");                                                                                \
      failures++;                                                                                  \
    }                                                                                              \
  } while (0)

/* least-squares fit of a sine at freq: returns amplitude, residual rms via *res */
static double fit(const int16_t *x, int n, int stride, double fs, double freq, double *res)
{
  double sc = 0, ss = 0, cc = 0, sn = 0, cs = 0;
  for (int i = 0; i < n; ++i) {
    double ph = 2 * M_PI * freq * i / fs, s = sin(ph), c = cos(ph), v = x[i * stride];
    sn += v * s;
    cs += v * c;
    ss += s * s;
    cc += c * c;
    sc += s * c;
  }
  (void)sc;
  double a = sn / ss, b = cs / cc, e = 0;
  for (int i = 0; i < n; ++i) {
    double ph = 2 * M_PI * freq * i / fs, d = x[i * stride] - (a * sin(ph) + b * cos(ph));
    e += d * d;
  }
  *res = sqrt(e / n);
  return sqrt(a * a + b * b);
}

static void tone_case(unsigned inr, unsigned inch, unsigned outr, double freq)
{
  bt_audio_flush();
  const double amp = 8000;
  int16_t in[2 * 960];
  static int16_t out[2 * 48000];
  unsigned blk = inr / 50, got = 0, n_out = 0;
  double ph = 0;
  /* 20 ms producer blocks, 128-frame consumer pulls, producer kept ahead by the prebuffer */
  for (int b = 0; b < 60; ++b) {
    for (unsigned i = 0; i < blk; ++i) {
      int16_t v = (int16_t)lrint(amp * sin(ph));
      ph += 2 * M_PI * freq / inr;
      in[i * inch] = v;
      if (inch == 2) {
        in[i * 2 + 1] = (int16_t)(-v);
      }
    }
    CHECK(bt_audio_push(in, blk, inr, inch) == blk, "push");
    if (b < 8) {
      continue; /* 160 ms prebuffer */
    }
    while (n_out + 128 <= outr / 50 * (unsigned)(b - 7) && n_out + 128 <= 48000) {
      got += bt_audio_pull(out + 2 * n_out, 128, outr, 2);
      n_out += 128;
    }
  }
  CHECK(got == n_out, "underrun %u of %u", n_out - got, n_out);
  double res, a = fit(out + 2 * 2000, (int)n_out - 4000, 2, outr, freq, &res);
  double snr = 20 * log10(a / sqrt(2) / (res > 1e-9 ? res : 1e-9));
  printf("%5u Hz x%u -> %5u Hz: tone %.0f Hz amplitude %.1f (in %.0f), SNR %.1f dB\n", inr, inch, outr,
         freq, a, amp, snr);
  CHECK(fabs(a - amp) / amp < 0.02, "amplitude %.1f", a);
  CHECK(snr > 45, "snr %.1f", snr);
  if (inch == 2) {
    double res2, a2 = fit(out + 2 * 2000 + 1, (int)n_out - 4000, 2, outr, freq, &res2);
    CHECK(fabs(a2 - amp) / amp < 0.02, "right amplitude %.1f", a2);
  }
}

int main(void)
{
  tone_case(16000, 1, 48000, 1000);
  tone_case(8000, 1, 44100, 1000);
  tone_case(48000, 2, 48000, 1000);
  tone_case(48000, 2, 44100, 3000);
  tone_case(22050, 1, 48000, 440);

  /* catch-up: 2 s queued at 16 kHz -> bounded to the target */
  bt_audio_flush();
  static int16_t big[32000];
  memset(big, 0, sizeof(big));
  bt_audio_push(big, 32000, 16000, 1);
  int16_t o[256];
  bt_audio_pull(o, 128, 48000, 2);
  unsigned lvl = bt_audio_level_ms();
  CHECK(lvl <= BT_AUDIO_TARGET_MS + 5, "level after catch-up %u ms", lvl);

  /* format change drops the old format's unread frames */
  bt_audio_push(big, 1600, 8000, 1);
  CHECK(bt_audio_level_ms() == 200, "level after format change %u", bt_audio_level_ms());

  /* underrun: empty ring -> silence, counted */
  bt_audio_flush();
  struct bt_audio_stats st0, st1;
  bt_audio_get_stats(&st0);
  unsigned g = bt_audio_pull(o, 128, 48000, 2);
  bt_audio_get_stats(&st1);
  CHECK(g == 0 && st1.underrun - st0.underrun == 128, "underrun count g=%u n=%llu", g, (unsigned long long)(st1.underrun - st0.underrun));
  for (int i = 0; i < 256; ++i) {
    CHECK(o[i] == 0, "silence");
    if (o[i]) {
      break;
    }
  }
  printf(failures ? "bt_audio_test: %d FAILED\n" : "bt_audio_test: ok\n", failures);
  return failures != 0;
}
