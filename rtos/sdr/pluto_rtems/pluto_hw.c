/*
 * AD9363A bring-up and control on the Pluto Nano with the ADI no-OS ad9361 driver.
 *
 * Init parameters: board/nano/dts/nano.dtsi (ad9361-phy@0) with the Linux ad9361 driver defaults
 * for every property the DT leaves out (the no-OS defaults in projects/ad9361/src/main.c are the
 * same values), 1R1T, CMOS full port, reference clock = U-Boot xo_correction.
 * Sample rate rules: SoapyPlutoSDR setSampleRate() + libad9361 ad9361_set_bb_rate().
 * External RX band control: the DT adi_rx_band_setting_* table (band-ctl GPIOs 81..83), applied
 * on every RX LO change like the Linux ad9361_ext_band_ctl.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "pluto_hw.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ad9361_api.h"
#include "ad9361.h"
#include "axi_adc_core.h"
#include "axi_dac_core.h"
#include "platform_rtems.h"
#include "pluto_fir.h"
#include "pluto_framing.h"
#include "pluto_regs.h"

static struct ad9361_rf_phy *phy;
static uint32_t cur_bb_rate;    /* AD9363 baseband rate last programmed */
static int cur_fpga_fir = -1;   /* PL x8 FIR on/off */
static int cur_band = -1;

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

static struct axi_adc_init rx_adc_init = {
  .name = "cf-ad9361-lpc",
  .base = PLUTO_ADC_BASE,
  .num_channels = 2, /* 1R1T: I0, Q0 */
  .slave_base = 0,
  .num_slave_channels = 0,
};

static struct axi_dac_init tx_dac_init = {
  .name = "cf-ad9361-dds-core-lpc",
  .base = PLUTO_DAC_BASE,
  .num_channels = 2,
  .channels = NULL,
  .rate = 1,
  .bus_type = 0,
};

static void fill_init_param(AD9361_InitParam *p, uint32_t refclk_hz)
{
  static const uint32_t rx_clks[6] = {983040000, 245760000, 122880000, 61440000, 30720000, 30720000};
  static const uint32_t tx_clks[6] = {983040000, 122880000, 122880000, 61440000, 30720000, 30720000};
  memset(p, 0, sizeof(*p));
  p->dev_sel = ID_AD9363A;
  p->reference_clk_rate = refclk_hz;
  /* Base configuration */
  p->two_rx_two_tx_mode_enable = 0;
  p->one_rx_one_tx_mode_use_rx_num = 1;
  p->one_rx_one_tx_mode_use_tx_num = 1;
  p->frequency_division_duplex_mode_enable = 1;
  p->dc_offset_tracking_update_event_mask = 5;
  p->dc_offset_attenuation_high_range = 6;
  p->dc_offset_attenuation_low_range = 5;
  p->dc_offset_count_high_range = 0x28;
  p->dc_offset_count_low_range = 0x32;
  p->trx_synthesizer_target_fref_overwrite_hz = MAX_SYNTH_FREF;
  /* LO */
  p->rx_synthesizer_frequency_hz = 2400000000ull;
  p->tx_synthesizer_frequency_hz = 2450000000ull;
  p->tx_lo_powerdown_managed_enable = 1;
  /* Rate & BW */
  memcpy(p->rx_path_clock_frequencies, rx_clks, sizeof(rx_clks));
  memcpy(p->tx_path_clock_frequencies, tx_clks, sizeof(tx_clks));
  p->rf_rx_bandwidth_hz = 18000000;
  p->rf_tx_bandwidth_hz = 18000000;
  /* RF ports: RX A balanced, TX A */
  p->rx_rf_port_input_select = 0;
  p->tx_rf_port_input_select = 0;
  p->tx_attenuation_mdB = 10000;
  /* Reference clock: external 40 MHz (board-trimmed value from xo_correction) */
  p->xo_disable_use_ext_refclk_enable = 1;
  p->dcxo_coarse_and_fine_tune[0] = 8;
  p->dcxo_coarse_and_fine_tune[1] = 5920;
  p->clk_output_mode_select = CLKOUT_DISABLE;
  /* Gain control (DT) */
  p->gc_rx1_mode = 2;
  p->gc_rx2_mode = 2;
  p->gc_adc_large_overload_thresh = 58;
  p->gc_adc_ovr_sample_size = 4;
  p->gc_adc_small_overload_thresh = 47;
  p->gc_dec_pow_measurement_duration = 8192;
  p->gc_lmt_overload_high_thresh = 800;
  p->gc_lmt_overload_low_thresh = 704;
  p->gc_low_power_thresh = 24;
  p->gc_max_dig_gain = 15;
  p->mgc_dec_gain_step = 2;
  p->mgc_inc_gain_step = 2;
  p->agc_adc_large_overload_exceed_counter = 10;
  p->agc_adc_large_overload_inc_steps = 2;
  p->agc_adc_small_overload_exceed_counter = 10;
  p->agc_dig_gain_step_size = 4;
  p->agc_dig_saturation_exceed_counter = 3;
  p->agc_gain_update_interval_us = 1000;
  p->agc_inner_thresh_high = 10;
  p->agc_inner_thresh_high_dec_steps = 1;
  p->agc_inner_thresh_low = 12;
  p->agc_inner_thresh_low_inc_steps = 1;
  p->agc_lmt_overload_large_exceed_counter = 10;
  p->agc_lmt_overload_large_inc_steps = 2;
  p->agc_lmt_overload_small_exceed_counter = 10;
  p->agc_outer_thresh_high = 5;
  p->agc_outer_thresh_high_dec_steps = 2;
  p->agc_outer_thresh_low = 18;
  p->agc_outer_thresh_low_inc_steps = 2;
  p->agc_attack_delay_extra_margin_us = 1;
  /* Fast AGC (DT) */
  p->fagc_dec_pow_measuremnt_duration = 64;
  p->fagc_state_wait_time_ns = 260;
  p->fagc_lp_thresh_increment_time = 5;
  p->fagc_lp_thresh_increment_steps = 1;
  p->fagc_lock_level_lmt_gain_increase_en = 1;
  p->fagc_lock_level_gain_increase_upper_limit = 5;
  p->fagc_lpf_final_settling_steps = 1;
  p->fagc_lmt_final_settling_steps = 1;
  p->fagc_final_overrange_count = 3;
  p->fagc_use_last_lock_level_for_set_gain_en = 1;
  p->fagc_rst_gla_stronger_sig_thresh_exceeded_en = 1;
  p->fagc_optimized_gain_offset = 5;
  p->fagc_rst_gla_stronger_sig_thresh_above_ll = 10;
  p->fagc_rst_gla_engergy_lost_sig_thresh_exceeded_en = 1;
  p->fagc_rst_gla_engergy_lost_goto_optim_gain_en = 1;
  p->fagc_rst_gla_engergy_lost_sig_thresh_below_ll = 10;
  p->fagc_energy_lost_stronger_sig_gain_lock_exit_cnt = 8;
  p->fagc_rst_gla_large_adc_overload_en = 1;
  p->fagc_rst_gla_large_lmt_overload_en = 1;
  p->fagc_power_measurement_duration_in_state5 = 64;
  p->fagc_large_overload_inc_steps = 2;
  /* RSSI (DT) */
  p->rssi_delay = 1;
  p->rssi_duration = 1000;
  p->rssi_restart_mode = 3;
  p->rssi_wait = 1;
  /* Aux ADC / DAC / temperature (DT + defaults) */
  p->aux_adc_decimation = 256;
  p->aux_adc_rate = 40000000;
  p->aux_dac_manual_mode_enable = 1;
  p->temp_sense_decimation = 256;
  p->temp_sense_measurement_interval_ms = 1000;
  p->temp_sense_offset_signed = (int8_t)0xb6;
  p->temp_sense_periodic_measurement_enable = 1;
  /* Control outputs (DT) */
  p->ctrl_outs_enable_mask = 0xff;
  p->ctrl_outs_index = 1;
  /* Digital interface (DT: CMOS full port, swaps, frame pulse, TX data delay 9) */
  p->digital_interface_tune_skip_mode = 0;
  p->digital_interface_tune_fir_disable = 0;
  p->pp_tx_swap_enable = 1;
  p->pp_rx_swap_enable = 1;
  p->rx_frame_pulse_mode_enable = 1;
  p->swap_ports_enable = 1;
  p->lvds_mode_enable = 0;
  p->full_port_enable = 1;
  p->rx_data_clock_delay = 0;
  p->rx_data_delay = 0;
  p->tx_fb_clock_delay = 0;
  p->tx_data_delay = 9;
  p->lvds_bias_mV = 150;
  p->lvds_rx_onchip_termination_enable = 0;
  p->lvds_invert1_control = 0xff;
  p->lvds_invert2_control = 0x0f;
  /* TX monitor defaults */
  p->low_high_gain_threshold_mdB = 37000;
  p->low_gain_dB = 0;
  p->high_gain_dB = 24;
  p->tx_mon_delay = 511;
  p->tx_mon_duration = 8192;
  p->tx1_mon_front_end_gain = 2;
  p->tx2_mon_front_end_gain = 2;
  p->tx1_mon_lo_cm = 48;
  p->tx2_mon_lo_cm = 48;
  /* GPIOs: reset-gpios = gpio0 67; no MCS / cal switches */
  p->gpio_resetb.number = PLUTO_GPIO_RESETB;
  p->gpio_resetb.platform_ops = &pluto_zynq_gpio_ops;
  p->gpio_sync.number = -1;
  p->gpio_sync.platform_ops = &pluto_zynq_gpio_ops;
  p->gpio_cal_sw1.number = -1;
  p->gpio_cal_sw1.platform_ops = &pluto_zynq_gpio_ops;
  p->gpio_cal_sw2.number = -1;
  p->gpio_cal_sw2.platform_ops = &pluto_zynq_gpio_ops;
  /* SPI: spi0, CS0, spi-cpha, 10 MHz */
  p->spi_param.device_id = 0;
  p->spi_param.max_speed_hz = PLUTO_SPI_MAX_HZ;
  p->spi_param.chip_select = PLUTO_SPI_CS;
  p->spi_param.mode = NO_OS_SPI_MODE_1;
  p->spi_param.platform_ops = &pluto_zynq_spi_ops;
  p->rx_adc_init = &rx_adc_init;
  p->tx_dac_init = &tx_dac_init;
}

/* DT adi_rx_band_setting_0..6: LO range -> band-ctl-0/1/2 */
static const struct {
  uint64_t max_hz;
  uint8_t ctl[3];
} rx_bands[] = {
  {30000000ull, {0, 0, 0}},   {100000000ull, {0, 0, 1}},  {200000000ull, {0, 1, 0}},
  {500000000ull, {0, 1, 1}},  {1500000000ull, {1, 0, 0}}, {2400000000ull, {1, 0, 1}},
  {6000000000ull, {1, 1, 0}},
};

static void apply_rx_band(uint64_t lo)
{
  int band = (int)(sizeof(rx_bands) / sizeof(rx_bands[0])) - 1;
  for (int i = 0; i < (int)(sizeof(rx_bands) / sizeof(rx_bands[0])); ++i)
    if (lo <= rx_bands[i].max_hz) {
      band = i;
      break;
    }
  if (band == cur_band)
    return;
  zynq_gpio_output(PLUTO_GPIO_BAND_CTL0, rx_bands[band].ctl[0]);
  zynq_gpio_output(PLUTO_GPIO_BAND_CTL1, rx_bands[band].ctl[1]);
  zynq_gpio_output(PLUTO_GPIO_BAND_CTL2, rx_bands[band].ctl[2]);
  cur_band = band;
  /* band-ctl-post: RX port A balanced */
  ad9361_set_rx_rf_port_input(phy, 0);
}

static void setup_cores(void)
{
  /* RX: only I0/Q0 into the packer */
  for (unsigned c = 2; c < 4; ++c)
    REG32(PLUTO_ADC_BASE + PLUTO_REG_ADC_CHAN_CNTRL(c)) &= ~1u;
  /* TX: I0/Q0 from the DMA (this also enables them in the unpacker), I1/Q1 zero */
  for (unsigned c = 0; c < 4; ++c)
    REG32(PLUTO_DAC_BASE + PLUTO_REG_DAC_CHAN_CNTRL_7(c)) = c < 2 ? PLUTO_DAC_SEL_DMA : PLUTO_DAC_SEL_ZERO;
  REG32(PLUTO_DAC_BASE + PLUTO_REG_DAC_SYNC) = 1;
  /* timestamping off and FIRs bypassed until a stream configures them */
  REG32(PLUTO_ADC_BASE + PLUTO_REG_GPIO_OUT) = 0;
  REG32(PLUTO_DAC_BASE + PLUTO_REG_GPIO_OUT) = 0;
}

int pluto_hw_is_open(void)
{
  return phy != NULL;
}

int pluto_hw_open(uint32_t refclk_hz)
{
  if (phy)
    return 0;
  AD9361_InitParam p;
  fill_init_param(&p, refclk_hz);
  printf("pluto: AD9363A init, reference %u Hz, 1R1T CMOS\n", (unsigned)refclk_hz);
  /* en_agc low (manual gain control by SPI), band control at the power-up LO */
  zynq_gpio_output(PLUTO_GPIO_EN_AGC, 0);
  int32_t ret = ad9361_init(&phy, &p);
  if (ret < 0 || !phy) {
    printf("pluto: ad9361_init failed (%d)\n", (int)ret);
    phy = NULL;
    return ret < 0 ? (int)ret : -ENODEV;
  }
  setup_cores();
  cur_bb_rate = 0;
  cur_fpga_fir = 0;
  cur_band = -1;
  apply_rx_band(p.rx_synthesizer_frequency_hz);
  ad9361_set_rx_gain_control_mode(phy, 0, RF_GAIN_MGC);
  int32_t t = 0;
  ad9361_get_temperature(phy, &t);
  printf("pluto: AD9363A ready, %d.%d C\n", (int)(t / 1000), (int)((t < 0 ? -t : t) % 1000 / 100));
  return 0;
}

void pluto_hw_close(void)
{
  /* the transceiver stays configured for the next open (like the Linux driver) */
  if (!phy)
    return;
  REG32(PLUTO_ADC_BASE + PLUTO_REG_GPIO_OUT) &= 1u;
  REG32(PLUTO_DAC_BASE + PLUTO_REG_GPIO_OUT) &= 1u;
}

/* ---- sample rate (libad9361 ad9361_set_bb_rate on the no-OS API) ---- */

static int load_fir(uint32_t dec, const int16_t *taps, uint32_t ntaps)
{
  AD9361_RXFIRConfig rx;
  AD9361_TXFIRConfig tx;
  memset(&rx, 0, sizeof(rx));
  memset(&tx, 0, sizeof(tx));
  rx.rx = 3;
  rx.rx_gain = -6;
  rx.rx_dec = dec;
  rx.rx_coef_size = (uint8_t)ntaps;
  memcpy(rx.rx_coef, taps, ntaps * sizeof(int16_t));
  tx.tx = 3;
  tx.tx_gain = 0;
  tx.tx_int = dec;
  tx.tx_coef_size = (uint8_t)ntaps;
  memcpy(tx.tx_coef, taps, ntaps * sizeof(int16_t));
  int32_t ret = ad9361_set_rx_fir_config(phy, rx);
  if (ret >= 0)
    ret = ad9361_set_tx_fir_config(phy, tx);
  return (int)ret;
}

static int set_bb_rate(uint32_t rate)
{
  uint32_t dec, ntaps;
  const int16_t *fir;
  if (rate <= 20000000u) {
    dec = 4; fir = pluto_fir_128_4; ntaps = 128;
  } else if (rate <= 40000000u) {
    dec = 2; fir = pluto_fir_128_2; ntaps = 128;
  } else if (rate <= 53333333u) {
    dec = 2; fir = pluto_fir_96_2; ntaps = 96;
  } else {
    dec = 2; fir = pluto_fir_64_2; ntaps = 64;
  }
  uint32_t current = 0;
  uint8_t enabled = 0;
  int32_t ret;
  ad9361_get_rx_sampling_freq(phy, &current);
  ad9361_get_rx_fir_en_dis(phy, &enabled);
  if (enabled) {
    if (current <= 25000000u / 12u)
      ad9361_set_rx_sampling_freq(phy, 3000000u);
    ret = ad9361_set_trx_fir_en_dis(phy, 0);
    if (ret < 0)
      return (int)ret;
  }
  ret = load_fir(dec, fir, ntaps);
  if (ret < 0)
    return (int)ret;
  if (rate <= 25000000u / 12u) {
    uint32_t rxc[6], txc[6];
    ad9361_get_trx_path_clks(phy, rxc, txc);
    /* TX FIR length limit: DAC clock / TX sample rate * 16 taps */
    if (txc[5] == 0)
      return -EINVAL;
    if ((txc[1] / txc[5]) * 16u < ntaps)
      ad9361_set_rx_sampling_freq(phy, 3000000u);
    ret = ad9361_set_trx_fir_en_dis(phy, 1);
    if (ret >= 0)
      ret = ad9361_set_rx_sampling_freq(phy, rate);
  } else {
    ret = ad9361_set_rx_sampling_freq(phy, rate);
    if (ret >= 0)
      ret = ad9361_set_trx_fir_en_dis(phy, 1);
  }
  return (int)ret;
}

int pluto_hw_set_rate(uint32_t rate_hz)
{
  if (!phy)
    return -ENODEV;
  /* SoapyPlutoSDR with libad9361 (fir = 4): below 25 MHz / 48 the PL x8 FIR is used */
  int fpga = (double)rate_hz < 25e6 / 48.0;
  uint64_t bb = fpga ? (uint64_t)rate_hz * 8u : rate_hz;
  if (fpga && (double)bb < 25e6 / 48.0)
    printf("pluto: sample rate %u is below the supported minimum\n", (unsigned)rate_hz);
  if (bb > 61440000u || bb == 0)
    return -EINVAL;
  if ((uint32_t)bb != cur_bb_rate) {
    int ret = set_bb_rate((uint32_t)bb);
    if (ret < 0) {
      printf("pluto: baseband rate %u failed (%d)\n", (unsigned)bb, ret);
      cur_bb_rate = 0;
      return ret;
    }
    cur_bb_rate = (uint32_t)bb;
  }
  if (fpga != cur_fpga_fir) {
    uint32_t a = REG32(PLUTO_ADC_BASE + PLUTO_REG_GPIO_OUT);
    uint32_t d = REG32(PLUTO_DAC_BASE + PLUTO_REG_GPIO_OUT);
    REG32(PLUTO_ADC_BASE + PLUTO_REG_GPIO_OUT) = pluto_gpio_with_fir(a, fpga);
    REG32(PLUTO_DAC_BASE + PLUTO_REG_GPIO_OUT) = pluto_gpio_with_fir(d, fpga);
    cur_fpga_fir = fpga;
  }
  return 0;
}

int pluto_hw_fpga_fir(void)
{
  return cur_fpga_fir > 0;
}

double pluto_hw_get_rate(int tx)
{
  if (!phy)
    return 0;
  uint32_t r = 0;
  if (tx)
    ad9361_get_tx_sampling_freq(phy, &r);
  else
    ad9361_get_rx_sampling_freq(phy, &r);
  return cur_fpga_fir > 0 ? (double)r / 8.0 : (double)r;
}

/* ---- LO, bandwidth, gain, ports ---- */

int pluto_hw_set_lo(int tx, uint64_t hz)
{
  if (!phy)
    return -ENODEV;
  if (hz < AD9363A_MIN_CARRIER_FREQ_HZ || hz > AD9363A_MAX_CARRIER_FREQ_HZ)
    return -EINVAL;
  int32_t ret = tx ? ad9361_set_tx_lo_freq(phy, hz) : ad9361_set_rx_lo_freq(phy, hz);
  if (ret >= 0 && !tx)
    apply_rx_band(hz);
  return (int)ret;
}

uint64_t pluto_hw_get_lo(int tx)
{
  uint64_t hz = 0;
  if (!phy)
    return 0;
  if (tx)
    ad9361_get_tx_lo_freq(phy, &hz);
  else
    ad9361_get_rx_lo_freq(phy, &hz);
  return hz;
}

int pluto_hw_lo_power(int tx, int on)
{
  if (!phy)
    return -ENODEV;
  return (int)(tx ? ad9361_tx_lo_powerdown(phy, on ? ON : OFF)
                  : ad9361_rx_lo_powerdown(phy, on ? ON : OFF));
}

int pluto_hw_set_bw(int tx, uint32_t hz)
{
  if (!phy)
    return -ENODEV;
  return (int)(tx ? ad9361_set_tx_rf_bandwidth(phy, hz) : ad9361_set_rx_rf_bandwidth(phy, hz));
}

uint32_t pluto_hw_get_bw(int tx)
{
  uint32_t hz = 0;
  if (!phy)
    return 0;
  if (tx)
    ad9361_get_tx_rf_bandwidth(phy, &hz);
  else
    ad9361_get_rx_rf_bandwidth(phy, &hz);
  return hz;
}

int pluto_hw_set_rx_agc(int automatic)
{
  if (!phy)
    return -ENODEV;
  return (int)ad9361_set_rx_gain_control_mode(phy, 0,
                                              automatic ? RF_GAIN_SLOWATTACK_AGC : RF_GAIN_MGC);
}

int pluto_hw_set_rx_gain(int32_t db)
{
  if (!phy)
    return -ENODEV;
  return (int)ad9361_set_rx_rf_gain(phy, 0, db);
}

int32_t pluto_hw_get_rx_gain(void)
{
  int32_t db = 0;
  if (phy)
    ad9361_get_rx_rf_gain(phy, 0, &db);
  return db;
}

int pluto_hw_set_tx_atten_mdb(uint32_t mdb)
{
  if (!phy)
    return -ENODEV;
  return (int)ad9361_set_tx_attenuation(phy, 0, mdb);
}

uint32_t pluto_hw_get_tx_atten_mdb(void)
{
  uint32_t mdb = 0;
  if (phy)
    ad9361_get_tx_attenuation(phy, 0, &mdb);
  return mdb;
}

int pluto_hw_set_rx_port(uint32_t port)
{
  return phy ? (int)ad9361_set_rx_rf_port_input(phy, port) : -ENODEV;
}

int pluto_hw_set_tx_port(uint32_t port)
{
  return phy ? (int)ad9361_set_tx_rf_port_output(phy, port) : -ENODEV;
}

int pluto_hw_set_loopback(int mode)
{
  return phy ? (int)ad9361_bist_loopback(phy, mode) : -ENODEV;
}

int pluto_hw_temp_mdeg(int32_t *mdeg)
{
  return phy ? (int)ad9361_get_temperature(phy, mdeg) : -ENODEV;
}

/* ---- timestamping core registers ---- */

void pluto_hw_set_timestamp_words(int tx, uint32_t words)
{
  uintptr_t reg = (tx ? PLUTO_DAC_BASE : PLUTO_ADC_BASE) + PLUTO_REG_GPIO_OUT;
  REG32(reg) = pluto_gpio_with_every(REG32(reg), words);
}

uint32_t pluto_hw_tx_discarded(void)
{
  return REG32(PLUTO_DAC_BASE + PLUTO_REG_GPIO_IN);
}

uint32_t pluto_hw_dma_status_clear(int tx)
{
  uintptr_t reg = (tx ? PLUTO_DAC_BASE : PLUTO_ADC_BASE) + PLUTO_REG_DMA_STATUS;
  uint32_t s = REG32(reg);
  REG32(reg) = s; /* sticky, write 1 to clear */
  return s;
}
