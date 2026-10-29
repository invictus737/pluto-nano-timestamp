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
 * Robustness (first board runs: PRODUCT_ID 0xff/0x00, calibration timeouts, a data abort when the
 * AXI ADC core never saw DATA_CLK):
 *  - one global lock around every transceiver access: SoapySDR 0.8 constructs devices outside its
 *    factory lock, so two opens (e.g. the station and a second make) may run in parallel and
 *    would otherwise interleave resets and SPI transfers on the one AD9363;
 *  - before ad9361_init: PL configured, axi_ad9361 core present, then a reset pulse and a bounded,
 *    repeated PRODUCT_ID check (pluto_probe.c) - nothing is touched on a dead bus;
 *  - after ad9361_init: BBPLL and both RF synthesizers locked, ENSM in FDD, ADC core status ok,
 *    otherwise the phy is removed and the open fails (the caller retries);
 *  - every failure leaves phy == NULL or a fully working phy; a phy that failed a later
 *    operation is marked suspect and rebuilt from reset at the next open with no other user.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "pluto_hw.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "ad9361_api.h"
#include "ad9361.h"
#include "axi_adc_core.h"
#include "axi_dac_core.h"
#include "no_os_delay.h"
#include "platform_rtems.h"
#include "pluto_fir.h"
#include "pluto_framing.h"
#include "pluto_probe.h"
#include "pluto_regs.h"

/* every phy access holds hw_lock; phy is only ever NULL or fully initialised. The lock uses
 * priority inheritance (RTEMS: OMIP on SMP): the low-priority level monitor takes it for the
 * 0x05E read, and a radio thread waiting behind it must lift its holder. */
static pthread_mutex_t hw_lock;
static pthread_once_t hw_lock_once = PTHREAD_ONCE_INIT;
static struct ad9361_rf_phy *phy;
static unsigned users;          /* open SoapySDR devices sharing phy */
static unsigned streams_active; /* active RX/TX streams over all devices (pluto_hw_stream_active) */
static int suspect;             /* an operation failed: rebuild at the next open without users */
static uint32_t cur_bb_rate;    /* AD9363 baseband rate last programmed */
static int cur_fpga_fir = -1;   /* PL x8 FIR on/off */
static int cur_band = -1;
static uint32_t tx_fir_scale = 1; /* TX FIR taps x1 (Linux table) or x2 (unity /4 passband) */

static void hw_lock_init(void)
{
  pthread_mutexattr_t a;
  int rc = pthread_mutexattr_init(&a);
  if (rc == 0) {
    rc = pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    if (rc == 0)
      rc = pthread_mutex_init(&hw_lock, &a);
    pthread_mutexattr_destroy(&a);
  }
  if (rc != 0) {
    printf("pluto: hw lock: priority inheritance refused (%d), plain mutex\n", rc);
    pthread_mutex_init(&hw_lock, NULL);
  } else {
    printf("pluto: hw lock: priority inheritance\n");
  }
}

static void hw_lock_acquire(void)
{
  pthread_once(&hw_lock_once, hw_lock_init);
  pthread_mutex_lock(&hw_lock);
}

#define LOCK() hw_lock_acquire()
#define UNLOCK() pthread_mutex_unlock(&hw_lock)

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

/* ---- presence checks ---- */

#define DEVCFG_INT_STS 0xf800700cu
#define DEVCFG_PCFG_DONE (1u << 2)

/* PL configured, level shifters on, FCLK0 out of reset, axi_ad9361 answering: before this the
 * EMIO SPI/GPIO lines do not reach the AD9363 and a PL register read can hang the AXI bus */
static int pl_ready(void)
{
  uint32_t sts = REG32(DEVCFG_INT_STS);
  uint32_t lvl = REG32(SLCR_LVL_SHFTR_EN);
  uint32_t rst = REG32(SLCR_FPGA_RST_CTRL);
  if (!(sts & DEVCFG_PCFG_DONE) || (lvl & 0xfu) != 0xfu || (rst & 1u)) {
    printf("pluto: PL not ready (devcfg int_sts %08x, lvl_shftr %x, fpga_rst %x)\n", (unsigned)sts,
           (unsigned)lvl, (unsigned)rst);
    return -EAGAIN;
  }
  uint32_t ver = REG32(PLUTO_ADC_BASE + 0x0000u);
  if (ver == 0 || ver == 0xffffffffu) {
    printf("pluto: axi_ad9361 core missing (version %08x)\n", (unsigned)ver);
    return -ENODEV;
  }
  return 0;
}

struct probe_ctx {
  struct no_os_spi_desc *spi;
};

static int probe_spi_read(void *ctx, uint16_t reg)
{
  return (int)ad9361_spi_read(((struct probe_ctx *)ctx)->spi, reg);
}

static void probe_reset(void *ctx, int level)
{
  (void)ctx;
  zynq_gpio_output(PLUTO_GPIO_RESETB, level);
}

static void probe_sleep(void *ctx, uint32_t us)
{
  (void)ctx;
  no_os_udelay(us);
}

static void spi_diag(void)
{
  printf("pluto: diag spi0 cr %08x isr %08x en %x, slcr spi_clk %08x spi_rst %08x, gpio dirm2 %08x "
         "oen2 %08x data2 %08x\n",
         (unsigned)REG32(PLUTO_SPI_BASE + 0x00u), (unsigned)REG32(PLUTO_SPI_BASE + 0x04u),
         (unsigned)REG32(PLUTO_SPI_BASE + 0x14u), (unsigned)REG32(SLCR_SPI_CLK_CTRL),
         (unsigned)REG32(SLCR_SPI_RST_CTRL), (unsigned)REG32(PLUTO_GPIO_BASE + 0x284u),
         (unsigned)REG32(PLUTO_GPIO_BASE + 0x288u), (unsigned)REG32(PLUTO_GPIO_BASE + 0x068u));
}

/* reset pulse + PRODUCT_ID until it reads back right several times in a row */
static int probe_transceiver(const AD9361_InitParam *p)
{
  struct probe_ctx c = {NULL};
  int32_t ret = no_os_spi_init(&c.spi, &p->spi_param);
  if (ret < 0 || !c.spi) {
    printf("pluto: SPI init failed (%d)\n", (int)ret);
    return ret < 0 ? (int)ret : -ENODEV;
  }
  struct pluto_probe_ops ops = {&c, probe_spi_read, probe_reset, probe_sleep};
  struct pluto_probe_cfg cfg;
  struct pluto_probe_result res;
  pluto_probe_default_cfg(&cfg);
  int r = pluto_probe_ad9363(&ops, &cfg, &res);
  if (r == 0)
    printf("pluto: AD9363 PRODUCT_ID 0x%02x after %u reset pulse(s), %u ms\n", (unsigned)res.id,
           (unsigned)res.attempts, (unsigned)(res.waited_us / 1000u));
  else {
    printf("pluto: AD9363 not answering: PRODUCT_ID 0x%x (first bad 0x%x) after %u reset pulses, "
           "%u ms\n", (unsigned)res.id, (unsigned)res.first_bad, (unsigned)res.attempts,
           (unsigned)(res.waited_us / 1000u));
    spi_diag();
  }
  no_os_spi_remove(c.spi);
  return r;
}

/*
 * Transceiver state: 0 healthy. Always required: PRODUCT_ID answers, BBPLL locked, the axi_ad9361
 * core sees DATA_CLK; right after init also ENSM in FDD. The RF synthesizer locks are reported
 * only: SoapySDR powers the LOs down at closeStream (as SoapyPlutoSDR does), so an unlocked VCO
 * on a reused phy is normal, and both are re-locked by the next LO tune.
 */
static int health_check(const char *when, int after_init)
{
  int id = (int)ad9361_spi_read(phy->spi, REG_PRODUCT_ID);
  int bb = (int)ad9361_spi_read(phy->spi, REG_CH_1_OVERFLOW);
  int rx = (int)ad9361_spi_read(phy->spi, REG_RX_CP_OVERRANGE_VCO_LOCK);
  int tx = (int)ad9361_spi_read(phy->spi, REG_TX_CP_OVERRANGE_VCO_LOCK);
  int st = (int)ad9361_spi_read(phy->spi, REG_STATE);
  uint32_t adc = REG32(PLUTO_ADC_BASE + AXI_ADC_REG_STATUS);
  int ok = pluto_probe_id_ok(id) && bb >= 0 && (bb & BBPLL_LOCK) && (adc & AXI_ADC_STATUS) &&
           (!after_init || (st >= 0 && (st & 0xf) == ENSM_STATE_FDD));
  printf("pluto: AD9363 %s %s: id 0x%x bbpll %s rx-vco %s tx-vco %s ensm 0x%x adc-core %s\n",
         ok ? "healthy" : "UNHEALTHY", when, (unsigned)id,
         (bb >= 0 && (bb & BBPLL_LOCK)) ? "locked" : "UNLOCKED",
         (rx >= 0 && (rx & VCO_LOCK)) ? "locked" : "unlocked",
         (tx >= 0 && (tx & VCO_LOCK)) ? "locked" : "unlocked", (unsigned)(st & 0xf),
         (adc & AXI_ADC_STATUS) ? "ok" : "NO DATA_CLK");
  return ok ? 0 : -EIO;
}

/*
 * The AD9363 runs full duplex (ENSM FDD, 0xA) whenever it is configured: every no-OS operation that
 * forces ALERT (FIR load, rate, bandwidth/calibrations) must restore it. Checked and logged after
 * init and after every rate/bandwidth/FIR change; a state left elsewhere is forced back to FDD once,
 * and the phy is marked suspect when that fails. Caller holds hw_lock, phy != NULL.
 */
static int ensm_assert_fdd(const char *when)
{
  struct probe_ctx c = {phy->spi};
  struct pluto_probe_ops ops = {&c, probe_spi_read, probe_reset, probe_sleep};
  int st = -1;
  if (pluto_ensm_wait(&ops, PLUTO_ENSM_FDD, 20, 100, &st) == 0) {
    printf("pluto: ENSM FDD (0x%x) %s\n", (unsigned)st, when);
    return 0;
  }
  printf("pluto: ENSM 0x%x %s, NOT FDD: forcing FDD\n", (unsigned)st, when);
  ad9361_ensm_force_state(phy, ENSM_STATE_FDD);
  if (pluto_ensm_wait(&ops, PLUTO_ENSM_FDD, 20, 100, &st) == 0) {
    printf("pluto: ENSM FDD (0x%x) %s, after forcing\n", (unsigned)st, when);
    return 0;
  }
  printf("pluto: ENSM 0x%x %s: FDD NOT reached, AD9363 marked suspect\n", (unsigned)st, when);
  suspect = 1;
  return -EIO;
}

static void teardown_locked(void)
{
  if (!phy)
    return;
  /* PL FIR and timestamps off, the AD9363 back in reset: a known state for the next probe */
  REG32(PLUTO_ADC_BASE + PLUTO_REG_GPIO_OUT) = 0;
  REG32(PLUTO_DAC_BASE + PLUTO_REG_GPIO_OUT) = 0;
  ad9361_remove(phy);
  phy = NULL;
  zynq_gpio_output(PLUTO_GPIO_RESETB, 0);
  cur_bb_rate = 0;
  cur_fpga_fir = -1;
  cur_band = -1;
  suspect = 0;
}

int pluto_hw_is_open(void)
{
  LOCK();
  int r = phy != NULL;
  UNLOCK();
  return r;
}

static int open_locked(uint32_t refclk_hz)
{
  if (phy && users == 0 && (suspect || health_check("at reopen", 0) < 0)) {
    printf("pluto: rebuilding the AD9363 state from reset\n");
    teardown_locked();
  }
  if (phy)
    return 0;
  int ret = pl_ready();
  if (ret < 0)
    return ret;
  AD9361_InitParam p;
  fill_init_param(&p, refclk_hz);
  printf("pluto: AD9363A init, reference %u Hz, 1R1T CMOS\n", (unsigned)refclk_hz);
  /* en_agc low (manual gain control by SPI), band control at the power-up LO */
  zynq_gpio_output(PLUTO_GPIO_EN_AGC, 0);
  /* two full attempts (probe + init) before the error goes to SoapySDR, which retries */
  struct ad9361_rf_phy *np = NULL;
  int32_t r = -ENODEV;
  for (int attempt = 1; attempt <= 2 && !np; ++attempt) {
    ret = probe_transceiver(&p);
    if (ret < 0) {
      r = ret;
      continue;
    }
    r = ad9361_init(&np, &p);
    if (r < 0 || !np) {
      /* the patched ad9361_init released everything (noos-patches/0001) */
      printf("pluto: ad9361_init failed (%d), attempt %d of 2\n", (int)r, attempt);
      np = NULL;
      zynq_gpio_output(PLUTO_GPIO_RESETB, 0);
    }
  }
  if (!np)
    return r < 0 ? (int)r : -ENODEV;
  phy = np;
  streams_active = 0;
  setup_cores();
  cur_bb_rate = 0;
  cur_fpga_fir = 0;
  cur_band = -1;
  apply_rx_band(p.rx_synthesizer_frequency_hz);
  if (ad9361_set_rx_gain_control_mode(phy, 0, RF_GAIN_MGC) < 0 || health_check("after init", 1) < 0 ||
      ensm_assert_fdd("after init") < 0) {
    teardown_locked();
    return -EIO;
  }
  int32_t t = 0;
  ad9361_get_temperature(phy, &t);
  printf("pluto: AD9363A ready, %d.%d C\n", (int)(t / 1000), (int)((t < 0 ? -t : t) % 1000 / 100));
  return 0;
}

int pluto_hw_open(uint32_t refclk_hz)
{
  LOCK();
  int ret = open_locked(refclk_hz);
  if (ret == 0)
    users++;
  UNLOCK();
  return ret;
}

void pluto_hw_close(void)
{
  /* the transceiver stays configured for the next open (like the Linux driver) */
  LOCK();
  if (users > 0)
    users--;
  if (phy && users == 0) {
    REG32(PLUTO_ADC_BASE + PLUTO_REG_GPIO_OUT) &= 1u;
    REG32(PLUTO_DAC_BASE + PLUTO_REG_GPIO_OUT) &= 1u;
  }
  UNLOCK();
}

/* ---- sample rate (libad9361 ad9361_set_bb_rate on the no-OS API) ---- */

/* libad9361 v0.3 ad9361_set_bb_rate: "RX 3 GAIN -6 DEC n" / "TX 3 GAIN 0 INT n", both channels */
#define FIR_RX_GAIN_DB (-6)
#define FIR_TX_GAIN_DB 0

/*
 * The FIR sets as loaded by load_fir (taps after tx_fir_scale, dec/int, gains): what the summaries
 * report. The coefficients are never read back from the AD9363: no-OS ad9361_get_rx/tx_fir_config
 * set FIR_START_CLK without forcing ALERT (ad9361_api.c) and leave FIR_SELECT at 1 instead of the 3
 * loaded, which stops the filter's data path while the ENSM is in FDD (the RX DMA froze mid-block
 * and nexus2 failed with StartupDeadline). Linux answers filter_fir_config from its cache too.
 * verify_rate checks the loaded state with plain register reads (pluto_fir_decode).
 */
static AD9361_RXFIRConfig fir_rx_seen;
static AD9361_TXFIRConfig fir_tx_seen;

static int load_fir(uint32_t dec, const int16_t *taps, uint32_t ntaps)
{
  AD9361_RXFIRConfig rx;
  AD9361_TXFIRConfig tx;
  memset(&rx, 0, sizeof(rx));
  memset(&tx, 0, sizeof(tx));
  rx.rx = 3;
  rx.rx_gain = FIR_RX_GAIN_DB;
  rx.rx_dec = dec;
  rx.rx_coef_size = (uint8_t)ntaps;
  memcpy(rx.rx_coef, taps, ntaps * sizeof(int16_t));
  tx.tx = 3;
  tx.tx_gain = FIR_TX_GAIN_DB;
  tx.tx_int = dec;
  tx.tx_coef_size = (uint8_t)ntaps;
  /* tx_fir_scale applies to the /4 table only: the /2 tables are already unity per phase */
  if (pluto_fir_scale_taps(taps, ntaps, dec == 4 ? tx_fir_scale : 1, tx.tx_coef) < 0)
    return -ERANGE;
  memset(&fir_rx_seen, 0, sizeof(fir_rx_seen));
  memset(&fir_tx_seen, 0, sizeof(fir_tx_seen));
  int32_t ret = ad9361_set_rx_fir_config(phy, rx);
  if (ret >= 0)
    ret = ad9361_set_tx_fir_config(phy, tx);
  if (ret >= 0) {
    fir_rx_seen = rx;
    fir_tx_seen = tx;
  }
  return (int)ret;
}

static const int16_t *fir_taps(uint32_t dec, uint32_t ntaps)
{
  if (dec == 4)
    return pluto_fir_128_4;
  if (ntaps == 128)
    return pluto_fir_128_2;
  return ntaps == 96 ? pluto_fir_96_2 : pluto_fir_64_2;
}

static int set_bb_rate(const struct pluto_rate_plan *pl)
{
  const uint32_t rate = pl->phy_hz, dec = pl->fir_dec, ntaps = pl->fir_taps;
  uint32_t current = 0;
  uint8_t enabled = 0;
  int32_t ret;
  ad9361_get_rx_sampling_freq(phy, &current);
  ad9361_get_rx_fir_en_dis(phy, &enabled);
  if (enabled) {
    /* below 25 MHz/12 there is no FIR-less clock chain: step to 3 MHz before the FIR goes off */
    if (current <= 25000000u / 12u) {
      ret = ad9361_set_rx_sampling_freq(phy, 3000000u);
      if (ret < 0)
        return (int)ret;
    }
    ret = ad9361_set_trx_fir_en_dis(phy, 0);
    if (ret < 0)
      return (int)ret;
  }
  ret = load_fir(dec, fir_taps(dec, ntaps), ntaps);
  if (ret < 0)
    return (int)ret;
  if (pl->via_fir_first) {
    uint32_t rxc[6], txc[6];
    ret = ad9361_get_trx_path_clks(phy, rxc, txc);
    if (ret < 0)
      return (int)ret;
    /* TX FIR length limit: DAC clock / TX sample rate * 16 taps */
    if (txc[5] == 0)
      return -EINVAL;
    if ((txc[1] / txc[5]) * 16u < ntaps) {
      ret = ad9361_set_rx_sampling_freq(phy, 3000000u);
      if (ret < 0)
        return (int)ret;
    }
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

/* FIR state from plain register reads (never the coefficient RAM, START_CLK untouched); -EIO on
 * an SPI error */
static int read_fir_regs(struct pluto_fir_state *s)
{
  static const uint16_t regs[5] = {PLUTO_REG_RX_FILTER_CONFIG, PLUTO_REG_RX_FILTER_GAIN,
                                   PLUTO_REG_TX_FILTER_CONF, PLUTO_REG_RX_ENABLE_FILTER_CTRL,
                                   PLUTO_REG_TX_ENABLE_FILTER_CTRL};
  uint8_t v[5];
  for (int i = 0; i < 5; ++i) {
    int r = (int)ad9361_spi_read(phy->spi, regs[i]);
    if (r < 0)
      return -EIO;
    v[i] = (uint8_t)r;
  }
  struct pluto_fir_regs fr = {v[0], v[1], v[2], v[3], v[4]};
  pluto_fir_decode(&fr, s);
  return 0;
}

/* readback against the plan: PHY RX/TX rate within 0.1 %, FIR on in both directions with the
 * loaded taps, decimation/interpolation and gains, both channels selected, coefficient clocks
 * stopped (register reads only), ENSM back in FDD */
static int verify_rate(const struct pluto_rate_plan *pl)
{
  uint32_t rxr = 0, txr = 0;
  uint8_t rxe = 0, txe = 0;
  struct pluto_fir_state fs;
  memset(&fs, 0, sizeof(fs));
  ad9361_get_rx_sampling_freq(phy, &rxr);
  ad9361_get_tx_sampling_freq(phy, &txr);
  ad9361_get_rx_fir_en_dis(phy, &rxe);
  ad9361_get_tx_fir_en_dis(phy, &txe);
  int fbad = read_fir_regs(&fs) < 0 ? -1
                                     : pluto_fir_check(&fs, pl->fir_taps, pl->fir_dec, fir_rx_seen.rx_gain,
                                                       fir_tx_seen.tx_gain);
  uint32_t rxc[6] = {0}, txc[6] = {0};
  ad9361_get_trx_path_clks(phy, rxc, txc);
  uint32_t tol = pl->phy_hz / 1000u + 1u;
  int ok = rxr + tol >= pl->phy_hz && rxr <= pl->phy_hz + tol && txr + tol >= pl->phy_hz &&
           txr <= pl->phy_hz + tol && rxe && txe && fbad == 0 && fir_rx_seen.rx_dec == pl->fir_dec &&
           fir_tx_seen.tx_int == pl->fir_dec;
  printf("pluto: PHY RX %u / TX %u Hz (plan %u), FIR RX %s %u taps dec %u gain %d dB, TX %s %u taps "
         "int %u gain %d dB (registers: select RX %u TX %u, start-clk RX %d TX %d, check 0x%x), "
         "PL x8 %s -> %u S/s%s\n"
         "pluto: RX path BBPLL %u ADC %u R2 %u R1 %u CLKRF %u RXSAMP %u; TX path DAC %u T2 %u T1 %u "
         "CLKTF %u TXSAMP %u\n",
         (unsigned)rxr, (unsigned)txr, (unsigned)pl->phy_hz, rxe ? "on" : "OFF",
         (unsigned)fs.rx_taps, (unsigned)fs.rx_dec, fs.rx_gain_db, txe ? "on" : "OFF",
         (unsigned)fs.tx_taps, (unsigned)fs.tx_int, fs.tx_gain_db, fs.rx_select, fs.tx_select,
         fs.rx_clk_started, fs.tx_clk_started, (unsigned)fbad,
         pl->fpga_fir ? "on" : "off", (unsigned)pl->stream_hz, ok ? "" : "  MISMATCH",
         (unsigned)rxc[0], (unsigned)rxc[1], (unsigned)rxc[2], (unsigned)rxc[3], (unsigned)rxc[4],
         (unsigned)rxc[5], (unsigned)txc[1], (unsigned)txc[2], (unsigned)txc[3], (unsigned)txc[4],
         (unsigned)txc[5]);
  return ok ? 0 : -EIO;
}

int pluto_hw_set_rate(uint32_t rate_hz)
{
  struct pluto_rate_plan pl;
  if (pluto_rate_plan(rate_hz, &pl) < 0) {
    printf("pluto: sample rate %u not supported (65105 S/s .. 61.44 MS/s)\n", (unsigned)rate_hz);
    return -EINVAL;
  }
  LOCK();
  int ret = 0;
  if (!phy) {
    ret = -ENODEV;
    goto out;
  }
  if (pl.phy_hz != cur_bb_rate) {
    ret = set_bb_rate(&pl);
    if (ret >= 0)
      ret = verify_rate(&pl);
    if (ret >= 0)
      ret = ensm_assert_fdd("after the rate change");
    if (ret < 0) {
      printf("pluto: baseband rate %u failed (%d)\n", (unsigned)pl.phy_hz, ret);
      cur_bb_rate = 0;
      suspect = 1;
      goto out;
    }
    cur_bb_rate = pl.phy_hz;
  }
  if (pl.fpga_fir != cur_fpga_fir) {
    uint32_t a = REG32(PLUTO_ADC_BASE + PLUTO_REG_GPIO_OUT);
    uint32_t d = REG32(PLUTO_DAC_BASE + PLUTO_REG_GPIO_OUT);
    REG32(PLUTO_ADC_BASE + PLUTO_REG_GPIO_OUT) = pluto_gpio_with_fir(a, pl.fpga_fir);
    REG32(PLUTO_DAC_BASE + PLUTO_REG_GPIO_OUT) = pluto_gpio_with_fir(d, pl.fpga_fir);
    cur_fpga_fir = pl.fpga_fir;
  }
out:
  UNLOCK();
  return ret;
}

int pluto_hw_fpga_fir(void)
{
  LOCK();
  int r = cur_fpga_fir > 0;
  UNLOCK();
  return r;
}

double pluto_hw_get_rate(int tx)
{
  LOCK();
  uint32_t r = 0;
  if (phy) {
    if (tx)
      ad9361_get_tx_sampling_freq(phy, &r);
    else
      ad9361_get_rx_sampling_freq(phy, &r);
  }
  double v = cur_fpga_fir > 0 ? (double)r / 8.0 : (double)r;
  UNLOCK();
  return v;
}

/* ---- LO, bandwidth, gain, ports (each under hw_lock; a failure marks the phy suspect) ---- */

#define HW_CALL(expr)                                                                             \
  do {                                                                                            \
    LOCK();                                                                                       \
    int r_ = phy ? (int)(expr) : -ENODEV;                                                         \
    if (r_ < 0 && phy)                                                                            \
      suspect = 1;                                                                                \
    UNLOCK();                                                                                     \
    return r_;                                                                                    \
  } while (0)

static int set_lo_locked(int tx, uint64_t hz)
{
  int32_t ret = tx ? ad9361_set_tx_lo_freq(phy, hz) : ad9361_set_rx_lo_freq(phy, hz);
  if (ret >= 0 && !tx)
    apply_rx_band(hz);
  return (int)ret;
}

int pluto_hw_set_lo(int tx, uint64_t hz)
{
  if (hz < AD9363A_MIN_CARRIER_FREQ_HZ || hz > AD9363A_MAX_CARRIER_FREQ_HZ)
    return -EINVAL;
  HW_CALL(set_lo_locked(tx, hz));
}

uint64_t pluto_hw_get_lo(int tx)
{
  uint64_t hz = 0;
  LOCK();
  if (phy) {
    if (tx)
      ad9361_get_tx_lo_freq(phy, &hz);
    else
      ad9361_get_rx_lo_freq(phy, &hz);
  }
  UNLOCK();
  return hz;
}

int pluto_hw_lo_power(int tx, int on)
{
  HW_CALL(tx ? ad9361_tx_lo_powerdown(phy, on ? ON : OFF) : ad9361_rx_lo_powerdown(phy, on ? ON : OFF));
}

static int set_bw_locked(int tx, uint32_t hz)
{
  int ret = (int)(tx ? ad9361_set_tx_rf_bandwidth(phy, hz) : ad9361_set_rx_rf_bandwidth(phy, hz));
  if (ret < 0)
    return ret;
  return ensm_assert_fdd(tx ? "after the TX bandwidth change" : "after the RX bandwidth change");
}

int pluto_hw_set_bw(int tx, uint32_t hz)
{
  HW_CALL(set_bw_locked(tx, hz));
}

uint32_t pluto_hw_get_bw(int tx)
{
  uint32_t hz = 0;
  LOCK();
  if (phy) {
    if (tx)
      ad9361_get_tx_rf_bandwidth(phy, &hz);
    else
      ad9361_get_rx_rf_bandwidth(phy, &hz);
  }
  UNLOCK();
  return hz;
}

int pluto_hw_set_rx_agc(int automatic)
{
  HW_CALL(ad9361_set_rx_gain_control_mode(phy, 0, automatic ? RF_GAIN_SLOWATTACK_AGC : RF_GAIN_MGC));
}

int pluto_hw_get_rx_agc(void)
{
  uint8_t mode = 0;
  LOCK();
  int r = phy ? (int)ad9361_get_rx_gain_control_mode(phy, 0, &mode) : -ENODEV;
  UNLOCK();
  return r < 0 ? r : (int)mode;
}

int pluto_hw_set_rx_gain(int32_t db)
{
  HW_CALL(ad9361_set_rx_rf_gain(phy, 0, db));
}

int pluto_hw_get_rx_gain(int32_t *db)
{
  /* the gain index reads right only with the ENSM back in FDD: a read during an ALERT excursion
   * (calibration, FIR load) once gave 73 dB for a 16 dB manual gain */
  LOCK();
  int r = -ENODEV;
  if (phy) {
    struct probe_ctx c = {phy->spi};
    struct pluto_probe_ops ops = {&c, probe_spi_read, probe_reset, probe_sleep};
    int st = -1;
    r = pluto_ensm_wait(&ops, PLUTO_ENSM_FDD, 50, 100, &st);
    if (r < 0) {
      printf("pluto: RX gain readback skipped: ENSM 0x%x, not FDD\n", (unsigned)st);
      r = -EAGAIN;
    } else {
      int32_t g = 0;
      r = (int)ad9361_get_rx_rf_gain(phy, 0, &g);
      if (r >= 0)
        *db = g;
    }
  }
  UNLOCK();
  return r < 0 ? r : 0;
}

int pluto_hw_set_tx_atten_mdb(uint32_t mdb)
{
  HW_CALL(ad9361_set_tx_attenuation(phy, 0, mdb));
}

uint32_t pluto_hw_get_tx_atten_mdb(void)
{
  uint32_t mdb = 0;
  LOCK();
  if (phy)
    ad9361_get_tx_attenuation(phy, 0, &mdb);
  UNLOCK();
  return mdb;
}

int pluto_hw_set_rx_port(uint32_t port)
{
  HW_CALL(ad9361_set_rx_rf_port_input(phy, port));
}

int pluto_hw_set_tx_port(uint32_t port)
{
  HW_CALL(ad9361_set_tx_rf_port_output(phy, port));
}

int pluto_hw_set_loopback(int mode)
{
  HW_CALL(ad9361_bist_loopback(phy, mode));
}

int pluto_hw_temp_mdeg(int32_t *mdeg)
{
  LOCK();
  int r = phy ? (int)ad9361_get_temperature(phy, mdeg) : -ENODEV;
  UNLOCK();
  return r;
}

/* one line of readbacks proving the signal path settings (stream start) */
int pluto_hw_summary(char *buf, size_t cap)
{
  LOCK();
  if (!phy) {
    UNLOCK();
    return snprintf(buf, cap, "AD9363 not open");
  }
  uint32_t rxr = 0, txr = 0, att = 0, rbw = 0, tbw = 0, rport = 0, tport = 0, bist = 0;
  int32_t g = 0;
  uint8_t mode = 0, rxe = 0, txe = 0;
  AD9361_RXFIRConfig rf;
  AD9361_TXFIRConfig tf;
  memset(&rf, 0, sizeof(rf));
  memset(&tf, 0, sizeof(tf));
  ad9361_get_rx_sampling_freq(phy, &rxr);
  ad9361_get_tx_sampling_freq(phy, &txr);
  ad9361_get_rx_fir_en_dis(phy, &rxe);
  ad9361_get_tx_fir_en_dis(phy, &txe);
  rf = fir_rx_seen; /* as loaded, never read back (see fir_rx_seen) */
  tf = fir_tx_seen;
  ad9361_get_tx_attenuation(phy, 0, &att);
  ad9361_get_rx_gain_control_mode(phy, 0, &mode);
  ad9361_get_rx_rf_gain(phy, 0, &g);
  ad9361_get_rx_rf_bandwidth(phy, &rbw);
  ad9361_get_tx_rf_bandwidth(phy, &tbw);
  ad9361_get_rx_rf_port_input(phy, &rport);
  ad9361_get_tx_rf_port_output(phy, &tport);
  ad9361_get_bist_loopback(phy, (int32_t *)&bist);
  uint32_t adc = REG32(PLUTO_ADC_BASE + PLUTO_REG_GPIO_OUT), dac = REG32(PLUTO_DAC_BASE + PLUTO_REG_GPIO_OUT);
  uint32_t dsel = REG32(PLUTO_DAC_BASE + PLUTO_REG_DAC_CHAN_CNTRL_7(0)) & 0xfu;
  uint32_t adcc = REG32(PLUTO_ADC_BASE + PLUTO_REG_ADC_CHAN_CNTRL(0));
  static const char *const modes[] = {"manual", "fast_attack", "slow_attack", "hybrid"};
  int n = snprintf(buf, cap,
                   "PHY RX %u TX %u Hz, PL x8 %s/%s (stream %u S/s); FIR RX %s %u/%u gain %d dB, TX %s "
                   "%u/%u gain %d dB; TX atten %u mdB; RX gain %d dB %s; RF BW RX %u TX %u Hz; "
                   "ports RX %u TX %u; BIST loopback %u; DAC sel %u (2 = DMA); ADC ch0 cntrl %08x; "
                   "samples RX 12-bit LSB aligned sign extended (CF32 /2048), TX 12-bit MSB aligned "
                   "(CF32 *32768)",
                   (unsigned)rxr, (unsigned)txr, (adc & 1u) ? "on" : "off", (dac & 1u) ? "on" : "off",
                   (unsigned)(cur_fpga_fir > 0 ? rxr / 8u : rxr), rxe ? "on" : "OFF",
                   (unsigned)rf.rx_coef_size, (unsigned)rf.rx_dec, (int)rf.rx_gain, txe ? "on" : "OFF",
                   (unsigned)tf.tx_coef_size, (unsigned)tf.tx_int, (int)tf.tx_gain, (unsigned)att,
                   (int)g, mode < 4 ? modes[mode] : "?", (unsigned)rbw, (unsigned)tbw,
                   (unsigned)rport, (unsigned)tport, (unsigned)bist, (unsigned)dsel, (unsigned)adcc);
  UNLOCK();
  return n;
}


/* passband (DC) gain of an AD936x FIR from its taps, in dB: RX = sum / 2^15 + gain field,
 * TX interpolating by L = mean per-phase sum / 2^15 + gain field (each output phase sums every
 * L-th tap). Same arithmetic for Linux and here: the taps and gain fields are the libad9361 ones. */
static double fir_dc_db(const int16_t *h, uint32_t n, uint32_t phases, int gain_db)
{
  double sum = 0;
  for (uint32_t i = 0; i < n; ++i)
    sum += h[i];
  double g = sum / (32768.0 * (phases ? phases : 1));
  return (g > 0 ? 20.0 * log10(g) : -99.0) + gain_db;
}

/* the TX and RX chains as programmed, from readbacks: one line each */
int pluto_hw_chain_summary(char *tx_line, size_t txcap, char *rx_line, size_t rxcap)
{
  LOCK();
  if (!phy) {
    UNLOCK();
    snprintf(tx_line, txcap, "AD9363 not open");
    snprintf(rx_line, rxcap, "AD9363 not open");
    return -ENODEV;
  }
  int tf = (int)ad9361_spi_read(phy->spi, REG_TX_ENABLE_FILTER_CTRL);
  int rfc = (int)ad9361_spi_read(phy->spi, REG_RX_ENABLE_FILTER_CTRL);
  AD9361_RXFIRConfig rf;
  AD9361_TXFIRConfig tc;
  memset(&rf, 0, sizeof(rf));
  memset(&tc, 0, sizeof(tc));
  rf = fir_rx_seen; /* as loaded, never read back (see fir_rx_seen) */
  tc = fir_tx_seen;
  uint32_t att = 0, txr = 0, rxr = 0;
  int32_t g = 0;
  uint8_t mode = 0;
  ad9361_get_tx_attenuation(phy, 0, &att);
  ad9361_get_tx_sampling_freq(phy, &txr);
  ad9361_get_rx_sampling_freq(phy, &rxr);
  ad9361_get_rx_rf_gain(phy, 0, &g);
  ad9361_get_rx_gain_control_mode(phy, 0, &mode);
  uint32_t dgpio = REG32(PLUTO_DAC_BASE + PLUTO_REG_GPIO_OUT), agpio = REG32(PLUTO_ADC_BASE + PLUTO_REG_GPIO_OUT);
  uint32_t tint = tc.tx_int ? tc.tx_int : 1, rdec = rf.rx_dec ? rf.rx_dec : 1;
  /* the loaded taps are ours (load_fir); the readback sizes/gains say they are in place */
  uint32_t tscale = tint == 4 ? tx_fir_scale : 1;
  double tdb = fir_dc_db(fir_taps(tint, tc.tx_coef_size), tc.tx_coef_size, tint, tc.tx_gain) +
               20.0 * log10((double)tscale);
  double rdb = fir_dc_db(fir_taps(rdec, rf.rx_coef_size), rf.rx_coef_size, 1, rf.rx_gain);
  int thb3 = tf >= 0 ? (tf >> 4) & 3 : -1;
  int rhb3 = rfc >= 0 ? (rfc >> 4) & 3 : -1;
  snprintf(tx_line, txcap,
           "TX chain: FPGA interp x%u (stock ADI pluto interpolator, same bitstream as Linux), "
           "FIR %u taps int %u gain %d dB x%u taps, effective passband %+.2f dB (x1 = libad9361 table as on Linux), HB THB3 %d THB2 %d THB1 %d (reg 0x002 "
           "= 0x%02x), PHY %u S/s, DAC 12-bit MSB aligned (CS16 +-32767 = full scale), atten %u mdB",
           (dgpio & 1u) ? 8u : 1u, (unsigned)tc.tx_coef_size, (unsigned)tc.tx_int, (int)tc.tx_gain,
           (unsigned)tscale, tdb, thb3, tf >= 0 ? !!(tf & THB2_EN) : -1, tf >= 0 ? !!(tf & THB1_EN) : -1,
           (unsigned)(tf & 0xff), (unsigned)txr, (unsigned)att);
  static const char *const modes[] = {"manual", "fast_attack", "slow_attack", "hybrid"};
  snprintf(rx_line, rxcap,
           "RX chain: FIR %u taps dec %u gain %d dB, taps sum + gain %+.2f dB (libad9361 table, as on Linux), HB RHB3 %d RHB2 %d RHB1 %d "
           "(reg 0x003 = 0x%02x), PHY %u S/s, FPGA decim x%u, ADC 12-bit LSB aligned sign extended "
           "(CS16 +-2048 = full scale), gain %d dB %s",
           (unsigned)rf.rx_coef_size, (unsigned)rf.rx_dec, (int)rf.rx_gain, rdb, rhb3,
           rfc >= 0 ? !!(rfc & RHB2_EN) : -1, rfc >= 0 ? !!(rfc & RHB1_EN) : -1,
           (unsigned)(rfc & 0xff), (unsigned)rxr, (agpio & 1u) ? 8u : 1u, (int)g,
           mode < 4 ? modes[mode] : "?");
  UNLOCK();
  return 0;
}

int pluto_hw_set_tx_fir_scale(uint32_t scale)
{
  if (scale != 1 && scale != 2)
    return -EINVAL;
  LOCK();
  int ret = 0;
  if (scale != tx_fir_scale && phy && cur_bb_rate && streams_active > 0) {
    /* a FIR reload forces the ENSM to ALERT: RF and the stream data path would stop */
    printf("pluto: TX FIR taps x%u refused while %u stream(s) run (stays x%u)\n", (unsigned)scale,
           streams_active, (unsigned)tx_fir_scale);
    ret = -EBUSY;
  } else if (scale != tx_fir_scale) {
    tx_fir_scale = scale;
    printf("pluto: TX FIR taps x%u (%s)\n", (unsigned)scale,
           scale == 1 ? "libad9361 table, as Linux" : "/4 passband at unity, +6.02 dB over Linux");
    /* a loaded FIR is reloaded at once at the current rate */
    if (phy && cur_bb_rate) {
      struct pluto_rate_plan pl;
      uint32_t stream = cur_fpga_fir > 0 ? cur_bb_rate / 8u : cur_bb_rate;
      ret = pluto_rate_plan(stream, &pl);
      if (ret == 0 && pl.phy_hz == cur_bb_rate) {
        ret = set_bb_rate(&pl);
        if (ret >= 0)
          ret = verify_rate(&pl);
        if (ret >= 0)
          ret = ensm_assert_fdd("after the TX FIR reload");
      }
      if (ret < 0) {
        cur_bb_rate = 0;
        suspect = 1;
      }
    }
  }
  UNLOCK();
  return ret;
}

void pluto_hw_stream_active(int on)
{
  LOCK();
  if (on)
    streams_active++;
  else if (streams_active > 0)
    streams_active--;
  UNLOCK();
}

uint32_t pluto_hw_get_tx_fir_scale(void)
{
  LOCK();
  uint32_t v = tx_fir_scale;
  UNLOCK();
  return v;
}

int pluto_hw_tx_overflow(void)
{
  LOCK();
  int v = phy ? (int)ad9361_spi_read(phy->spi, REG_CH_1_OVERFLOW) : -ENODEV;
  UNLOCK();
  return v < 0 ? v : (v & (CH_1_INT3 | CH1_HB3 | CH1_HB2 | CH1_QEC | CH1_HB1 | CH1_TFIR));
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
