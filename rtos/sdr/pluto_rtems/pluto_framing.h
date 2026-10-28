/*
 * Sample/timestamp framing of the Pluto timestamping PL design (hdl-quantulum
 * util_cpack2_timestamp / util_upack2_timestamp) for one channel (I0/Q0, 1R1T).
 * Platform independent: host unit tests build this file as is.
 *
 * Hardware contract (firmware/hdl-quantulum + patches/v0.39/hdl/0001):
 *  - The DMA bus is 64 bits wide. With one channel enabled one bus word carries two IQ samples
 *    (I16,Q16 little endian, sample n in bits 31:0, sample n+1 in bits 63:32).
 *  - A 64-bit sample counter (c_counter_binary, clocked by l_clk, enabled by the RX decimator's
 *    valid strobe) counts samples at the stream rate.
 *  - axi_ad9361 up_adc_gpio_out[31:1] (RX) / up_dac_gpio_out[31:1] (TX) = timestamp_every in BUS
 *    WORDS (so samples/2 on one channel); bit 0 stays the x8 FIR decimator/interpolator enable.
 *    0 disables timestamping.
 *  - RX: the packer emits [stamp word][E data words][stamp word]... continuously; the stamp is
 *    the counter of the first sample of the data that follows. The DMA transfer start is synced
 *    to stamp words (util_wr_sync_mux), so one DMA transfer of (E+1) words is one block.
 *  - TX: each DMA transfer must start with a stamp word followed by E data words (the unpacker
 *    counter restarts on every transfer). The block is held until counter >= stamp; a stamp in
 *    the past, or more than 16*E samples (8 blocks) ahead, discards the whole block and counts it
 *    in discarded_block_count (up_dac_gpio_in).
 * In SoapyPlutoSDR terms a block is timestamp_every samples plus two sample slots of stamp, which
 * is why the driver insists on bufflen = timestamp_every + 2.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PLUTO_FRAMING_H
#define PLUTO_FRAMING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLUTO_SAMPLE_BYTES 4u   /* one IQ sample, I16 + Q16 */
#define PLUTO_WORD_BYTES 8u     /* one DMA bus word */
#define PLUTO_STAMP_SAMPLES 2u  /* sample slots one 64-bit stamp takes on one channel */
#define PLUTO_RX_FULL_SCALE 2048.0f  /* RX: 12-bit LSB aligned, sign extended */
#define PLUTO_TX_FULL_SCALE 32768.0f /* TX: 12-bit MSB aligned */

/* largest timestamp_every accepted (bus word count must fit gpio_out[31:1], and a block must stay
 * a sane DMA transfer) */
#define PLUTO_MAX_EVERY (1u << 20)

enum pluto_format { PLUTO_FMT_CS16, PLUTO_FMT_CF32, PLUTO_FMT_CS8 };

struct pluto_geom {
  uint32_t every;         /* data samples per block (timestamp_every); 0 = untimed stream */
  uint32_t data_samples;  /* samples of data per block (== every when timed) */
  uint32_t block_samples; /* whole block incl. stamp slots (SoapyPlutoSDR "buffer size") */
  uint32_t block_bytes;   /* DMA transfer length */
  uint32_t stamp_bytes;   /* 8 when timed, 0 when untimed */
  uint32_t gpio_words;    /* value for gpio_out[31:1] */
};

/*
 * timestamp_every > 0: timed blocks of timestamp_every data samples (must be even: one channel
 * packs two samples per bus word). timestamp_every == 0: untimed blocks of untimed_len samples
 * (even). Returns 0 or -1 (invalid).
 */
int pluto_geom_init(struct pluto_geom *g, uint32_t timestamp_every, uint32_t untimed_len);

/* gpio_out register value: keep bit 0 (FIR decimator/interpolator), timestamp_every words in
 * 31:1 */
uint32_t pluto_gpio_with_every(uint32_t current, uint32_t words);
uint32_t pluto_gpio_with_fir(uint32_t current, int fir_enable);

/* little endian 64-bit stamp at the start of a block (the DMA writes the bus word as is) */
uint64_t pluto_stamp_get(const void *block);
void pluto_stamp_put(void *block, uint64_t stamp);

/* SoapySDR::ticksToTimeNs / timeNsToTicks, bit exact (SoapySDR/Time.hpp 0.8.1) */
long long pluto_ticks_to_ns(long long ticks, double rate);
long long pluto_ns_to_ticks(long long time_ns, double rate);

/* RX block continuity: classify each block stamp against the expected one */
enum pluto_rx_verdict {
  PLUTO_RX_FIRST,   /* first block after (re)start */
  PLUTO_RX_OK,      /* exactly contiguous */
  PLUTO_RX_GAP,     /* later than expected: blocks lost (overflow) */
  PLUTO_RX_BAD      /* earlier than expected or absurdly far: framing lost (not a stamp) */
};
struct pluto_rx_track {
  uint64_t expected;   /* stamp the next block must carry */
  int have;
  uint64_t blocks, overflows, lost_samples, misframed;
};
void pluto_rx_track_reset(struct pluto_rx_track *t);
/* a plausible stamp is never more than this far ahead of the expected one (~2^36 samples is
 * hours at any Pluto rate) */
#define PLUTO_RX_MAX_JUMP (1ull << 36)
enum pluto_rx_verdict pluto_rx_track_block(struct pluto_rx_track *t, uint64_t stamp,
                                           uint32_t data_samples);

/* sample conversion between the bus layout and the SoapySDR formats */
size_t pluto_format_bytes(enum pluto_format f); /* bytes per complex sample */
void pluto_rx_convert(enum pluto_format f, void *dst, const void *src, size_t samples);
void pluto_tx_convert(enum pluto_format f, void *dst, const void *src, size_t samples);

/*
 * RX reader: consumes completed blocks, hands out samples with their stamp. The block source is
 * abstract so the same code runs on the DMA ring and in host tests.
 */
struct pluto_rx_source {
  void *ctx;
  /* next completed block, waiting up to timeout_us; 0 ok, -1 timeout */
  int (*next)(void *ctx, const uint8_t **block, long timeout_us);
  /* the reader is done with the block returned by the last next() */
  void (*release)(void *ctx);
};
struct pluto_rx_reader {
  struct pluto_geom geom;
  const uint8_t *cur;     /* current block, NULL = none */
  uint32_t remaining;     /* data samples left in cur */
  uint32_t offset;        /* byte offset of the next sample in cur */
  uint64_t stamp;         /* counter of the next sample */
  uint64_t untimed_count; /* untimed streams: running sample count */
  struct pluto_rx_track track;
};
void pluto_rx_reader_init(struct pluto_rx_reader *r, const struct pluto_geom *g);
/* drop the current block (stream stop) */
void pluto_rx_reader_reset(struct pluto_rx_reader *r, const struct pluto_rx_source *src);
/*
 * Read up to n samples (never crossing a block). Returns the count (> 0), PLUTO_ERR_TIMEOUT, or
 * PLUTO_ERR_FRAMING when a block is not a valid stamp/data block (the caller resynchronises the
 * DMA). *has_time is set for timed streams, *stamp = counter of the first returned sample.
 */
#define PLUTO_ERR_TIMEOUT (-1)
#define PLUTO_ERR_FRAMING (-2)
int pluto_rx_read(struct pluto_rx_reader *r, const struct pluto_rx_source *src, enum pluto_format f,
                  void *dst, size_t n, int *has_time, uint64_t *stamp, long timeout_us);

/*
 * TX writer, SoapyPlutoSDR tx_streamer_usb_gadget::send() semantics on DMA blocks:
 *  - a new block starts at the requested time (or continues the previous time without one);
 *  - a timed write into a partly filled block first pads zeros up to the requested time (a time
 *    behind the fill position pads the rest of the block and starts a new one);
 *  - a full block, or END_BURST once all samples are in, is committed; a partial block is
 *    zero-padded to the full block length (the PL expects whole blocks).
 * Unlike the gadget, a block slot is acquired BEFORE any sample is consumed, so a timeout never
 * loses accepted samples.
 */
struct pluto_tx_sink {
  void *ctx;
  /* a free block slot, waiting up to timeout_us; 0 ok, -1 timeout */
  int (*acquire)(void *ctx, uint8_t **block, long timeout_us);
  /* hand the filled slot (from the last acquire) to the DMA */
  void (*commit)(void *ctx);
};
struct pluto_tx_writer {
  struct pluto_geom geom;
  uint8_t *cur;        /* current block, NULL = none */
  uint32_t stored;     /* sample slots used in cur, stamp slots included */
  uint64_t next_stamp; /* counter of the next sample to store */
  uint64_t blocks, backwards;
};
void pluto_tx_writer_init(struct pluto_tx_writer *w, const struct pluto_geom *g);
/* forget a partly filled block (stream stop); its slot is committed zero padded */
void pluto_tx_writer_flush(struct pluto_tx_writer *w, const struct pluto_tx_sink *sink);
void pluto_tx_writer_reset(struct pluto_tx_writer *w);
/* returns samples consumed (>= 0) or PLUTO_ERR_TIMEOUT */
int pluto_tx_write(struct pluto_tx_writer *w, const struct pluto_tx_sink *sink, enum pluto_format f,
                   const void *src, size_t n, int has_time, uint64_t stamp, int end_burst,
                   long timeout_us);

#ifdef __cplusplus
}
#endif
#endif
