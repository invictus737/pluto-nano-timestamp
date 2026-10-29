/*
 * AD9363A + axi_ad9361 control for the Pluto Nano through the ADI no-OS ad9361 driver.
 * Not thread safe: the SoapySDR device serialises every call.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PLUTO_HW_H
#define PLUTO_HW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up the AD9363 (PL check, reset + PRODUCT_ID probe, setup, digital interface tuning,
 * cores, health check) or reuse the healthy one: 0 ok, negative error with nothing left
 * initialised. Every pluto_hw_* call is serialised by one lock (thread safe, shared by all
 * SoapySDR device instances); pluto_hw_close() releases one open. A phy whose operation failed
 * is rebuilt from reset by the next open made while no other open is held.
 */
int pluto_hw_open(uint32_t refclk_hz);
void pluto_hw_close(void);
int pluto_hw_is_open(void);

/*
 * Stream sample rate for RX and TX together (the AD9363 shares one BBPLL; the counter that
 * timestamps both directions runs at this rate). Below 25 MHz/48 the PL x8 FIR decimator and
 * interpolator are used and the AD9363 runs 8x faster (SoapyPlutoSDR + libad9361 rules, with the
 * same baseband FIR sets). 0 ok, negative error.
 */
int pluto_hw_set_rate(uint32_t rate_hz);
/* readback: AD9363 rate (BBPLL derived, e.g. 1599999 Hz for 1.6 MHz), / 8 when the PL FIR is
 * used, like cf-ad9361-lpc sampling_frequency */
double pluto_hw_get_rate(int tx);
int pluto_hw_fpga_fir(void);

int pluto_hw_set_lo(int tx, uint64_t hz);
uint64_t pluto_hw_get_lo(int tx);
int pluto_hw_lo_power(int tx, int on);

int pluto_hw_set_bw(int tx, uint32_t hz);
uint32_t pluto_hw_get_bw(int tx);

int pluto_hw_set_rx_agc(int automatic);
/* gain control mode readback: 0 manual, 2 slow attack (RF_GAIN_*), negative error */
int pluto_hw_get_rx_agc(void);
int pluto_hw_set_rx_gain(int32_t db);
int32_t pluto_hw_get_rx_gain(void);
int pluto_hw_set_tx_atten_mdb(uint32_t mdb);
uint32_t pluto_hw_get_tx_atten_mdb(void);

/* 0 = A_BALANCED / TXA */
int pluto_hw_set_rx_port(uint32_t port);
int pluto_hw_set_tx_port(uint32_t port);

/* AD936x BIST loopback: 0 off, 1 digital (TX->RX in the AD9363), 2 RF */
int pluto_hw_set_loopback(int mode);

/* one-line readback summary of the signal path (rates, FIR, gains, BW, ports, data select) */
int pluto_hw_summary(char *buf, size_t cap);
/* "TX chain: ..." and "RX chain: ..." readback lines (FIR passband gain from the loaded taps) */
int pluto_hw_chain_summary(char *tx_line, size_t txcap, char *rx_line, size_t rxcap);

/* AD9363 temperature in milli-degrees C */
int pluto_hw_temp_mdeg(int32_t *mdeg);

/* timestamping: gpio_out[31:1] of the ADC (RX) or DAC (TX) core */
void pluto_hw_set_timestamp_words(int tx, uint32_t words);
/* upack discarded_block_count (late or too early TX blocks), free running */
uint32_t pluto_hw_tx_discarded(void);
/* core DMA interface status (REG_DMA_STATUS, read and clear): RX bit 2 = the packer overflowed
 * (no DMA transfer ready), TX bit 1 = the unpacker underflowed (no data) */
#define PLUTO_DMA_STATUS_OVF 0x4u
#define PLUTO_DMA_STATUS_UNF 0x2u
uint32_t pluto_hw_dma_status_clear(int tx);

#ifdef __cplusplus
}
#endif
#endif
