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

/*
 * AD9363 FIR state from plain register reads only. The no-OS ad9361_get_rx/tx_fir_config read the
 * coefficient RAM: they set FIR_START_CLK without forcing the ENSM to ALERT and leave FIR_SELECT at
 * the one channel they read (1 instead of the 3 loaded), which stops the filter's data path while
 * the ENSM is in FDD (RX DMA frozen mid-block). Never call them; decode these registers instead
 * (names from no-OS ad9361.h):
 *   0x0F5 REG_RX_FILTER_CONFIG  [7:5] NUMBER_OF_TAPS (n/16-1), [4:3] SELECT_RX_CH, [1] START_RX_CLOCK
 *   0x0F6 REG_RX_FILTER_GAIN    [1:0] FILTER_GAIN (gain = 6 - 6 * field dB)
 *   0x065 REG_TX_FILTER_CONF    [7:5] FIR_NUM_TAPS, [4:3] FIR_SELECT, [1] FIR_START_CLK,
 *                               [0] TX_FIR_GAIN_6DB (-6 dB)
 *   0x003 REG_RX_ENABLE_FILTER_CTRL [1:0] RX_FIR_ENABLE_DECIMATION (0 off, 1 /1, 2 /2, 3 /4)
 *   0x002 REG_TX_ENABLE_FILTER_CTRL [1:0] TX_FIR_ENABLE_INTERPOLATION (same coding)
 * The coefficients themselves are verified once by ad9361_load_fir_filter_coef, in ALERT.
 */
#define PLUTO_REG_TX_ENABLE_FILTER_CTRL 0x002u
#define PLUTO_REG_RX_ENABLE_FILTER_CTRL 0x003u
#define PLUTO_REG_STATE 0x017u
#define PLUTO_REG_TX_FILTER_CONF 0x065u
#define PLUTO_REG_RX_FILTER_CONFIG 0x0f5u
#define PLUTO_REG_RX_FILTER_GAIN 0x0f6u
#define PLUTO_ENSM_ALERT 0x5u
#define PLUTO_ENSM_FDD 0xau

struct pluto_fir_regs {
  uint8_t rx_conf, rx_gain, tx_conf, rx_en, tx_en;
};
struct pluto_fir_state {
  uint32_t rx_taps, tx_taps;
  uint32_t rx_dec, tx_int; /* 0 = FIR bypassed */
  int rx_gain_db, tx_gain_db;
  unsigned rx_select, tx_select;      /* 3 = both channels, as loaded */
  int rx_clk_started, tx_clk_started; /* START_CLK left set: coefficient access mode */
};
void pluto_fir_decode(const struct pluto_fir_regs *r, struct pluto_fir_state *s);
/* mismatch bits of pluto_fir_check (0 = matches the loaded set) */
#define PLUTO_FIR_BAD_RX_TAPS 0x001
#define PLUTO_FIR_BAD_TX_TAPS 0x002
#define PLUTO_FIR_BAD_RX_DEC 0x004
#define PLUTO_FIR_BAD_TX_INT 0x008
#define PLUTO_FIR_BAD_RX_GAIN 0x010
#define PLUTO_FIR_BAD_TX_GAIN 0x020
#define PLUTO_FIR_BAD_RX_SELECT 0x040
#define PLUTO_FIR_BAD_TX_SELECT 0x080
#define PLUTO_FIR_BAD_RX_CLK 0x100
#define PLUTO_FIR_BAD_TX_CLK 0x200
int pluto_fir_check(const struct pluto_fir_state *s, uint32_t taps, uint32_t dec, int rx_gain_db,
                    int tx_gain_db);

/*
 * Poll the ENSM state (REG_STATE [3:0]) until it reads `want`: up to `polls` reads `gap_us` apart.
 * 0 when reached, -EIO when not (or on an SPI error); *last = the last state read (or the error).
 */
int pluto_ensm_wait(const struct pluto_probe_ops *ops, unsigned want, uint32_t polls, uint32_t gap_us,
                    int *last);

#ifdef __cplusplus
}
#endif
#endif
