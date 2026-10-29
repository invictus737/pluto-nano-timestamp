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
