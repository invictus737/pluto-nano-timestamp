/*
 * Host test of the ADI no-OS ad9361 driver as the native Pluto driver builds it (fetch-noos.sh,
 * noos-patches applied) against a register-file model of the AD9363 and the axi_ad9361 core:
 *  - ad9361_init failure paths (dead SPI, a calibration that never completes, an ADC core without
 *    DATA_CLK) return an error, leave *phy NULL, never dereference a NULL core and free every
 *    allocation (counted through the weak no_os_malloc/calloc/free);
 *  - a successful init + ad9361_remove frees everything;
 *  - the libad9361 ad9361_set_bb_rate sequence the driver uses for 200 kS/s (PHY 1.6 MS/s, FIR
 *    128 taps /4, "RX 3 GAIN -6" / "TX 3 GAIN 0") lands on the Linux state: RX and TX at
 *    1.6 MS/s, FIR on both ways, CLKRF == CLKTF (a TX quadrature calibration case the driver
 *    handles), clock chain within the AD9361 limits.
 * The chip model is a 1024-byte register file plus the self-clearing / status bits the driver
 * polls; it is not an AD9363 simulator (no RF), only enough for the control flow.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ad9361.h"
#include "ad9361_api.h"
#include "axi_adc_core.h"
#include "axi_dac_core.h"
#include "no_os_alloc.h"
#include "no_os_axi_io.h"
#include "no_os_delay.h"
#include "no_os_gpio.h"
#include "no_os_spi.h"
#include "pluto_fir.h"
#include "pluto_probe.h"

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

/* ---- allocation accounting (overrides the weak util/no_os_alloc.c) ---- */

static long live_allocs;

void *no_os_malloc(size_t size)
{
  void *p = malloc(size);
  if (p)
    live_allocs++;
  return p;
}

void *no_os_calloc(size_t n, size_t size)
{
  void *p = calloc(n, size);
  if (p)
    live_allocs++;
  return p;
}

void no_os_free(void *p)
{
  if (p)
    live_allocs--;
  free(p);
}

/* ---- platform: time, AXI ---- */

void no_os_udelay(uint32_t us) { (void)us; }
void no_os_mdelay(uint32_t ms) { (void)ms; }
struct no_os_time no_os_get_time(void)
{
  struct no_os_time t = {0, 0};
  return t;
}

enum chip_mode { CHIP_DEAD, CHIP_NO_CP_CAL, CHIP_OK };
static enum chip_mode chip;
static int adc_core_clock; /* axi_ad9361 sees DATA_CLK */
static uint8_t regs[1024];

int32_t no_os_axi_io_read(uint32_t base, uint32_t offset, uint32_t *data)
{
  (void)base;
  if (offset == AXI_ADC_REG_STATUS)
    *data = adc_core_clock ? AXI_ADC_STATUS : 0;
  else if (offset == AXI_ADC_REG_CHAN_STATUS(0) || offset == AXI_ADC_REG_CHAN_STATUS(1))
    *data = 0; /* PN checker locked, no errors */
  else if (offset == 0x0004)
    *data = 0; /* ADI_REG_ID 0: single core, full interface tuning like the Pluto */
  else if (offset == 0x0000 || offset == 0x4000)
    *data = 0x000a0162; /* core version 10.1.b */
  else
    *data = 0;
  return 0;
}

int32_t no_os_axi_io_write(uint32_t base, uint32_t offset, uint32_t data)
{
  (void)base;
  (void)offset;
  (void)data;
  return 0;
}

/* ---- AD9363 register model behind the SPI ops ---- */

static uint8_t chip_read(uint16_t a)
{
  if (chip == CHIP_DEAD)
    return 0xff;
  switch (a) {
  case REG_PRODUCT_ID:
    return 0x0a; /* AD936x, rev 2 */
  case REG_CH_1_OVERFLOW:
    return regs[a] | BBPLL_LOCK;
  case REG_RX_CP_OVERRANGE_VCO_LOCK:
  case REG_TX_CP_OVERRANGE_VCO_LOCK:
    return regs[a] | VCO_LOCK;
  case REG_RX_CAL_STATUS:
  case REG_TX_CAL_STATUS:
    return chip == CHIP_NO_CP_CAL ? 0 : (uint8_t)(regs[a] | CP_CAL_VALID);
  default:
    return regs[a];
  }
}

static uint32_t filter_conf_writes, start_clk_in_fdd;

static void chip_write(uint16_t a, uint8_t v)
{
  if (a == REG_RX_FILTER_CONFIG || a == REG_TX_FILTER_CONF) {
    filter_conf_writes++;
    if ((v & FIR_START_CLK) && (regs[REG_STATE] & 0xf) == ENSM_STATE_FDD)
      start_clk_in_fdd++;
  }
  switch (a) {
  case REG_CALIBRATION_CTRL:
    regs[a] = 0; /* every calibration completes at once */
    return;
  case REG_ENSM_CONFIG_1:
    regs[a] = v;
    if (v & FORCE_TX_ON)
      regs[REG_STATE] = ENSM_STATE_FDD;
    else if (v & FORCE_RX_ON)
      regs[REG_STATE] = ENSM_STATE_RX;
    else if (v & FORCE_ALERT_STATE)
      regs[REG_STATE] = ENSM_STATE_ALERT;
    return;
  default:
    regs[a] = v;
  }
}

static int32_t fspi_init(struct no_os_spi_desc **desc, const struct no_os_spi_init_param *p)
{
  struct no_os_spi_desc *d = no_os_calloc(1, sizeof(*d));
  if (!d)
    return -1;
  d->device_id = p->device_id;
  *desc = d;
  return 0;
}

static int32_t fspi_xfer(struct no_os_spi_desc *desc, uint8_t *data, uint16_t len)
{
  (void)desc;
  if (len < 2)
    return -1;
  uint16_t cmd = (uint16_t)(data[0] << 8 | data[1]);
  int write = (cmd & AD_WRITE) != 0;
  uint16_t addr = cmd & 0x3ff;
  for (uint16_t i = 2; i < len; ++i, --addr) {
    if (write)
      chip_write(addr, data[i]);
    else
      data[i] = chip_read(addr);
  }
  return 0;
}

static int32_t fspi_remove(struct no_os_spi_desc *desc)
{
  no_os_free(desc);
  return 0;
}

static const struct no_os_spi_platform_ops fspi_ops = {
  .init = fspi_init, .write_and_read = fspi_xfer, .remove = fspi_remove};

static int reset_level = -1;

static int32_t fgpio_get(struct no_os_gpio_desc **desc, const struct no_os_gpio_init_param *p)
{
  struct no_os_gpio_desc *d = no_os_calloc(1, sizeof(*d));
  if (!d)
    return -1;
  d->number = p->number;
  *desc = d;
  return 0;
}

static int32_t fgpio_get_opt(struct no_os_gpio_desc **desc, const struct no_os_gpio_init_param *p)
{
  if (p->number < 0) {
    *desc = NULL;
    return 0;
  }
  return fgpio_get(desc, p);
}

static int32_t fgpio_remove(struct no_os_gpio_desc *d)
{
  no_os_free(d);
  return 0;
}

static int32_t fgpio_out(struct no_os_gpio_desc *d, uint8_t v)
{
  (void)d;
  reset_level = v;
  return 0;
}

static int32_t fgpio_set(struct no_os_gpio_desc *d, uint8_t v)
{
  (void)d;
  reset_level = v;
  return 0;
}

static const struct no_os_gpio_platform_ops fgpio_ops = {
  .gpio_ops_get = fgpio_get,
  .gpio_ops_get_optional = fgpio_get_opt,
  .gpio_ops_remove = fgpio_remove,
  .gpio_ops_direction_output = fgpio_out,
  .gpio_ops_set_value = fgpio_set,
};

/* ---- init parameters: the subset pluto_hw.c fill_init_param sets that drives control flow ---- */

static struct axi_adc_init adc_init = {.name = "cf-ad9361-lpc", .base = 0x79020000u, .num_channels = 2};
static struct axi_dac_init dac_init = {.name = "cf-ad9361-dds-core-lpc", .base = 0x79024000u, .num_channels = 2};

static void params(AD9361_InitParam *p)
{
  static const uint32_t rxc[6] = {983040000, 245760000, 122880000, 61440000, 30720000, 30720000};
  static const uint32_t txc[6] = {983040000, 122880000, 122880000, 61440000, 30720000, 30720000};
  memset(p, 0, sizeof(*p));
  p->dev_sel = ID_AD9363A;
  p->reference_clk_rate = 39999331;
  p->one_rx_one_tx_mode_use_rx_num = 1;
  p->one_rx_one_tx_mode_use_tx_num = 1;
  p->frequency_division_duplex_mode_enable = 1;
  p->trx_synthesizer_target_fref_overwrite_hz = MAX_SYNTH_FREF;
  p->rx_synthesizer_frequency_hz = 2400000000ull;
  p->tx_synthesizer_frequency_hz = 2450000000ull;
  p->tx_lo_powerdown_managed_enable = 1;
  memcpy(p->rx_path_clock_frequencies, rxc, sizeof(rxc));
  memcpy(p->tx_path_clock_frequencies, txc, sizeof(txc));
  p->rf_rx_bandwidth_hz = 18000000;
  p->rf_tx_bandwidth_hz = 18000000;
  p->tx_attenuation_mdB = 10000;
  p->xo_disable_use_ext_refclk_enable = 1;
  p->gc_rx1_mode = 2;
  p->gc_rx2_mode = 2;
  p->gc_dec_pow_measurement_duration = 8192;
  p->agc_gain_update_interval_us = 1000;
  p->rssi_duration = 1000;
  p->aux_adc_decimation = 256;
  p->aux_adc_rate = 40000000;
  p->temp_sense_decimation = 256;
  p->temp_sense_measurement_interval_ms = 1000;
  p->ctrl_outs_enable_mask = 0xff;
  p->ctrl_outs_index = 1;
  p->pp_tx_swap_enable = 1;
  p->pp_rx_swap_enable = 1;
  p->rx_frame_pulse_mode_enable = 1;
  p->swap_ports_enable = 1;
  p->full_port_enable = 1;
  p->tx_data_delay = 9;
  p->lvds_bias_mV = 150;
  p->tx_mon_delay = 511;
  p->tx_mon_duration = 8192;
  p->gpio_resetb.number = 67;
  p->gpio_resetb.platform_ops = &fgpio_ops;
  p->gpio_sync.number = -1;
  p->gpio_sync.platform_ops = &fgpio_ops;
  p->gpio_cal_sw1.number = -1;
  p->gpio_cal_sw1.platform_ops = &fgpio_ops;
  p->gpio_cal_sw2.number = -1;
  p->gpio_cal_sw2.platform_ops = &fgpio_ops;
  p->spi_param.device_id = 0;
  p->spi_param.max_speed_hz = 10000000;
  p->spi_param.mode = NO_OS_SPI_MODE_1;
  p->spi_param.platform_ops = &fspi_ops;
  p->rx_adc_init = &adc_init;
  p->tx_dac_init = &dac_init;
}

static void reset_chip(enum chip_mode m, int clock)
{
  memset(regs, 0, sizeof(regs));
  chip = m;
  adc_core_clock = clock;
}

static void test_failure(const char *name, enum chip_mode m, int clock)
{
  AD9361_InitParam p;
  params(&p);
  reset_chip(m, clock);
  long before = live_allocs;
  struct ad9361_rf_phy *phy = (struct ad9361_rf_phy *)0x1; /* must come back NULL */
  int32_t r = ad9361_init(&phy, &p);
  CHECK(r < 0, "%s: ad9361_init returned %d", name, (int)r);
  CHECK(phy == NULL, "%s: *phy not NULL after a failure", name);
  CHECK(live_allocs == before, "%s: %ld allocation(s) leaked", name, live_allocs - before);
}

/* libad9361 v0.3 ad9361_set_bb_rate, as pluto_hw.c set_bb_rate() runs it on the no-OS API */
static int set_bb_rate(struct ad9361_rf_phy *phy, const struct pluto_rate_plan *pl)
{
  AD9361_RXFIRConfig rx;
  AD9361_TXFIRConfig tx;
  memset(&rx, 0, sizeof(rx));
  memset(&tx, 0, sizeof(tx));
  rx.rx = 3;
  rx.rx_gain = -6;
  rx.rx_dec = pl->fir_dec;
  rx.rx_coef_size = (uint8_t)pl->fir_taps;
  memcpy(rx.rx_coef, pluto_fir_128_4, sizeof(pluto_fir_128_4));
  tx.tx = 3;
  tx.tx_gain = 0;
  tx.tx_int = pl->fir_dec;
  tx.tx_coef_size = (uint8_t)pl->fir_taps;
  memcpy(tx.tx_coef, pluto_fir_128_4, sizeof(pluto_fir_128_4));
  if (ad9361_set_rx_fir_config(phy, rx) < 0 || ad9361_set_tx_fir_config(phy, tx) < 0)
    return -1;
  uint32_t rxc[6], txc[6];
  ad9361_get_trx_path_clks(phy, rxc, txc);
  if (txc[5] == 0)
    return -2;
  if ((txc[1] / txc[5]) * 16u < pl->fir_taps && ad9361_set_rx_sampling_freq(phy, 3000000u) < 0)
    return -3;
  if (ad9361_set_trx_fir_en_dis(phy, 1) < 0)
    return -4;
  return ad9361_set_rx_sampling_freq(phy, pl->phy_hz) < 0 ? -5 : 0;
}

static void test_success_and_rate(void)
{
  AD9361_InitParam p;
  params(&p);
  reset_chip(CHIP_OK, 1);
  long before = live_allocs;
  struct ad9361_rf_phy *phy = NULL;
  int32_t r = ad9361_init(&phy, &p);
  CHECK(r == 0 && phy, "healthy model: ad9361_init returned %d", (int)r);
  if (!phy)
    return;
  CHECK(reset_level == 1, "RESETB released after init");

  struct pluto_rate_plan pl;
  CHECK(pluto_rate_plan(200000, &pl) == 0, "plan 200k");
  CHECK(set_bb_rate(phy, &pl) == 0, "set_bb_rate 1.6M");
  uint32_t rxr = 0, txr = 0;
  uint8_t rxe = 0, txe = 0;
  ad9361_get_rx_sampling_freq(phy, &rxr);
  ad9361_get_tx_sampling_freq(phy, &txr);
  ad9361_get_rx_fir_en_dis(phy, &rxe);
  ad9361_get_tx_fir_en_dis(phy, &txe);
  /* Linux ground truth on this board: in/out_voltage_sampling_frequency 1600000, FIR 128,4 on */
  CHECK(rxr >= 1599000 && rxr <= 1601000, "PHY RX rate %u", (unsigned)rxr);
  CHECK(txr >= 1599000 && txr <= 1601000, "PHY TX rate %u", (unsigned)txr);
  CHECK(rxe && txe, "FIR enabled RX %d TX %d", rxe, txe);
  uint32_t rxc[6], txc[6];
  ad9361_get_trx_path_clks(phy, rxc, txc);
  printf("200 kS/s: RX path BBPLL %u ADC %u R2 %u R1 %u CLKRF %u RXSAMP %u\n", (unsigned)rxc[0],
         (unsigned)rxc[1], (unsigned)rxc[2], (unsigned)rxc[3], (unsigned)rxc[4], (unsigned)rxc[5]);
  printf("200 kS/s: TX path BBPLL %u DAC %u T2 %u T1 %u CLKTF %u TXSAMP %u\n", (unsigned)txc[0],
         (unsigned)txc[1], (unsigned)txc[2], (unsigned)txc[3], (unsigned)txc[4], (unsigned)txc[5]);
  /* ad9361_tx_quad_calib handles clkrf == clktf and clkrf == 2 * clktf only */
  CHECK(rxc[4] == txc[4] || rxc[4] == 2 * txc[4], "CLKRF %u CLKTF %u: unhandled TX quad cal case",
        (unsigned)rxc[4], (unsigned)txc[4]);
  CHECK(rxc[4] / 4 - rxc[5] <= 1 && txc[4] / 4 - txc[5] <= 1, "FIR /4 between CLKRF/CLKTF and the sample clocks");
  /* Linux tx_path_rates at 1.6 MS/s: BBPLL 819.2 MHz, DAC 51.2, T2 25.6, T1 12.8, TF 6.4 MHz */
  CHECK(rxc[0] / 1000 == 819199 && rxc[1] / 1000 == 51199 && txc[4] / 1000 == 6399, "clock chain values");
  CHECK(rxc[1] >= MIN_ADC_CLK && rxc[1] <= MAX_ADC_CLK, "ADC clock %u within limits", (unsigned)rxc[1]);
  CHECK(rxc[0] >= MIN_BBPLL_FREQ && rxc[0] <= MAX_BBPLL_FREQ, "BBPLL %u within limits", (unsigned)rxc[0]);
  CHECK((txc[1] / txc[5]) * 16u >= 128u, "TX FIR 128 taps fit (DAC/TXSAMP*16 = %u)",
        (unsigned)((txc[1] / txc[5]) * 16u));
  /* the driver's verification: plain register reads only (pluto_fir_decode), ENSM back in FDD */
  struct pluto_fir_regs fr = {chip_read(PLUTO_REG_RX_FILTER_CONFIG), chip_read(PLUTO_REG_RX_FILTER_GAIN),
                              chip_read(PLUTO_REG_TX_FILTER_CONF), chip_read(PLUTO_REG_RX_ENABLE_FILTER_CTRL),
                              chip_read(PLUTO_REG_TX_ENABLE_FILTER_CTRL)};
  struct pluto_fir_state fs;
  pluto_fir_decode(&fr, &fs);
  int fbad = pluto_fir_check(&fs, 128, 4, -6, 0);
  CHECK(fbad == 0, "FIR registers after the rate change: check 0x%x (RX %u/%u/%d sel %u, TX %u/%u/%d sel %u)",
        (unsigned)fbad, (unsigned)fs.rx_taps, (unsigned)fs.rx_dec, fs.rx_gain_db, fs.rx_select,
        (unsigned)fs.tx_taps, (unsigned)fs.tx_int, fs.tx_gain_db, fs.tx_select);
  CHECK((chip_read(REG_STATE) & 0xf) == ENSM_STATE_FDD, "ENSM FDD after the rate change (0x%x)",
        (unsigned)chip_read(REG_STATE));
  /* why the driver never calls ad9361_get_rx/tx_fir_config: they write REG_*_FILTER_CONFIG with
   * FIR_START_CLK without forcing ALERT (ENSM stays FDD) and leave FIR_SELECT at 1 */
  CHECK(start_clk_in_fdd == 0, "init + FIR load + rate never set START_CLK in FDD (%u)", (unsigned)start_clk_in_fdd);
  uint32_t writes_before = filter_conf_writes;
  AD9361_RXFIRConfig rf;
  memset(&rf, 0, sizeof(rf));
  ad9361_get_rx_fir_config(phy, 0, &rf);
  CHECK(filter_conf_writes > writes_before && start_clk_in_fdd > 0,
        "no-OS FIR readback sets START_CLK while the ENSM is FDD (%u writes)", (unsigned)start_clk_in_fdd);
  fr.rx_conf = chip_read(PLUTO_REG_RX_FILTER_CONFIG);
  pluto_fir_decode(&fr, &fs);
  CHECK(fs.rx_select == 1, "no-OS FIR readback leaves RX FIR_SELECT %u (loaded 3)", fs.rx_select);

  /* TX attenuation 0 at SoapySDR TX gain 89 (hardwaregain = gain - 89) */
  CHECK(ad9361_set_tx_attenuation(phy, 0, 0) == 0, "TX attenuation 0");
  uint32_t att = 1;
  ad9361_get_tx_attenuation(phy, 0, &att);
  CHECK(att == 0, "TX attenuation readback %u mdB", (unsigned)att);
  /* RX manual 16 dB */
  CHECK(ad9361_set_rx_gain_control_mode(phy, 0, RF_GAIN_MGC) == 0, "MGC");
  CHECK(ad9361_set_rx_rf_gain(phy, 0, 16) == 0, "RX gain 16");
  /* (the gain index readback is not modelled) */

  /* rebuilding (what pluto_hw does on a suspect phy) frees everything */
  ad9361_remove(phy);
  CHECK(live_allocs == before, "init + remove leaked %ld allocation(s)", live_allocs - before);
}

int main(void)
{
  test_failure("dead SPI (PRODUCT_ID 0xff)", CHIP_DEAD, 1);
  test_failure("TX synth CP calibration timeout", CHIP_NO_CP_CAL, 1);
  test_failure("ADC core without DATA_CLK (Status errors)", CHIP_OK, 0);
  test_success_and_rate();
  /* a retry loop: every failure again, nothing accumulates */
  long before = live_allocs;
  for (int i = 0; i < 20; ++i)
    test_failure("retry", (enum chip_mode)(i % 2), 1);
  CHECK(live_allocs == before, "20 failed inits leaked %ld allocation(s)", live_allocs - before);
  printf("test_noos_init: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
