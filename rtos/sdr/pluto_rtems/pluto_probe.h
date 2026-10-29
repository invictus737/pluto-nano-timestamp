/*
 * AD9363 presence probe and rate plan for the native Pluto driver. Pure logic (the register,
 * reset and delay accesses are callbacks), so the host unit tests run it unchanged.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PLUTO_PROBE_H
#define PLUTO_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* REG_PRODUCT_ID (0x037): [7:3] product 0x08 (AD9361/AD9363/AD9364), [2:0] revision */
#define PLUTO_REG_PRODUCT_ID 0x037u
#define PLUTO_PRODUCT_ID_MASK 0xf8u
#define PLUTO_PRODUCT_ID_9361 0x08u

struct pluto_probe_ops {
  void *ctx;
  /* one SPI register read: 0..255, or negative on a transfer error */
  int (*spi_read)(void *ctx, uint16_t reg);
  /* RESETB pin level (0 = held in reset) */
  void (*reset)(void *ctx, int level);
  void (*sleep_us)(void *ctx, uint32_t us);
};

struct pluto_probe_cfg {
  uint32_t attempts;      /* reset pulses at most */
  uint32_t reset_low_us;  /* RESETB low */
  uint32_t settle_us;     /* RESETB high to the first SPI access */
  uint32_t reads;         /* consecutive identical good reads required */
  uint32_t read_gap_us;
  uint32_t backoff_us;    /* after a failed attempt, doubled each time ... */
  uint32_t backoff_max_us; /* ... up to this */
};

/* Linux ad9361_reset() uses 1 ms / 1 ms and one read; the defaults are 10x that, 3 identical
 * reads, 8 attempts with 50..400 ms between them: at most ~2.2 s before giving up */
void pluto_probe_default_cfg(struct pluto_probe_cfg *cfg);

struct pluto_probe_result {
  uint32_t attempts; /* reset pulses issued */
  int id;            /* last PRODUCT_ID read (or negative SPI error) */
  int first_bad;     /* first wrong value seen (-1000 = none) */
  uint32_t waited_us; /* sleep time requested in total */
};

/* 1 when v is an AD936x PRODUCT_ID */
int pluto_probe_id_ok(int v);

/* reset pulse(s) + PRODUCT_ID verification; 0 when the transceiver answered, -ENODEV not */
int pluto_probe_ad9363(const struct pluto_probe_ops *ops, const struct pluto_probe_cfg *cfg,
                       struct pluto_probe_result *res);

/*
 * Stream rate -> AD9363 baseband plan (SoapyPlutoSDR + libad9361 ad9361_set_bb_rate rules).
 * Below 25 MHz/48 the PL x8 FIR decimator/interpolator is used and the AD9363 runs 8x faster:
 * 200 kS/s -> PHY 1.6 MS/s, AD9363 FIR 128 taps /4 (RX gain -6 dB, TX 0 dB), PL /8 - the state
 * the Linux firmware reaches (filter_fir_config "FIR Rx: 128,4 Tx: 128,4").
 */
struct pluto_rate_plan {
  uint32_t stream_hz;   /* requested */
  uint32_t phy_hz;      /* AD9363 RX/TX sampling frequency */
  int fpga_fir;         /* PL x8 decimator/interpolator on */
  uint32_t fir_dec;     /* AD9363 FIR decimation = interpolation */
  uint32_t fir_taps;
  int via_fir_first;    /* phy_hz <= 25 MHz/12: FIR on before the rate (no FIR-less chain) */
};
int pluto_rate_plan(uint32_t stream_hz, struct pluto_rate_plan *plan);

/*
 * TX FIR taps x scale (1 or 2). The libad9361 128-tap /4 table sums to 2^14 per output phase,
 * -6.02 dB after the x4 zero stuffing, while its /2 tables and the AD936x fixed interpolators sum to
 * full scale per phase (unity). scale 2 brings the /4 table to unity (largest tap 31842).
 * 0, or -ERANGE (nothing written) when a tap would leave int16, -EINVAL for another scale.
 */
int pluto_fir_scale_taps(const int16_t *in, uint32_t n, uint32_t scale, int16_t *out);

#ifdef __cplusplus
}
#endif
#endif
