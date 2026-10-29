/*
 * Host tests: AD9363 presence probe (reset pulse + bounded PRODUCT_ID retry), the stream rate ->
 * PHY/FIR/PL plan (SoapyPlutoSDR + libad9361 rules, Linux ground truth 200 kS/s -> PHY 1.6 MS/s,
 * FIR 128 taps /4, PL x8) and the stream level meter.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pluto_fir.h"
#include "pluto_framing.h"
#include "pluto_probe.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures, checks;
#define CHECK(c, ...)                                                                            \
  do {                                                                                           \
    checks++;                                                                                    \
    if (!(c)) {                                                                                  \
      failures++;                                                                                \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                \
      printf(__VA_ARGS__);                                                                       \
      printf("\n");                                                                              \
    }                                                                                            \
  } while (0)

/* chip model: answers 0xff (or 0x00) until `alive_after_us` of simulated time after a reset
 * release, then PRODUCT_ID 0x0a; optional glitch every `glitch_every` reads */
struct fake {
  uint64_t now_us, released_at;
  int level;
  uint64_t alive_after_us; /* UINT64_MAX: never */
  int dead_value;
  unsigned reads, glitch_every, pulses;
  int read_while_reset;
};

static int f_read(void *ctx, uint16_t reg)
{
  struct fake *f = ctx;
  f->reads++;
  if (reg != PLUTO_REG_PRODUCT_ID)
    return 0;
  if (f->level == 0) {
    f->read_while_reset++;
    return 0xff;
  }
  if (f->glitch_every && f->reads % f->glitch_every == 0)
    return 0x0b ^ 0xff;
  if (f->now_us < f->alive_after_us)
    return f->dead_value;
  return 0x0a;
}

static void f_reset(void *ctx, int level)
{
  struct fake *f = ctx;
  if (level && !f->level)
    f->released_at = f->now_us;
  if (!level && f->level)
    f->pulses++;
  f->level = level;
}

static void f_sleep(void *ctx, uint32_t us)
{
  ((struct fake *)ctx)->now_us += us;
}

static void test_probe(void)
{
  struct pluto_probe_cfg cfg;
  struct pluto_probe_result res;
  pluto_probe_default_cfg(&cfg);

  CHECK(!pluto_probe_id_ok(0xff) && !pluto_probe_id_ok(0x00) && !pluto_probe_id_ok(-5), "idle bus rejected");
  CHECK(pluto_probe_id_ok(0x0a) && pluto_probe_id_ok(0x08) && pluto_probe_id_ok(0x0f), "AD936x ids accepted");

  /* healthy: one pulse, 3 reads */
  struct fake f = {.level = 1, .alive_after_us = 0, .dead_value = 0xff};
  struct pluto_probe_ops ops = {&f, f_read, f_reset, f_sleep};
  CHECK(pluto_probe_ad9363(&ops, &cfg, &res) == 0, "healthy chip");
  CHECK(res.attempts == 1 && res.id == 0x0a, "healthy: %u attempts id 0x%x", res.attempts, res.id);
  CHECK(f.reads == 3 && f.read_while_reset == 0, "3 reads, none in reset (%u)", f.reads);
  CHECK(f.level == 1, "RESETB left released");
  CHECK(res.waited_us >= cfg.reset_low_us + cfg.settle_us, "reset low + settle honoured");

  /* the first-boot symptom: 0xff for 300 ms, then alive: recovered within the budget */
  struct fake g = {.level = 1, .alive_after_us = 300000, .dead_value = 0xff};
  ops.ctx = &g;
  CHECK(pluto_probe_ad9363(&ops, &cfg, &res) == 0, "late chip recovered");
  CHECK(res.attempts > 1 && res.first_bad == 0xff, "late chip: %u attempts, first bad 0x%x", res.attempts,
        res.first_bad);
  CHECK(g.pulses == res.attempts, "one reset pulse per attempt (%u/%u)", g.pulses, res.attempts);

  /* 0x00 variant */
  struct fake h = {.level = 1, .alive_after_us = 100000, .dead_value = 0x00};
  ops.ctx = &h;
  CHECK(pluto_probe_ad9363(&ops, &cfg, &res) == 0 && res.first_bad == 0, "0x00 then alive");

  /* dead: bounded */
  struct fake d = {.level = 1, .alive_after_us = UINT64_MAX, .dead_value = 0xff};
  ops.ctx = &d;
  CHECK(pluto_probe_ad9363(&ops, &cfg, &res) == -ENODEV, "dead chip -> -ENODEV");
  CHECK(res.attempts == cfg.attempts, "dead: all %u attempts", res.attempts);
  CHECK(res.waited_us < 2500000u, "dead: gave up after %u ms", res.waited_us / 1000u);
  printf("probe: dead chip gives up after %u ms, %u reset pulses\n", res.waited_us / 1000u, res.attempts);

  /* an inconsistent bus (one corrupted read in three) is not accepted on a single good read */
  struct fake n = {.level = 1, .alive_after_us = 0, .glitch_every = 2};
  ops.ctx = &n;
  CHECK(pluto_probe_ad9363(&ops, &cfg, &res) == -ENODEV, "glitching bus rejected");

  /* invalid arguments */
  CHECK(pluto_probe_ad9363(NULL, &cfg, &res) == -EINVAL, "NULL ops");
}

static void test_plan(void)
{
  struct pluto_rate_plan p;
  /* Linux ground truth on this Pluto: 200 kS/s DMA, PHY 1600000, FIR 128,4 */
  CHECK(pluto_rate_plan(200000, &p) == 0, "200k");
  CHECK(p.fpga_fir == 1 && p.phy_hz == 1600000 && p.fir_dec == 4 && p.fir_taps == 128 && p.via_fir_first == 1,
        "200k -> PL x8, PHY %u, FIR %u/%u", p.phy_hz, p.fir_taps, p.fir_dec);
  /* 100 kS/s: PHY 800000, FIR 128,4 (Linux evidence 2026-10-18) */
  CHECK(pluto_rate_plan(100000, &p) == 0 && p.phy_hz == 800000 && p.fpga_fir && p.fir_dec == 4, "100k");
  /* SoapyPlutoSDR threshold 25e6/(12*4) = 520833.3 */
  CHECK(pluto_rate_plan(520833, &p) == 0 && p.fpga_fir == 1 && p.phy_hz == 4166664, "520833 decimated");
  CHECK(pluto_rate_plan(520834, &p) == 0 && p.fpga_fir == 0 && p.phy_hz == 520834 && p.via_fir_first,
        "520834 not decimated");
  CHECK(pluto_rate_plan(3000000, &p) == 0 && !p.fpga_fir && !p.via_fir_first && p.fir_dec == 4, "3M");
  CHECK(pluto_rate_plan(30720000, &p) == 0 && p.fir_dec == 2 && p.fir_taps == 128, "30.72M");
  CHECK(pluto_rate_plan(50000000, &p) == 0 && p.fir_taps == 96, "50M");
  CHECK(pluto_rate_plan(61440000, &p) == 0 && p.fir_taps == 64, "61.44M");
  CHECK(pluto_rate_plan(61440001, &p) == -EINVAL, "above 61.44M");
  CHECK(pluto_rate_plan(65104, &p) == -EINVAL && pluto_rate_plan(65105, &p) == 0, "minimum 25e6/384");
  CHECK(pluto_rate_plan(0, &p) == -EINVAL, "zero");
}

static void test_level(void)
{
  struct pluto_level l;
  double pk, rms;
  /* full-scale complex tone on RX CS16 (12-bit LSB aligned, 2048): 0 dBFS */
  int16_t buf[2 * 64];
  for (int i = 0; i < 64; ++i) {
    double a = 2.0 * M_PI * i / 16.0;
    buf[2 * i] = (int16_t)lround(2047.0 * cos(a));
    buf[2 * i + 1] = (int16_t)lround(2047.0 * sin(a));
  }
  pluto_level_reset(&l);
  CHECK(pluto_level_dbfs(&l, PLUTO_FMT_CS16, PLUTO_RX_FULL_SCALE, &pk, &rms) == -1, "empty meter");
  pluto_level_add(&l, PLUTO_FMT_CS16, buf, 64);
  CHECK(pluto_level_dbfs(&l, PLUTO_FMT_CS16, PLUTO_RX_FULL_SCALE, &pk, &rms) == 0, "meter");
  CHECK(fabs(rms) < 0.05 && fabs(pk) < 0.05, "RX full-scale tone: peak %.2f rms %.2f dBFS", pk, rms);
  /* the same samples as TX CS16 (MSB aligned, 32768) read 24 dB lower */
  CHECK(pluto_level_dbfs(&l, PLUTO_FMT_CS16, PLUTO_TX_FULL_SCALE, &pk, &rms) == 0 && fabs(rms + 24.08) < 0.05,
        "TX scale: rms %.2f", rms);
  /* CF32 at -20 dBFS */
  float fb[2 * 64];
  for (int i = 0; i < 64; ++i) {
    double a = 2.0 * M_PI * i / 16.0;
    fb[2 * i] = (float)(0.1 * cos(a));
    fb[2 * i + 1] = (float)(0.1 * sin(a));
  }
  pluto_level_reset(&l);
  pluto_level_add(&l, PLUTO_FMT_CF32, fb, 32);
  pluto_level_add(&l, PLUTO_FMT_CF32, fb + 64, 32);
  CHECK(pluto_level_dbfs(&l, PLUTO_FMT_CF32, PLUTO_RX_FULL_SCALE, &pk, &rms) == 0 && fabs(rms + 20.0) < 0.05 &&
            fabs(pk + 20.0) < 0.05,
        "CF32 -20 dBFS: peak %.2f rms %.2f", pk, rms);
  /* CS8 */
  int8_t cb[4] = {127, 0, -127, 0};
  pluto_level_reset(&l);
  pluto_level_add(&l, PLUTO_FMT_CS8, cb, 2);
  CHECK(pluto_level_dbfs(&l, PLUTO_FMT_CS8, PLUTO_RX_FULL_SCALE, &pk, &rms) == 0 && fabs(pk + 0.07) < 0.05,
        "CS8 peak %.2f", pk);
}

static void test_fir_scale(void)
{
  int16_t out[128];
  int32_t ph[4] = {0, 0, 0, 0};
  int peak = 0;
  CHECK(pluto_fir_scale_taps(pluto_fir_128_4, 128, 2, out) == 0, "x2 fits int16");
  for (int i = 0; i < 128; ++i) {
    CHECK(out[i] == 2 * pluto_fir_128_4[i], "tap %d", i);
    ph[i % 4] += out[i];
    if (abs(out[i]) > peak)
      peak = abs(out[i]);
  }
  CHECK(peak == 31842, "largest scaled tap %d", peak);
  /* unity per output phase after the x4 zero stuffing: 32768 +- 0.1 % */
  for (int k = 0; k < 4; ++k)
    CHECK(abs(ph[k] - 32768) <= 33, "phase %d sum %d", k, (int)ph[k]);
  double mean = (ph[0] + ph[1] + ph[2] + ph[3]) / 4.0 / 32768.0;
  CHECK(fabs(20.0 * log10(mean)) < 0.01, "x2 passband %.3f dB", 20.0 * log10(mean));
  /* x1 = the Linux table: -6.02 dB */
  int32_t s1 = 0;
  for (int i = 0; i < 128; ++i)
    s1 += pluto_fir_128_4[i];
  CHECK(fabs(20.0 * log10(s1 / 4.0 / 32768.0) + 6.02) < 0.01, "x1 passband -6.02 dB");
  CHECK(pluto_fir_scale_taps(pluto_fir_128_4, 128, 1, out) == 0 && memcmp(out, pluto_fir_128_4, sizeof(out)) == 0,
        "x1 unchanged");
  /* the /2 tables (already unity) would overflow at x2: refused, output untouched */
  int16_t guard[128];
  memset(guard, 0x5a, sizeof(guard));
  CHECK(pluto_fir_scale_taps(pluto_fir_128_2, 128, 2, guard) == -ERANGE && guard[0] == 0x5a5a, "x2 of a /2 table refused");
  CHECK(pluto_fir_scale_taps(pluto_fir_128_4, 128, 3, out) == -EINVAL, "scale 3 refused");
  printf("fir: /4 table x2 per-phase sums %d %d %d %d, largest tap %d\n", (int)ph[0], (int)ph[1], (int)ph[2],
         (int)ph[3], peak);
}

/* register-only FIR verification (never the coefficient RAM) and the ENSM poll */
static void test_fir_regs(void)
{
  struct pluto_fir_state st;
  /* as ad9361_load_fir_filter_coef leaves 128 taps /4, RX -6 dB, TX 0 dB, select 3, clocks off:
   * RX conf NUM_TAPS 7 | SELECT 3 = 0xf8, gain field 2, TX conf 0xf8, enable fields 3 */
  struct pluto_fir_regs r = {0xf8, 0x02, 0xf8, 0x03, 0x03};
  pluto_fir_decode(&r, &st);
  CHECK(st.rx_taps == 128 && st.tx_taps == 128 && st.rx_dec == 4 && st.tx_int == 4, "decode taps/dec");
  CHECK(st.rx_gain_db == -6 && st.tx_gain_db == 0, "decode gains %d %d", st.rx_gain_db, st.tx_gain_db);
  CHECK(pluto_fir_check(&st, 128, 4, -6, 0) == 0, "loaded state matches");
  /* what ad9361_get_rx/tx_fir_config leave behind: FIR_SELECT 1 (and START_CLK while reading) */
  struct pluto_fir_regs bad = {0xea, 0x02, 0xeb, 0x03, 0x03};
  pluto_fir_decode(&bad, &st);
  int b = pluto_fir_check(&st, 128, 4, -6, 0);
  CHECK(b == (PLUTO_FIR_BAD_RX_SELECT | PLUTO_FIR_BAD_TX_SELECT | PLUTO_FIR_BAD_RX_CLK | PLUTO_FIR_BAD_TX_CLK |
              PLUTO_FIR_BAD_TX_GAIN),
        "readback damage detected: 0x%x", b);
  /* FIR bypassed, 64 taps /2, TX -6 dB */
  struct pluto_fir_regs o = {0x78, 0x01, 0x79, 0x00, 0x02};
  pluto_fir_decode(&o, &st);
  CHECK(st.rx_taps == 64 && st.rx_dec == 0 && st.tx_int == 2 && st.rx_gain_db == 0 && st.tx_gain_db == -6,
        "decode 64/off/2");
  CHECK(pluto_fir_check(&st, 128, 4, -6, 0) ==
            (PLUTO_FIR_BAD_RX_TAPS | PLUTO_FIR_BAD_TX_TAPS | PLUTO_FIR_BAD_RX_DEC | PLUTO_FIR_BAD_TX_INT |
             PLUTO_FIR_BAD_RX_GAIN | PLUTO_FIR_BAD_TX_GAIN),
        "mismatches flagged");
}

struct ensm_fake {
  int seq[8];
  unsigned n, reads;
  uint64_t slept;
};
static int ensm_read(void *ctx, uint16_t reg)
{
  struct ensm_fake *f = ctx;
  if (reg != PLUTO_REG_STATE)
    return 0;
  int v = f->seq[f->reads < f->n ? f->reads : f->n - 1];
  f->reads++;
  return v;
}
static void ensm_sleep(void *ctx, uint32_t us) { ((struct ensm_fake *)ctx)->slept += us; }
static void ensm_reset(void *ctx, int level) { (void)ctx; (void)level; }

static void test_ensm_wait(void)
{
  struct ensm_fake f = {{0x05, 0x05, 0x1a}, 3, 0, 0};
  struct pluto_probe_ops ops = {&f, ensm_read, ensm_reset, ensm_sleep};
  int last = -1;
  CHECK(pluto_ensm_wait(&ops, PLUTO_ENSM_FDD, 20, 100, &last) == 0 && last == 0xa && f.reads == 3 && f.slept == 200,
        "ALERT then FDD (upper bits ignored): reads %u last 0x%x", f.reads, (unsigned)last);
  struct ensm_fake g = {{0x05}, 1, 0, 0};
  ops.ctx = &g;
  CHECK(pluto_ensm_wait(&ops, PLUTO_ENSM_FDD, 20, 100, &last) == -EIO && last == 5 && g.reads == 20,
        "stuck in ALERT: -EIO after 20 reads, last 0x%x", (unsigned)last);
  struct ensm_fake h = {{-5}, 1, 0, 0};
  ops.ctx = &h;
  CHECK(pluto_ensm_wait(&ops, PLUTO_ENSM_FDD, 3, 0, &last) == -EIO && last == -5, "SPI error reported");
}

int main(void)
{
  test_fir_regs();
  test_ensm_wait();
  test_fir_scale();
  test_probe();
  test_plan();
  test_level();
  printf("test_probe: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
