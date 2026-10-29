// Native SoapySDR driver for the Pluto Nano on RTEMS (no Linux, no libiio).
//
// Registered statically as "plutosdr" and shaped like pgreenland's SoapyPlutoSDR timestamp branch
// (sdr_gadget_timestamping), so nexus2 opens it with its existing arguments
// (driver=plutosdr, uri=local:|usb:|ip:, direct=1, local_timestamp=1, timestamp_every=N,
// loopback=0) and sees the same keys, formats, gains, ranges and stream semantics:
//  - driver key "PlutoSDR", hardware key "ADALM-PLUTO", one RX and one TX channel, full duplex;
//  - RX CS16 is 12 bit LSB aligned (full scale 2048), TX CS16 is MSB aligned (full scale 32768),
//    CF32/CS8 converted like the gadget streamers;
//  - with timestamp_every > 0 every RX block carries the FPGA sample counter (SOAPY_SDR_HAS_TIME,
//    timeNs = counter at the stream rate), TX blocks are held by the FPGA until their time;
//    bufflen = timestamp_every + 2, MTU = timestamp_every;
//  - gains: RX "PGA" 0..73 dB (manual, or slow attack AGC with setGainMode), TX "PGA" 0..89 dB
//    (= 89 dB - attenuation);
//  - hasHardwareTime() follows timestamp_every; getHardwareTime() is extrapolated from the newest
//    RX block stamp while RX runs and throws otherwise (so nexus2 keeps its RX-derived clock, as
//    with SoapyPlutoSDR whose getHardwareTime always throws).
// The transceiver is brought up once with the ADI no-OS ad9361 driver (pluto_hw.c); streaming is
// AXI DMAC block rings with interrupt-driven service threads (pluto_dma.c); the framing is
// pluto_framing.c (host unit tested).
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Time.hpp>
#include <SoapySDR/Types.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <time.h>
#include <vector>

#include <rtems.h>

#include "pluto_dma.h"
#include "pluto_framing.h"
#include "pluto_hw.h"

extern "C" const char *nexus_config_get(const char *key) __attribute__((weak));

namespace {

constexpr double kRxGainMax = 73.0;
constexpr double kTxGainMax = 89.0;
constexpr uint32_t kTxSlots = 16;       // the gadget's TX queue depth
constexpr uint32_t kRxMinSlots = 16;
constexpr double kRxBufferSeconds = 0.064;
constexpr size_t kRxPoolMax = 2u << 20; // DMA-coherent bytes per direction at most

int64_t monotonic_ns()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

uint32_t parse_hz(const char *s)
{
  // U-Boot style values may be bare ("39999331") or DT style ("<40000000>")
  if (!s)
    return 0;
  uint64_t v = 0;
  bool any = false;
  for (; *s; ++s) {
    if (*s >= '0' && *s <= '9') {
      v = v * 10 + uint64_t(*s - '0');
      any = true;
      if (v > 0xffffffffu)
        return 0;
    } else if (any) {
      break;
    }
  }
  return any ? uint32_t(v) : 0;
}

uint32_t reference_clock(const SoapySDR::Kwargs &args, std::string &source)
{
  const char *keys[] = {"xo_correction", "ad936x_ext_refclk"};
  for (const char *k : keys) {
    auto it = args.find(k);
    uint32_t hz = it != args.end() ? parse_hz(it->second.c_str()) : 0;
    if (hz >= 10000000u && hz <= 80000000u) {
      source = std::string("device argument ") + k;
      return hz;
    }
  }
  if (nexus_config_get) {
    for (const char *k : keys) {
      uint32_t hz = parse_hz(nexus_config_get(k));
      if (hz >= 10000000u && hz <= 80000000u) { // AD9363 reference input range
        source = std::string("U-Boot ") + k;
        return hz;
      }
    }
  }
  source = "nominal";
  return 40000000u;
}

uint32_t parse_u32(const SoapySDR::Kwargs &args, const char *key, uint32_t dflt, bool &present)
{
  auto it = args.find(key);
  present = it != args.end();
  if (!present)
    return dflt;
  char *end = nullptr;
  unsigned long v = std::strtoul(it->second.c_str(), &end, 10);
  if (it->second.empty() || !end || *end != '\0')
    throw std::runtime_error(std::string("invalid value for ") + key + ", expected number");
  return uint32_t(v);
}

pluto_format to_format(const std::string &f)
{
  if (f == SOAPY_SDR_CS16)
    return PLUTO_FMT_CS16;
  if (f == SOAPY_SDR_CF32)
    return PLUTO_FMT_CF32;
  if (f == SOAPY_SDR_CS8)
    return PLUTO_FMT_CS8;
  throw std::runtime_error("setupStream invalid format '" + f +
                           "' -- only CS8, CS16 and CF32 are supported by the Pluto driver");
}

uint32_t pow2_by_rate(double rate)
{
  // SoapyPlutoSDR set_buffer_size_by_samplerate: ~60 reads per second, power of two
  uint32_t want = uint32_t(std::lround(rate / 60.0));
  uint32_t p = 2;
  while (p < want && p < (1u << 20))
    p <<= 1;
  return p;
}

struct Stream {
  int dir = 0;
  pluto_format fmt = PLUTO_FMT_CS16;
  pluto_geom geom{};
  pluto_dma *dma = nullptr;
  bool active = false;
  std::mutex mutex;
  // level meter over the samples the stream moves (diagnostics), logged every kLevelPeriodNs
  pluto_level level{};
  int64_t level_since_ns = 0;
};

constexpr int64_t kLevelPeriodNs = 10000000000LL;

struct RxStream : Stream {
  pluto_rx_reader reader{};
  pluto_rx_source source{};
  uint64_t reported_overflows = 0;
  int64_t last_overflow_log_ns = 0;
  uint64_t restarts = 0;
};

struct TxStream : Stream {
  pluto_tx_writer writer{};
  pluto_tx_sink sink{};
  uint32_t discard_base = 0;    // discarded_block_count at activation
  uint32_t discard_reported = 0;
};

int rx_next(void *ctx, const uint8_t **block, long timeout_us)
{
  return pluto_dma_rx_next(static_cast<pluto_dma *>(ctx), block, timeout_us);
}
void rx_release(void *ctx)
{
  pluto_dma_rx_release(static_cast<pluto_dma *>(ctx));
}
int tx_acquire(void *ctx, uint8_t **block, long timeout_us)
{
  return pluto_dma_tx_acquire(static_cast<pluto_dma *>(ctx), block, timeout_us);
}
void tx_commit(void *ctx)
{
  pluto_dma_tx_commit(static_cast<pluto_dma *>(ctx));
}

} // namespace

class SoapyPlutoRtems : public SoapySDR::Device
{
public:
  explicit SoapyPlutoRtems(const SoapySDR::Kwargs &args);
  ~SoapyPlutoRtems() override;

  // identification
  std::string getDriverKey(void) const override { return "PlutoSDR"; }
  std::string getHardwareKey(void) const override { return "ADALM-PLUTO"; }
  SoapySDR::Kwargs getHardwareInfo(void) const override;

  // channels
  size_t getNumChannels(const int) const override { return 1; }
  bool getFullDuplex(const int, const size_t) const override { return true; }

  // streams
  std::vector<std::string> getStreamFormats(const int, const size_t) const override
  {
    return {SOAPY_SDR_CS8, SOAPY_SDR_CS16, SOAPY_SDR_CF32};
  }
  std::string getNativeStreamFormat(const int direction, const size_t, double &fullScale) const override
  {
    fullScale = direction == SOAPY_SDR_RX ? PLUTO_RX_FULL_SCALE : PLUTO_TX_FULL_SCALE;
    return SOAPY_SDR_CS16;
  }
  SoapySDR::ArgInfoList getStreamArgsInfo(const int direction, const size_t channel) const override;
  SoapySDR::Stream *setupStream(const int direction, const std::string &format,
                                const std::vector<size_t> &channels, const SoapySDR::Kwargs &args) override;
  void closeStream(SoapySDR::Stream *stream) override;
  size_t getStreamMTU(SoapySDR::Stream *stream) const override;
  int activateStream(SoapySDR::Stream *stream, const int flags, const long long timeNs,
                     const size_t numElems) override;
  int deactivateStream(SoapySDR::Stream *stream, const int flags, const long long timeNs) override;
  int readStream(SoapySDR::Stream *stream, void *const *buffs, const size_t numElems, int &flags,
                 long long &timeNs, const long timeoutUs) override;
  int writeStream(SoapySDR::Stream *stream, const void *const *buffs, const size_t numElems, int &flags,
                  const long long timeNs, const long timeoutUs) override;
  int readStreamStatus(SoapySDR::Stream *stream, size_t &chanMask, int &flags, long long &timeNs,
                       const long timeoutUs) override;

  // antennas
  std::vector<std::string> listAntennas(const int direction, const size_t) const override
  {
    return {direction == SOAPY_SDR_RX ? "A_BALANCED" : "A"};
  }
  void setAntenna(const int direction, const size_t channel, const std::string &name) override;
  std::string getAntenna(const int direction, const size_t) const override
  {
    return direction == SOAPY_SDR_RX ? "A_BALANCED" : "A";
  }
  bool hasDCOffsetMode(const int, const size_t) const override { return false; }

  // gains
  std::vector<std::string> listGains(const int, const size_t) const override { return {"PGA"}; }
  bool hasGainMode(const int direction, const size_t) const override { return direction == SOAPY_SDR_RX; }
  void setGainMode(const int direction, const size_t channel, const bool automatic) override;
  bool getGainMode(const int direction, const size_t) const override
  {
    return direction == SOAPY_SDR_RX && gain_auto_;
  }
  void setGain(const int direction, const size_t channel, const double value) override;
  void setGain(const int direction, const size_t channel, const std::string &, const double value) override
  {
    setGain(direction, channel, value);
  }
  double getGain(const int direction, const size_t channel) const override
  {
    return getGain(direction, channel, "PGA");
  }
  double getGain(const int direction, const size_t channel, const std::string &name) const override;
  SoapySDR::Range getGainRange(const int direction, const size_t) const override
  {
    return SoapySDR::Range(0, direction == SOAPY_SDR_RX ? kRxGainMax : kTxGainMax);
  }
  SoapySDR::Range getGainRange(const int direction, const size_t channel, const std::string &) const override
  {
    return getGainRange(direction, channel);
  }

  // frequency
  void setFrequency(const int direction, const size_t channel, const std::string &name,
                    const double frequency, const SoapySDR::Kwargs &args) override;
  double getFrequency(const int direction, const size_t channel, const std::string &name) const override;
  std::vector<std::string> listFrequencies(const int, const size_t) const override { return {"RF"}; }
  SoapySDR::RangeList getFrequencyRange(const int, const size_t, const std::string &) const override
  {
    return SoapySDR::RangeList(1, SoapySDR::Range(325000000.0, 3800000000.0)); // AD9363A
  }
  SoapySDR::ArgInfoList getFrequencyArgsInfo(const int, const size_t) const override { return {}; }

  // sample rate and bandwidth
  void setSampleRate(const int direction, const size_t channel, const double rate) override;
  double getSampleRate(const int direction, const size_t channel) const override;
  std::vector<double> listSampleRates(const int, const size_t) const override
  {
    return {65105, 1e6, 2e6, 3e6, 4e6, 5e6, 6e6, 7e6, 8e6, 9e6, 10e6};
  }
  SoapySDR::RangeList getSampleRateRange(const int, const size_t) const override
  {
    return SoapySDR::RangeList(1, SoapySDR::Range(25e6 / 384, 61440000));
  }
  void setBandwidth(const int direction, const size_t channel, const double bw) override;
  double getBandwidth(const int direction, const size_t channel) const override;
  std::vector<double> listBandwidths(const int, const size_t) const override
  {
    return {0.2e6, 1e6, 2e6, 3e6, 4e6, 5e6, 6e6, 7e6, 8e6, 9e6, 10e6};
  }

  // time
  bool hasHardwareTime(const std::string &) const override
  {
    return timestamp_every_rx_ + timestamp_every_tx_ > 0;
  }
  long long getHardwareTime(const std::string &what) const override;
  void setHardwareTime(const long long timeNs, const std::string &what) override;

  // sensors and settings
  std::vector<std::string> listSensors(void) const override { return {"ad9361-phy_temp0"}; }
  SoapySDR::ArgInfo getSensorInfo(const std::string &key) const override;
  std::string readSensor(const std::string &key) const override;
  SoapySDR::ArgInfoList getSettingInfo(void) const override;
  void writeSetting(const std::string &key, const std::string &value) override;
  std::string readSetting(const std::string &key) const override;

private:
  void handle_args(const SoapySDR::Kwargs &args, int direction);
  void check_channel(size_t channel) const
  {
    if (channel != 0)
      throw std::runtime_error("PlutoSDR: only channel 0 exists");
  }
  bool rx_handle(SoapySDR::Stream *s) const { return s && s == reinterpret_cast<SoapySDR::Stream *>(rx_); }
  bool tx_handle(SoapySDR::Stream *s) const { return s && s == reinterpret_cast<SoapySDR::Stream *>(tx_); }
  static void rx_block_hook(void *ctx, const uint8_t *block);
  bool hw_now_ticks(uint64_t &ticks) const;
  int restart_rx_locked();
  void log_signal_path(const char *when);
  void level_account(Stream *s, const void *buf, size_t samples, bool rx);

  mutable std::recursive_mutex dev_mutex_;
  RxStream *rx_ = nullptr;
  TxStream *tx_ = nullptr;
  uint32_t timestamp_every_rx_ = 0;
  uint32_t timestamp_every_tx_ = 0;
  int loopback_ = -1;
  bool gain_auto_ = false;
  std::atomic<int> rx_gain_db_{-1000};   // last RX gain readback (for the level log)
  std::atomic<uint32_t> tx_atten_mdb_{0}; // last TX attenuation readback
  double rate_ = 1e6;           // nominal stream rate: tick <-> ns conversions
  uint32_t refclk_hz_ = 0;
  std::string refclk_source_;
  std::atomic<int64_t> time_offset_ticks_{0};
  // RX time anchor (seqlock): stamp just past the newest completed block, and when it completed
  mutable std::atomic<uint32_t> anchor_seq_{0};
  uint64_t anchor_ticks_ = 0;
  int64_t anchor_ns_ = 0;
  std::atomic<bool> anchor_valid_{false};
};

// ---------------------------------------------------------------------------------------------

SoapyPlutoRtems::SoapyPlutoRtems(const SoapySDR::Kwargs &args)
{
  if (args.count("label") != 0)
    SoapySDR_logf(SOAPY_SDR_INFO, "Opening %s...", args.at("label").c_str());
  refclk_hz_ = reference_clock(args, refclk_source_);
  SoapySDR_logf(SOAPY_SDR_INFO, "PlutoSDR (RTEMS native): AD9363A reference %u Hz (%s)",
                unsigned(refclk_hz_), refclk_source_.c_str());
  int ret = pluto_hw_open(refclk_hz_);
  if (ret < 0)
    throw std::runtime_error("PlutoSDR: AD9363 initialisation failed (" + std::to_string(ret) + ")");
  // the destructor does not run when the constructor throws: release the open here
  try {
    // SoapyPlutoSDR constructor: RX A_BALANCED, manual gain, TX A
    setAntenna(SOAPY_SDR_RX, 0, "A_BALANCED");
    setGainMode(SOAPY_SDR_RX, 0, false);
    setAntenna(SOAPY_SDR_TX, 0, "A");
    // TX FIR scale: the device argument wins, else the board config key (config.txt [RADIO])
    if (args.count("tx_fir_scale") == 0 && nexus_config_get) {
      const char *v = nexus_config_get("ad9363_tx_fir_scale");
      if (v && *v)
        handle_args(SoapySDR::Kwargs{{"tx_fir_scale", v}}, -1);
    }
    handle_args(args, -1);
  } catch (...) {
    pluto_hw_close();
    throw;
  }
}

SoapyPlutoRtems::~SoapyPlutoRtems()
{
  if (rx_)
    closeStream(reinterpret_cast<SoapySDR::Stream *>(rx_));
  if (tx_)
    closeStream(reinterpret_cast<SoapySDR::Stream *>(tx_));
  pluto_hw_close();
}

void SoapyPlutoRtems::handle_args(const SoapySDR::Kwargs &args, int direction)
{
  std::string direct = "0";
  if (args.count("usb_direct"))
    direct = args.at("usb_direct");
  if (args.count("direct"))
    direct = args.at("direct");
  if (direct != "0" && direct != "1")
    throw std::runtime_error("invalid value for direct, expected 0/1");
  // direct=1 selects the gadget streamer on Linux; here the DMA is always direct.

  bool present = false;
  uint32_t lb = parse_u32(args, "loopback", 0, present);
  if (present) {
    if (lb > 2)
      throw std::runtime_error("invalid value for loopback, expected 0-2");
    if (int(lb) != loopback_) {
      std::lock_guard<std::recursive_mutex> lock(dev_mutex_);
      if (pluto_hw_set_loopback(int(lb)) < 0)
        throw std::runtime_error("failed to set loopback mode");
      if (lb == 1)
        SoapySDR_logf(SOAPY_SDR_INFO, "digital loopback enabled");
      else if (lb == 2)
        SoapySDR_logf(SOAPY_SDR_INFO, "analog loopback enabled");
      loopback_ = int(lb);
    }
  }

  uint32_t scale = parse_u32(args, "tx_fir_scale", 1, present);
  if (present) {
    if (scale != 1 && scale != 2)
      throw std::runtime_error("invalid value for tx_fir_scale, expected 1 (Linux table) or 2 (unity /4 TX FIR)");
    std::lock_guard<std::recursive_mutex> lock(dev_mutex_);
    if (pluto_hw_set_tx_fir_scale(scale) < 0)
      throw std::runtime_error("PlutoSDR: TX FIR reload failed");
  }

  uint32_t every = parse_u32(args, "timestamp_every", 0, present);
  if (present) {
    if (every & 1u)
      throw std::runtime_error("timestamp_every must be even on one channel (two samples per bus word)");
    if (every > PLUTO_MAX_EVERY)
      throw std::runtime_error("timestamp_every too large");
    if (direction != SOAPY_SDR_TX) {
      timestamp_every_rx_ = every;
      SoapySDR_logf(SOAPY_SDR_INFO, "RX timestamping enabled, every %u samples", unsigned(every));
    }
    if (direction != SOAPY_SDR_RX) {
      timestamp_every_tx_ = every;
      SoapySDR_logf(SOAPY_SDR_INFO, "TX timestamping enabled, every %u samples", unsigned(every));
    }
  }
}

SoapySDR::Kwargs SoapyPlutoRtems::getHardwareInfo(void) const
{
  SoapySDR::Kwargs info;
  info["backend"] = "rtems-native";
  info["model"] = "Nano PlutoSDR (Z7010/AD9363A)";
  info["ad936x_refclk"] = std::to_string(refclk_hz_);
  info["ad936x_refclk_source"] = refclk_source_;
  info["timestamping"] = "pluto-timestamp v0.39";
  return info;
}

SoapySDR::ArgInfoList SoapyPlutoRtems::getStreamArgsInfo(const int, const size_t) const
{
  SoapySDR::ArgInfoList list;
  SoapySDR::ArgInfo bufflen;
  bufflen.key = "bufflen";
  bufflen.name = "Buffer length";
  bufflen.description = "Samples per DMA block including the stamp (timestamp_every + 2 when timed)";
  bufflen.type = SoapySDR::ArgInfo::INT;
  list.push_back(bufflen);
  SoapySDR::ArgInfo every;
  every.key = "timestamp_every";
  every.name = "Timestamp every";
  every.description = "Data samples per timestamped block (even), 0 = untimed";
  every.type = SoapySDR::ArgInfo::INT;
  list.push_back(every);
  return list;
}

// ---- streams ----

SoapySDR::Stream *SoapyPlutoRtems::setupStream(const int direction, const std::string &format,
                                               const std::vector<size_t> &channels,
                                               const SoapySDR::Kwargs &args)
{
  if (direction != SOAPY_SDR_RX && direction != SOAPY_SDR_TX)
    throw std::runtime_error("setupStream: invalid direction");
  for (size_t c : channels)
    check_channel(c);
  if (channels.size() > 1)
    throw std::runtime_error("setupStream: the Pluto driver streams one channel (1R1T)");
  pluto_format fmt = to_format(format);
  SoapySDR_logf(SOAPY_SDR_INFO, "Using format %s.", format.c_str());
  handle_args(args, direction);

  std::lock_guard<std::recursive_mutex> lock(dev_mutex_);
  const bool rx = direction == SOAPY_SDR_RX;
  if ((rx && rx_) || (!rx && tx_))
    throw std::runtime_error("setupStream: a stream for this direction is already open");

  const uint32_t every = rx ? timestamp_every_rx_ : timestamp_every_tx_;
  bool present = false;
  uint32_t bufflen = parse_u32(args, "bufflen", 0, present);
  uint32_t untimed = 0;
  if (every > 0) {
    if (present && bufflen > 0 && bufflen != every + PLUTO_STAMP_SAMPLES)
      SoapySDR_logf(SOAPY_SDR_WARNING,
                    "bufflen %u does not match timestamp_every %u (+2): using %u (the FPGA block)",
                    unsigned(bufflen), unsigned(every), unsigned(every + PLUTO_STAMP_SAMPLES));
  } else {
    untimed = (present && bufflen > 0) ? bufflen : (rx ? pow2_by_rate(rate_) : 4096u);
    untimed = (untimed + 1u) & ~1u;
  }

  std::unique_ptr<Stream> s;
  if (rx)
    s.reset(new RxStream);
  else
    s.reset(new TxStream);
  s->dir = direction;
  s->fmt = fmt;
  if (pluto_geom_init(&s->geom, every, untimed) != 0)
    throw std::runtime_error("setupStream: invalid block geometry");

  uint32_t slots = kTxSlots;
  if (rx) {
    double per_block = double(s->geom.data_samples) / std::max(rate_, 1.0);
    slots = std::max<uint32_t>(kRxMinSlots, uint32_t(std::ceil(kRxBufferSeconds / per_block)));
    slots = std::min<uint32_t>(slots, 256);
  }
  slots = std::max<uint32_t>(2, std::min<uint32_t>(slots, uint32_t(kRxPoolMax / s->geom.block_bytes)));
  s->dma = pluto_dma_create(rx ? PLUTO_DMA_RX : PLUTO_DMA_TX, s->geom.block_bytes, slots);
  if (!s->dma)
    throw std::runtime_error("setupStream: DMA setup failed");

  pluto_hw_lo_power(rx ? 0 : 1, 1); // SoapyPlutoSDR: LO on at setupStream
  if (rx) {
    RxStream *r = static_cast<RxStream *>(s.release());
    pluto_rx_reader_init(&r->reader, &r->geom);
    r->source.ctx = r->dma;
    r->source.next = rx_next;
    r->source.release = rx_release;
    if (every > 0)
      pluto_dma_set_block_hook(r->dma, rx_block_hook, this);
    rx_ = r;
    SoapySDR_logf(SOAPY_SDR_INFO, "RX stream: %u samples/block%s, %u blocks", unsigned(r->geom.data_samples),
                  every ? " + stamp" : "", unsigned(slots));
    return reinterpret_cast<SoapySDR::Stream *>(r);
  }
  TxStream *t = static_cast<TxStream *>(s.release());
  pluto_tx_writer_init(&t->writer, &t->geom);
  t->sink.ctx = t->dma;
  t->sink.acquire = tx_acquire;
  t->sink.commit = tx_commit;
  tx_ = t;
  SoapySDR_logf(SOAPY_SDR_INFO, "TX stream: %u samples/block%s, %u blocks", unsigned(t->geom.data_samples),
                every ? " + stamp" : "", unsigned(slots));
  return reinterpret_cast<SoapySDR::Stream *>(t);
}

void SoapyPlutoRtems::closeStream(SoapySDR::Stream *stream)
{
  std::lock_guard<std::recursive_mutex> lock(dev_mutex_);
  if (rx_handle(stream)) {
    deactivateStream(stream, 0, 0);
    std::lock_guard<std::mutex> slock(rx_->mutex);
    pluto_dma_destroy(rx_->dma);
    rx_->dma = nullptr;
    anchor_valid_ = false;
    pluto_hw_lo_power(0, 0);
    delete rx_;
    rx_ = nullptr;
  } else if (tx_handle(stream)) {
    deactivateStream(stream, 0, 0);
    std::lock_guard<std::mutex> slock(tx_->mutex);
    pluto_dma_destroy(tx_->dma);
    tx_->dma = nullptr;
    pluto_hw_lo_power(1, 0);
    delete tx_;
    tx_ = nullptr;
  }
}

size_t SoapyPlutoRtems::getStreamMTU(SoapySDR::Stream *stream) const
{
  if (rx_handle(stream))
    return rx_->geom.data_samples;
  if (tx_handle(stream))
    return tx_->geom.data_samples;
  return 0;
}

int SoapyPlutoRtems::activateStream(SoapySDR::Stream *stream, const int flags, const long long, const size_t)
{
  if (flags & ~SOAPY_SDR_END_BURST)
    return SOAPY_SDR_NOT_SUPPORTED;
  std::lock_guard<std::recursive_mutex> lock(dev_mutex_);
  if (rx_handle(stream)) {
    std::lock_guard<std::mutex> slock(rx_->mutex);
    if (rx_->active)
      return 0;
    pluto_hw_set_timestamp_words(0, rx_->geom.gpio_words);
    pluto_hw_dma_status_clear(0);
    pluto_rx_reader_reset(&rx_->reader, nullptr);
    anchor_valid_ = false;
    pluto_level_reset(&rx_->level);
    rx_->level_since_ns = monotonic_ns();
    log_signal_path("RX stream start");
    if (pluto_dma_start(rx_->dma) != 0)
      return SOAPY_SDR_STREAM_ERROR;
    rx_->active = true;
    return 0;
  }
  if (tx_handle(stream)) {
    std::lock_guard<std::mutex> slock(tx_->mutex);
    if (tx_->active)
      return 0;
    pluto_hw_set_timestamp_words(1, tx_->geom.gpio_words);
    pluto_hw_dma_status_clear(1);
    pluto_tx_writer_reset(&tx_->writer);
    tx_->discard_base = pluto_hw_tx_discarded();
    tx_->discard_reported = 0;
    pluto_level_reset(&tx_->level);
    tx_->level_since_ns = monotonic_ns();
    log_signal_path("TX stream start");
    if (pluto_dma_start(tx_->dma) != 0)
      return SOAPY_SDR_STREAM_ERROR;
    tx_->active = true;
    return 0;
  }
  return SOAPY_SDR_STREAM_ERROR;
}

int SoapyPlutoRtems::deactivateStream(SoapySDR::Stream *stream, const int, const long long)
{
  if (rx_handle(stream)) {
    // stop first (wakes a blocked reader), then take the stream lock
    pluto_dma_stop(rx_->dma);
    std::lock_guard<std::mutex> slock(rx_->mutex);
    pluto_rx_reader_reset(&rx_->reader, nullptr);
    rx_->active = false;
    anchor_valid_ = false;
    return 0;
  }
  if (tx_handle(stream)) {
    pluto_dma_stop(tx_->dma);
    std::lock_guard<std::mutex> slock(tx_->mutex);
    pluto_tx_writer_reset(&tx_->writer);
    tx_->active = false;
    return 0;
  }
  return SOAPY_SDR_STREAM_ERROR;
}

int SoapyPlutoRtems::restart_rx_locked()
{
  // framing lost (a transfer did not start on a stamp word): restart the DMA, which re-syncs the
  // first transfer to the next stamp
  pluto_dma_stop(rx_->dma);
  pluto_rx_reader_reset(&rx_->reader, nullptr);
  anchor_valid_ = false;
  rx_->restarts++;
  return pluto_dma_start(rx_->dma);
}

int SoapyPlutoRtems::readStream(SoapySDR::Stream *stream, void *const *buffs, const size_t numElems,
                                int &flags, long long &timeNs, const long timeoutUs)
{
  if (!rx_handle(stream))
    return SOAPY_SDR_NOT_SUPPORTED;
  RxStream *r = rx_;
  std::lock_guard<std::mutex> slock(r->mutex);
  const int64_t deadline = monotonic_ns() + int64_t(std::max(0L, timeoutUs)) * 1000;
  for (int attempt = 0;; ++attempt) {
    int has_time = 0;
    uint64_t stamp = 0;
    long left = timeoutUs;
    if (attempt > 0)
      left = long(std::max<int64_t>(0, (deadline - monotonic_ns()) / 1000));
    int n = pluto_rx_read(&r->reader, &r->source, r->fmt, buffs[0], numElems, &has_time, &stamp, left);
    if (n == PLUTO_ERR_TIMEOUT)
      return SOAPY_SDR_TIMEOUT;
    if (n == PLUTO_ERR_FRAMING) {
      SoapySDR_logf(SOAPY_SDR_ERROR, "PlutoSDR: RX block without a valid stamp, restarting the RX DMA");
      if (!r->active || attempt > 0)
        return SOAPY_SDR_TIMEOUT;
      restart_rx_locked();
      continue;
    }
    flags = 0;
    if (has_time) {
      flags |= SOAPY_SDR_HAS_TIME;
      timeNs = SoapySDR::ticksToTimeNs((long long)(stamp + uint64_t(time_offset_ticks_.load())), rate_);
    }
    if (r->reader.track.overflows != r->reported_overflows) {
      // samples were lost (the reader fell behind the ring): the stamp jump carries it, like the
      // gadget, which drops buffers silently; log at most once a second
      int64_t now = monotonic_ns();
      if (now - r->last_overflow_log_ns > 1000000000) {
        SoapySDR_logf(SOAPY_SDR_WARNING, "PlutoSDR: RX overflow, %llu samples lost so far",
                      (unsigned long long)r->reader.track.lost_samples);
        r->last_overflow_log_ns = now;
      }
      r->reported_overflows = r->reader.track.overflows;
    }
    level_account(r, buffs[0], size_t(n), true);
    return n;
  }
}

int SoapyPlutoRtems::writeStream(SoapySDR::Stream *stream, const void *const *buffs, const size_t numElems,
                                 int &flags, const long long timeNs, const long timeoutUs)
{
  if (!tx_handle(stream))
    return SOAPY_SDR_NOT_SUPPORTED;
  TxStream *t = tx_;
  std::lock_guard<std::mutex> slock(t->mutex);
  if (!t->active)
    return SOAPY_SDR_STREAM_ERROR;
  const bool has_time = (flags & SOAPY_SDR_HAS_TIME) != 0;
  uint64_t stamp = 0;
  if (has_time)
    stamp = uint64_t(SoapySDR::timeNsToTicks(timeNs, rate_) - time_offset_ticks_.load());
  int n = pluto_tx_write(&t->writer, &t->sink, t->fmt, buffs[0], numElems, has_time ? 1 : 0, stamp,
                         (flags & SOAPY_SDR_END_BURST) ? 1 : 0, timeoutUs);
  if (n == PLUTO_ERR_TIMEOUT)
    return SOAPY_SDR_TIMEOUT;
  level_account(t, buffs[0], size_t(n), false);
  return n;
}

int SoapyPlutoRtems::readStreamStatus(SoapySDR::Stream *stream, size_t &chanMask, int &flags,
                                      long long &timeNs, const long timeoutUs)
{
  if (!tx_handle(stream))
    return SOAPY_SDR_NOT_SUPPORTED;
  const int64_t deadline = monotonic_ns() + int64_t(std::max(0L, timeoutUs)) * 1000;
  for (;;) {
    {
      std::lock_guard<std::mutex> slock(tx_->mutex);
      uint32_t discarded = pluto_hw_tx_discarded() - tx_->discard_base;
      if (discarded != tx_->discard_reported) {
        // the FPGA dropped late (or more than 8 blocks early) TX blocks
        tx_->discard_reported = discarded;
        chanMask = 1;
        flags = 0;
        timeNs = 0;
        return SOAPY_SDR_TIME_ERROR;
      }
    }
    if (monotonic_ns() >= deadline)
      return SOAPY_SDR_TIMEOUT;
    rtems_task_wake_after(1);
  }
}

// ---- hardware time ----

void SoapyPlutoRtems::log_signal_path(const char *when)
{
  // at every stream start: readbacks proving the signal path settings on the board
  char line[640], tx[384], rx[384];
  pluto_hw_summary(line, sizeof(line));
  pluto_hw_chain_summary(tx, sizeof(tx), rx, sizeof(rx));
  SoapySDR_logf(SOAPY_SDR_INFO, "PlutoSDR %s: %s", when, line);
  SoapySDR_logf(SOAPY_SDR_INFO, "PlutoSDR %s", tx);
  SoapySDR_logf(SOAPY_SDR_INFO, "PlutoSDR %s", rx);
}

void SoapyPlutoRtems::level_account(Stream *s, const void *buf, size_t samples, bool rx)
{
  // caller holds s->mutex; the samples are the ones just copied (cached memory)
  if (samples == 0)
    return;
  pluto_level_add(&s->level, s->fmt, buf, samples);
  int64_t now = monotonic_ns();
  if (now - s->level_since_ns < kLevelPeriodNs)
    return;
  double pk = 0, rms = 0;
  if (pluto_level_dbfs(&s->level, s->fmt, rx ? PLUTO_RX_FULL_SCALE : PLUTO_TX_FULL_SCALE, &pk, &rms) == 0) {
    if (rx)
      SoapySDR_logf(SOAPY_SDR_INFO, "PlutoSDR RX level: peak %.1f dBFS, rms %.1f dBFS over %.1f s; gain %d dB %s",
                    pk, rms, double(now - s->level_since_ns) * 1e-9, rx_gain_db_.load(),
                    gain_auto_ ? "slow_attack" : "manual");
    else {
      // REG 0x05E CH1 overflow bits, read only (an SPI read every 10 s)
      int ov = pluto_hw_tx_overflow();
      char ovs[64];
      if (ov < 0)
        std::snprintf(ovs, sizeof(ovs), "unread (%d)", ov);
      else
        std::snprintf(ovs, sizeof(ovs), "0x%02x fir %d hb1 %d qec %d hb2 %d hb3 %d int3 %d", unsigned(ov),
                      !!(ov & 0x02), !!(ov & 0x04), !!(ov & 0x08), !!(ov & 0x10), !!(ov & 0x20), !!(ov & 0x40));
      SoapySDR_logf(SOAPY_SDR_INFO,
                    "PlutoSDR TX level: peak %.1f dBFS, rms %.1f dBFS over %.1f s; attenuation %u mdB; FIR x%u; "
                    "TX overflow: %s",
                    pk, rms, double(now - s->level_since_ns) * 1e-9, unsigned(tx_atten_mdb_.load()),
                    unsigned(pluto_hw_get_tx_fir_scale()), ovs);
    }
  }
  pluto_level_reset(&s->level);
  s->level_since_ns = now;
}

void SoapyPlutoRtems::rx_block_hook(void *ctx, const uint8_t *block)
{
  auto *self = static_cast<SoapyPlutoRtems *>(ctx);
  RxStream *r = self->rx_;
  if (!r)
    return;
  uint64_t end = pluto_stamp_get(block) + r->geom.data_samples;
  int64_t now = monotonic_ns();
  self->anchor_seq_.fetch_add(1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release); // odd sequence visible before the data
  self->anchor_ticks_ = end;
  self->anchor_ns_ = now;
  self->anchor_seq_.fetch_add(1, std::memory_order_acq_rel);
  self->anchor_valid_ = true;
}

bool SoapyPlutoRtems::hw_now_ticks(uint64_t &ticks) const
{
  if (!anchor_valid_)
    return false;
  uint64_t a;
  int64_t at;
  uint32_t s0, s1;
  do {
    s0 = anchor_seq_.load(std::memory_order_acquire);
    a = anchor_ticks_;
    at = anchor_ns_;
    // seqlock: the data loads must complete before the second sequence load (an acquire load
    // only orders what follows it)
    std::atomic_thread_fence(std::memory_order_acquire);
    s1 = anchor_seq_.load(std::memory_order_relaxed);
  } while ((s0 & 1u) || s0 != s1);
  int64_t age = monotonic_ns() - at;
  if (age < 0 || age > 100000000) // no block for 100 ms: the stream is not running
    return false;
  ticks = a + uint64_t(SoapySDR::timeNsToTicks(age, rate_));
  return true;
}

long long SoapyPlutoRtems::getHardwareTime(const std::string &) const
{
  uint64_t ticks;
  if (!hw_now_ticks(ticks))
    throw std::runtime_error("PlutoSDR: hardware time needs a running timestamped RX stream");
  return SoapySDR::ticksToTimeNs((long long)(ticks + uint64_t(time_offset_ticks_.load())), rate_);
}

void SoapyPlutoRtems::setHardwareTime(const long long timeNs, const std::string &)
{
  uint64_t ticks;
  if (!hw_now_ticks(ticks))
    throw std::runtime_error("PlutoSDR: setHardwareTime needs a running timestamped RX stream");
  time_offset_ticks_ = SoapySDR::timeNsToTicks(timeNs, rate_) - (long long)ticks;
}

// ---- antennas, gains ----

void SoapyPlutoRtems::setAntenna(const int direction, const size_t channel, const std::string &name)
{
  check_channel(channel);
  std::lock_guard<std::recursive_mutex> lock(dev_mutex_);
  if (direction == SOAPY_SDR_RX) {
    if (name != "A_BALANCED")
      throw std::runtime_error("PlutoSDR: RX antenna is A_BALANCED");
    pluto_hw_set_rx_port(0);
  } else {
    if (name != "A")
      throw std::runtime_error("PlutoSDR: TX antenna is A");
    pluto_hw_set_tx_port(0);
  }
}

void SoapyPlutoRtems::setGainMode(const int direction, const size_t channel, const bool automatic)
{
  check_channel(channel);
  if (direction != SOAPY_SDR_RX)
    return;
  std::lock_guard<std::recursive_mutex> lock(dev_mutex_);
  if (pluto_hw_set_rx_agc(automatic ? 1 : 0) < 0)
    throw std::runtime_error("PlutoSDR: gain control mode failed");
  gain_auto_ = automatic;
  int mode = pluto_hw_get_rx_agc();
  SoapySDR_logf(SOAPY_SDR_INFO, "PlutoSDR: RX gain control %s (readback %s)", automatic ? "slow_attack" : "manual",
                mode == 0 ? "manual" : mode == 2 ? "slow_attack" : "?");
}

void SoapyPlutoRtems::setGain(const int direction, const size_t channel, const double value)
{
  check_channel(channel);
  if (!std::isfinite(value))
    throw std::runtime_error("PlutoSDR: gain must be finite");
  std::lock_guard<std::recursive_mutex> lock(dev_mutex_);
  // SoapyPlutoSDR: (long long)value written as hardwaregain, i.e. truncated to whole dB; TX
  // hardwaregain = gain - 89 dB (attenuation 0 at 89). Applied at once, also while streaming.
  if (direction == SOAPY_SDR_RX) {
    double g = std::min(kRxGainMax, std::max(0.0, value));
    if (pluto_hw_set_rx_gain(int32_t(g)) < 0)
      throw std::runtime_error("PlutoSDR: RX gain failed");
    int32_t rb = pluto_hw_get_rx_gain();
    rx_gain_db_ = int(rb);
    SoapySDR_logf(SOAPY_SDR_INFO, "PlutoSDR: RX gain %d dB (readback %d dB, %s)", int(int32_t(g)), int(rb),
                  gain_auto_ ? "slow_attack" : "manual");
  } else {
    double g = std::min(kTxGainMax, std::max(0.0, value));
    uint32_t mdb = uint32_t((long long)kTxGainMax - (long long)g) * 1000u;
    if (pluto_hw_set_tx_atten_mdb(mdb) < 0)
      throw std::runtime_error("PlutoSDR: TX attenuation failed");
    uint32_t rb = pluto_hw_get_tx_atten_mdb();
    tx_atten_mdb_ = rb;
    SoapySDR_logf(SOAPY_SDR_INFO, "PlutoSDR: TX gain %lld dB: TX attenuation %u mdB (readback %u mdB)",
                  (long long)g, unsigned(mdb), unsigned(rb));
  }
}

double SoapyPlutoRtems::getGain(const int direction, const size_t channel, const std::string &) const
{
  check_channel(channel);
  std::lock_guard<std::recursive_mutex> lock(dev_mutex_);
  if (direction == SOAPY_SDR_RX)
    return double(pluto_hw_get_rx_gain());
  return kTxGainMax - double(pluto_hw_get_tx_atten_mdb()) / 1000.0;
}

// ---- frequency, rate, bandwidth ----

void SoapyPlutoRtems::setFrequency(const int direction, const size_t channel, const std::string &name,
                                   const double frequency, const SoapySDR::Kwargs &)
{
  check_channel(channel);
  if (name != "RF")
    throw std::runtime_error("PlutoSDR: unknown frequency component " + name);
  if (!std::isfinite(frequency) || frequency < 0)
    throw std::runtime_error("PlutoSDR: invalid frequency");
  std::lock_guard<std::recursive_mutex> lock(dev_mutex_);
  int ret = pluto_hw_set_lo(direction == SOAPY_SDR_TX, uint64_t(frequency));
  if (ret < 0)
    throw std::runtime_error("PlutoSDR: LO " + std::to_string(uint64_t(frequency)) + " Hz failed (" +
                             std::to_string(ret) + ")");
}

double SoapyPlutoRtems::getFrequency(const int direction, const size_t channel, const std::string &) const
{
  check_channel(channel);
  std::lock_guard<std::recursive_mutex> lock(dev_mutex_);
  return double(pluto_hw_get_lo(direction == SOAPY_SDR_TX));
}

void SoapyPlutoRtems::setSampleRate(const int direction, const size_t channel, const double rate)
{
  check_channel(channel);
  if (!std::isfinite(rate) || rate <= 0 || rate > 61440000.0)
    throw std::runtime_error("PlutoSDR: invalid sample rate");
  std::lock_guard<std::recursive_mutex> lock(dev_mutex_);
  if ((rx_ && rx_->active) || (tx_ && tx_->active))
    SoapySDR_logf(SOAPY_SDR_WARNING, "PlutoSDR: sample rate change while streaming");
  int ret = pluto_hw_set_rate(uint32_t(std::llround(rate)));
  if (ret < 0)
    throw std::runtime_error("PlutoSDR: sample rate " + std::to_string(rate) + " failed (" +
                             std::to_string(ret) + ")");
  if (std::fabs(rate - rate_) > 0.5 && ((rx_ && rx_->active) || (tx_ && tx_->active)))
    anchor_valid_ = false;
  rate_ = rate; // RX and TX share the rate (one BBPLL, one sample counter)
  (void)direction;
}

double SoapyPlutoRtems::getSampleRate(const int direction, const size_t channel) const
{
  check_channel(channel);
  std::lock_guard<std::recursive_mutex> lock(dev_mutex_);
  return pluto_hw_get_rate(direction == SOAPY_SDR_TX);
}

void SoapyPlutoRtems::setBandwidth(const int direction, const size_t channel, const double bw)
{
  check_channel(channel);
  if (!std::isfinite(bw) || bw <= 0)
    throw std::runtime_error("PlutoSDR: invalid bandwidth");
  std::lock_guard<std::recursive_mutex> lock(dev_mutex_);
  if (pluto_hw_set_bw(direction == SOAPY_SDR_TX, uint32_t(bw)) < 0)
    throw std::runtime_error("PlutoSDR: bandwidth failed");
}

double SoapyPlutoRtems::getBandwidth(const int direction, const size_t channel) const
{
  check_channel(channel);
  std::lock_guard<std::recursive_mutex> lock(dev_mutex_);
  return double(pluto_hw_get_bw(direction == SOAPY_SDR_TX));
}

// ---- sensors, settings ----

SoapySDR::ArgInfo SoapyPlutoRtems::getSensorInfo(const std::string &key) const
{
  SoapySDR::ArgInfo info;
  if (key == "ad9361-phy_temp0") {
    info.key = key;
    info.name = "AD9363 temperature";
    info.type = SoapySDR::ArgInfo::FLOAT;
    info.value = "0.0";
    info.units = "C";
  }
  return info;
}

std::string SoapyPlutoRtems::readSensor(const std::string &key) const
{
  if (key != "ad9361-phy_temp0")
    return std::string();
  std::lock_guard<std::recursive_mutex> lock(dev_mutex_);
  int32_t mdeg = 0;
  if (pluto_hw_temp_mdeg(&mdeg) < 0)
    return std::string();
  return std::to_string(double(mdeg) / 1000.0);
}

SoapySDR::ArgInfoList SoapyPlutoRtems::getSettingInfo(void) const
{
  SoapySDR::ArgInfoList list;
  SoapySDR::ArgInfo lb;
  lb.key = "loopback";
  lb.name = "AD936x BIST loopback";
  lb.description = "0 off, 1 digital, 2 RF";
  lb.type = SoapySDR::ArgInfo::INT;
  lb.value = "0";
  list.push_back(lb);
  return list;
}

void SoapyPlutoRtems::writeSetting(const std::string &key, const std::string &value)
{
  // SoapyPlutoSDR ignores settings; loopback is accepted here like the device argument
  if (key == "loopback")
    handle_args(SoapySDR::Kwargs{{"loopback", value}}, -1);
  else if (key == "tx_fir_scale")
    handle_args(SoapySDR::Kwargs{{"tx_fir_scale", value}}, -1);
}

std::string SoapyPlutoRtems::readSetting(const std::string &key) const
{
  // diagnostics (SoapyPlutoSDR returns "" for everything)
  if (key == "loopback")
    return std::to_string(loopback_ < 0 ? 0 : loopback_);
  if (key == "ad936x_refclk")
    return std::to_string(refclk_hz_);
  if (key == "tx_fir_scale")
    return std::to_string(pluto_hw_get_tx_fir_scale());
  if (key == "rx_overflows" && rx_)
    return std::to_string(rx_->reader.track.overflows);
  if (key == "rx_lost_samples" && rx_)
    return std::to_string(rx_->reader.track.lost_samples);
  if (key == "rx_misframed" && rx_)
    return std::to_string(rx_->reader.track.misframed + rx_->restarts);
  if ((key == "rx_dma" && rx_) || (key == "tx_dma" && tx_)) {
    pluto_dma_stats s;
    pluto_dma_get_stats(key == "rx_dma" ? rx_->dma : tx_->dma, &s);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "irqs=%llu blocks=%llu starved=%llu underruns=%llu inflight=%u",
                  (unsigned long long)s.irqs, (unsigned long long)s.blocks, (unsigned long long)s.starved,
                  (unsigned long long)s.underruns, unsigned(s.inflight));
    return buf;
  }
  if (key == "tx_late_blocks" && tx_)
    return std::to_string(pluto_hw_tx_discarded() - tx_->discard_base);
  return std::string();
}

// ---- registration ----

static std::vector<SoapySDR::Kwargs> find_pluto_rtems(const SoapySDR::Kwargs &args)
{
  // One radio, on this board. Any uri the configuration names (written for SoapyPlutoSDR on a
  // host: usb:/ip:/local:) is this Pluto; the enumeration result reports it as local:.
  if (args.count("uri") && args.at("uri") != "local:")
    SoapySDR_logf(SOAPY_SDR_DEBUG, "PlutoSDR: uri %s served by the on-board radio", args.at("uri").c_str());
  SoapySDR::Kwargs dev;
  dev["device"] = "PlutoSDR";
  dev["uri"] = "local:";
  dev["label"] = "PlutoSDR #0 local:";
  return {dev};
}

static SoapySDR::Device *make_pluto_rtems(const SoapySDR::Kwargs &args)
{
  return new SoapyPlutoRtems(args);
}

static SoapySDR::Registry register_plutosdr("plutosdr", &find_pluto_rtems, &make_pluto_rtems,
                                            SOAPY_SDR_ABI_VERSION);

// Referenced by nothing; a symbol the image can name with -u to keep this object without
// --whole-archive: -Wl,-u,soapy_pluto_rtems_registered
extern "C" {
int soapy_pluto_rtems_registered = 1;
}
