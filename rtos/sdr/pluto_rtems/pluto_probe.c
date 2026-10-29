/*
 * AD9363 presence probe and rate plan (see pluto_probe.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "pluto_probe.h"

#include <errno.h>
#include <stddef.h>

void pluto_probe_default_cfg(struct pluto_probe_cfg *cfg)
{
  cfg->attempts = 8;
  cfg->reset_low_us = 10000;
  cfg->settle_us = 10000;
  cfg->reads = 3;
  cfg->read_gap_us = 1000;
  cfg->backoff_us = 50000;
  cfg->backoff_max_us = 400000;
}

int pluto_probe_id_ok(int v)
{
  /* an idle MISO reads 0x00 or 0xff: neither matches the mask */
  return v >= 0 && ((unsigned)v & PLUTO_PRODUCT_ID_MASK) == PLUTO_PRODUCT_ID_9361;
}

static void nap(const struct pluto_probe_ops *ops, struct pluto_probe_result *res, uint32_t us)
{
  if (us == 0)
    return;
  ops->sleep_us(ops->ctx, us);
  res->waited_us += us;
}

int pluto_probe_ad9363(const struct pluto_probe_ops *ops, const struct pluto_probe_cfg *cfg,
                       struct pluto_probe_result *res)
{
  struct pluto_probe_result local;
  if (!res)
    res = &local;
  res->attempts = 0;
  res->id = -EIO;
  res->first_bad = -1000;
  res->waited_us = 0;
  if (!ops || !ops->spi_read || !ops->reset || !ops->sleep_us || !cfg || cfg->attempts == 0)
    return -EINVAL;
  uint32_t backoff = cfg->backoff_us;
  for (uint32_t a = 0; a < cfg->attempts; ++a) {
    if (a > 0) {
      nap(ops, res, backoff);
      backoff = backoff * 2 > cfg->backoff_max_us ? cfg->backoff_max_us : backoff * 2;
    }
    res->attempts++;
    ops->reset(ops->ctx, 0);
    nap(ops, res, cfg->reset_low_us);
    ops->reset(ops->ctx, 1);
    nap(ops, res, cfg->settle_us);
    uint32_t good = 0;
    int ref = -1;
    for (uint32_t r = 0; r < (cfg->reads ? cfg->reads : 1); ++r) {
      if (r > 0)
        nap(ops, res, cfg->read_gap_us);
      int v = ops->spi_read(ops->ctx, PLUTO_REG_PRODUCT_ID);
      res->id = v;
      if (!pluto_probe_id_ok(v) || (ref >= 0 && v != ref)) {
        if (res->first_bad == -1000)
          res->first_bad = v;
        break;
      }
      ref = v;
      good++;
    }
    if (good == (cfg->reads ? cfg->reads : 1))
      return 0;
  }
  return -ENODEV;
}

/* libad9361 v0.3 ad9361_set_bb_rate FIR choice by the PHY rate */
static void fir_for(uint32_t phy_hz, uint32_t *dec, uint32_t *taps)
{
  if (phy_hz <= 20000000u) {
    *dec = 4; *taps = 128;
  } else if (phy_hz <= 40000000u) {
    *dec = 2; *taps = 128;
  } else if (phy_hz <= 53333333u) {
    *dec = 2; *taps = 96;
  } else {
    *dec = 2; *taps = 64;
  }
}

int pluto_rate_plan(uint32_t stream_hz, struct pluto_rate_plan *plan)
{
  if (!plan || stream_hz == 0)
    return -EINVAL;
  plan->stream_hz = stream_hz;
  /* SoapyPlutoSDR: samplerate < 25e6/48 -> decimation, PHY at 8x */
  plan->fpga_fir = (uint64_t)stream_hz * 48u < 25000000u;
  uint64_t phy = plan->fpga_fir ? (uint64_t)stream_hz * 8u : stream_hz;
  /* below 25 MHz/48 even with the PL x8 (65.1 kS/s) the AD9363 has no clock chain */
  if (phy > 61440000u || phy * 48u < 25000000u)
    return -EINVAL;
  plan->phy_hz = (uint32_t)phy;
  fir_for(plan->phy_hz, &plan->fir_dec, &plan->fir_taps);
  plan->via_fir_first = (uint64_t)plan->phy_hz * 12u <= 25000000u;
  return 0;
}

int pluto_fir_scale_taps(const int16_t *in, uint32_t n, uint32_t scale, int16_t *out)
{
  if (scale != 1 && scale != 2)
    return -EINVAL;
  for (uint32_t i = 0; i < n; ++i) {
    int32_t v = (int32_t)in[i] * (int32_t)scale;
    if (v > 32767 || v < -32768)
      return -ERANGE;
  }
  for (uint32_t i = 0; i < n; ++i)
    out[i] = (int16_t)((int32_t)in[i] * (int32_t)scale);
  return 0;
}

static uint32_t fir_factor(uint8_t field)
{
  field &= 3u;
  return field == 3u ? 4u : field; /* 0 bypassed, 1, 2, 3 = /4 */
}

void pluto_fir_decode(const struct pluto_fir_regs *r, struct pluto_fir_state *s)
{
  s->rx_taps = (((uint32_t)r->rx_conf >> 5) + 1u) * 16u;
  s->tx_taps = (((uint32_t)r->tx_conf >> 5) + 1u) * 16u;
  s->rx_dec = fir_factor(r->rx_en);
  s->tx_int = fir_factor(r->tx_en);
  s->rx_gain_db = 6 - 6 * (int)(r->rx_gain & 3u);
  s->tx_gain_db = (r->tx_conf & 1u) ? -6 : 0;
  s->rx_select = (r->rx_conf >> 3) & 3u;
  s->tx_select = (r->tx_conf >> 3) & 3u;
  s->rx_clk_started = (r->rx_conf >> 1) & 1u;
  s->tx_clk_started = (r->tx_conf >> 1) & 1u;
}

int pluto_fir_check(const struct pluto_fir_state *s, uint32_t taps, uint32_t dec, int rx_gain_db,
                    int tx_gain_db)
{
  int bad = 0;
  if (s->rx_taps != taps)
    bad |= PLUTO_FIR_BAD_RX_TAPS;
  if (s->tx_taps != taps)
    bad |= PLUTO_FIR_BAD_TX_TAPS;
  if (s->rx_dec != dec)
    bad |= PLUTO_FIR_BAD_RX_DEC;
  if (s->tx_int != dec)
    bad |= PLUTO_FIR_BAD_TX_INT;
  if (s->rx_gain_db != rx_gain_db)
    bad |= PLUTO_FIR_BAD_RX_GAIN;
  if (s->tx_gain_db != tx_gain_db)
    bad |= PLUTO_FIR_BAD_TX_GAIN;
  if (s->rx_select != 3u)
    bad |= PLUTO_FIR_BAD_RX_SELECT;
  if (s->tx_select != 3u)
    bad |= PLUTO_FIR_BAD_TX_SELECT;
  if (s->rx_clk_started)
    bad |= PLUTO_FIR_BAD_RX_CLK;
  if (s->tx_clk_started)
    bad |= PLUTO_FIR_BAD_TX_CLK;
  return bad;
}

int pluto_ensm_wait(const struct pluto_probe_ops *ops, unsigned want, uint32_t polls, uint32_t gap_us,
                    int *last)
{
  int v = -EIO;
  for (uint32_t i = 0; i < (polls ? polls : 1u); ++i) {
    if (i > 0 && gap_us)
      ops->sleep_us(ops->ctx, gap_us);
    v = ops->spi_read(ops->ctx, PLUTO_REG_STATE);
    if (v >= 0 && ((unsigned)v & 0xfu) == want) {
      if (last)
        *last = (int)((unsigned)v & 0xfu);
      return 0;
    }
  }
  if (last)
    *last = v < 0 ? v : (int)((unsigned)v & 0xfu);
  return -EIO;
}
