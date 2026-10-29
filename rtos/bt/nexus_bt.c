/*
 * Nexus-BS RTOS Bluetooth manager (see nexus_bt.h): BTstack bring-up on the AIC8800 HCI
 * transport, GAP (power, inquiry with names, pairing), A2DP source + AVRCP target, the media
 * clock feeding the SBC encoder from bt_audio, the known-device files in /data/bt, automatic
 * reconnection and the `bt` shell command.
 *
 * Every BTstack call runs on the Bluetooth thread. Shell commands are handed over with
 * btstack_run_loop_execute_on_main_thread() and wait for the result text, which the shell
 * thread prints (stdout is per thread: a command run over SSH prints there). Asynchronous
 * progress (connected, stream started, link lost) goes to the console as "bt: ..." lines.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "nexus_bt.h"

#include "bt_audio.h"
#include "btstack_defines.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_tlv.h"
#include "btstack_util.h"
#include "classic/a2dp_source.h"
#include "classic/avdtp.h"
#include "classic/avdtp_util.h"
#include "classic/avrcp.h"
#include "classic/avrcp_target.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "gap.h"
#include "hci.h"
#include "l2cap.h"
#include "btstack_run_loop_rtems.h"
#include "btstack_tlv_posix.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "classic/btstack_sbc_bluedroid.h"
#include "hci_dump.h"
#include "hci_transport_aicwf.h"

#include "aicwf_wifi.h"
#include "config_store.h"

#include <rtems/shell.h>

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- tunables ---- */

/* NEXUS_DATA_DIR "/bt": devices.txt (address|class|name), last (last sink), keys-<controller>.tlv */
static char bt_dir[96], devices_path[128], last_path[128];
#define BT_DATA_DIR bt_dir
#define BT_DEVICES_FILE devices_path
#define BT_LAST_FILE last_path

/* POSIX SCHED_FIFO priorities: above the shell (classic 200 = POSIX 55), below the AIC driver
 * receive workers (POSIX 135) and libbsd; CPU0 only (CPU1 is the radio core) */
#define BT_CPU 0
#define BT_LOOP_PRIO 125
#define BT_READER_PRIO 130
#define BT_TONE_PRIO 60

#define MAX_KNOWN 16
#define MAX_FOUND 24
#define MAX_NAME_REQUESTS 10
#define POWER_RETRY_MS 3000
#define WATCH_MS 1000
#define RECONNECT_MIN_S 15
#define RECONNECT_MAX_S 300
#define AUDIO_TICK_MS 10
#define PREBUFFER_MS 160   /* > BT_AUDIO_TARGET_MS: start sending with the steady-state level */
#define IDLE_STOP_MS 300   /* silence after the producer stops, then nothing is sent */
#define SBC_STORAGE 1030
#define NEXUS_SBC_MAX_BITPOOL 53

/* ---- state (Bluetooth thread unless noted) ---- */

typedef struct {
  bd_addr_t addr;
  uint32_t cod;
  char name[64];
} known_t;

typedef struct {
  bd_addr_t addr;
  uint32_t cod;
  int8_t rssi;
  bool have_rssi;
  uint8_t psrm;
  uint16_t clock_offset;
  char name[64];
} found_t;

static known_t known[MAX_KNOWN];
static int known_n;
static bd_addr_t last_sink;
static bool have_last;

static found_t found[MAX_FOUND];
static int found_n;
static bool scanning;
static int name_next; /* next found[] index for a remote name request, -1 = none running */
static int name_requests;

static bool want_on;
static bool autoconnect = true;
static bool afh_enabled = true;
static unsigned sbc_bitpool = NEXUS_SBC_MAX_BITPOOL;
static bd_addr_t local_addr;
static char tlv_path[160];
static const btstack_tlv_t *tlv_impl;
static btstack_tlv_posix_t tlv_ctx;

static btstack_timer_source_t power_timer, watch_timer, reconnect_timer;
static unsigned reconnect_s;
static bool reconnect_armed;
static bool user_disconnect; /* no automatic reconnect until the next connect/boot */
static int afh_channel;       /* Wi-Fi 2.4 GHz channel excluded from hopping, 0 = none */

static btstack_packet_callback_registration_t hci_cb;

/* A2DP / AVRCP */
static struct {
  uint16_t a2dp_cid;
  uint8_t local_seid;
  uint8_t remote_seid;
  uint16_t avrcp_cid;
  hci_con_handle_t con;
  bd_addr_t addr;
  bool signaling;
  bool stream_open;
  bool started;
  bool establishing;
  bool user_paused; /* headset pause/stop button: no restart on station audio */
  /* media clock */
  btstack_timer_source_t timer;
  uint32_t time_sent_ms;
  uint32_t acc_missed;
  uint32_t samples_ready;
  uint32_t rtp_timestamp;
  bool flowing;
  uint32_t idle_since_ms;
  uint32_t silent_frames;
  int max_payload;
  uint8_t storage[SBC_STORAGE];
  uint16_t storage_count;
  bool ready_to_send;
  /* codec */
  int rate;
  int channels;
  int block_length, subbands, min_bitpool, max_bitpool;
  btstack_sbc_channel_mode_t channel_mode;
  btstack_sbc_allocation_method_t allocation;
  uint32_t packets_sent;
} media;

static uint8_t sbc_caps[] = {
    (AVDTP_SBC_44100 << 4) | (AVDTP_SBC_48000 << 4) | AVDTP_SBC_STEREO | AVDTP_SBC_JOINT_STEREO,
    0xFF, /* all block lengths, subbands, allocation methods */
    2, NEXUS_SBC_MAX_BITPOOL};
static uint8_t sbc_config[4];
static const btstack_sbc_encoder_t *sbc_enc;
static btstack_sbc_encoder_bluedroid_t sbc_enc_state;

static uint8_t sdp_a2dp_source[150];
static uint8_t sdp_avrcp_target[200];

static avrcp_track_t track = {
    {0, 0, 0, 0, 0, 0, 0, 1}, 1, "Nexus-BS", "Radio", "Nexus-BS", "radio", 0xffffffff, 0xffffffff};
static avrcp_playback_status_t play_status = AVRCP_PLAYBACK_STATUS_STOPPED;

/* cross-thread */
static atomic_bool stream_ready; /* station writes are accepted */
static atomic_bool tone_on;      /* the tone thread owns the producer side */
static atomic_bool tone_req;
static atomic_int writers;
static atomic_bool started;
static pthread_mutex_t tone_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tone_cond = PTHREAD_COND_INITIALIZER;

/* shell requests */
enum { OP_ON = 1, OP_OFF, OP_STATUS, OP_SCAN, OP_PAIR, OP_CONNECT, OP_DISCONNECT, OP_DEVICES, OP_FORGET, OP_TONE, OP_STOP };
static struct {
  int op;
  bd_addr_t addr;
  bool has_addr;
  int arg;
  unsigned seq;
} req;
static btstack_context_callback_registration_t req_cb;
static pthread_mutex_t call_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t done_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done_cond = PTHREAD_COND_INITIALIZER;
static unsigned req_counter, done_seq;
static int pending_op;
static unsigned pending_seq;
static char out[4096];
static size_t out_len;

/* ---- helpers ---- */

static void logf_bt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_bt(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  printf("bt: ");
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
}

static void outf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void outf(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  if (out_len < sizeof(out) - 1) {
    int n = vsnprintf(out + out_len, sizeof(out) - out_len, fmt, ap);
    if (n > 0) {
      out_len += (size_t)n < sizeof(out) - out_len ? (size_t)n : sizeof(out) - out_len - 1;
    }
  }
  va_end(ap);
}

/* the shell request op (if still the one waiting) is finished */
static void finish(int op)
{
  if (pending_op != op) {
    return;
  }
  pending_op = 0;
  pthread_mutex_lock(&done_lock);
  done_seq = pending_seq;
  pthread_cond_broadcast(&done_cond);
  pthread_mutex_unlock(&done_lock);
}

static bool cfg_bool(const char *key, bool def)
{
  const char *v = nexus_config_get(key);
  if (v == NULL || v[0] == '\0') {
    return def;
  }
  return strcmp(v, "1") == 0 || strcasecmp(v, "on") == 0 || strcasecmp(v, "yes") == 0 ||
         strcasecmp(v, "true") == 0;
}

static const char *known_name(const bd_addr_t a)
{
  for (int i = 0; i < known_n; ++i) {
    if (bd_addr_cmp(known[i].addr, a) == 0) {
      return known[i].name;
    }
  }
  for (int i = 0; i < found_n; ++i) {
    if (bd_addr_cmp(found[i].addr, a) == 0 && found[i].name[0]) {
      return found[i].name;
    }
  }
  return "";
}

static bool has_link_key(const bd_addr_t a)
{
  btstack_link_key_iterator_t it;
  bd_addr_t ka;
  link_key_t key;
  link_key_type_t type;
  bool hit = false;
  if (!gap_link_key_iterator_init(&it)) {
    return false;
  }
  while (gap_link_key_iterator_get_next(&it, ka, key, &type)) {
    if (bd_addr_cmp(ka, a) == 0) {
      hit = true;
      break;
    }
  }
  gap_link_key_iterator_done(&it);
  return hit;
}

static void sanitize_name(char *dst, size_t cap, const uint8_t *src, size_t len)
{
  size_t j = 0;
  for (size_t i = 0; i < len && src[i] && j + 1 < cap; ++i) {
    uint8_t c = src[i];
    dst[j++] = (c < 0x20 || c == 0x7f || c == '|') ? '?' : (char)c; /* UTF-8 bytes kept */
  }
  dst[j] = '\0';
}

/* ---- /data/bt files (plain text, atomic replace) ---- */

static void ensure_dir(void)
{
  (void)mkdir(BT_DATA_DIR, 0700);
}

static int write_atomic(const char *path, const char *text)
{
  char tmp[160];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "w");
  if (f == NULL) {
    return -1;
  }
  int ok = fputs(text, f) >= 0;
  ok = fflush(f) == 0 && ok;
  ok = fsync(fileno(f)) == 0 && ok;
  ok = fclose(f) == 0 && ok;
  if (!ok || rename(tmp, path) != 0) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

static void save_known(void)
{
  static char text[MAX_KNOWN * 96 + 64];
  size_t n = (size_t)snprintf(text, sizeof(text), "# address|class|name\n");
  for (int i = 0; i < known_n && n < sizeof(text); ++i) {
    n += (size_t)snprintf(text + n, sizeof(text) - n, "%s|%06x|%s\n", bd_addr_to_str(known[i].addr),
                          (unsigned)known[i].cod, known[i].name);
  }
  ensure_dir();
  if (write_atomic(BT_DEVICES_FILE, text) != 0) {
    logf_bt("cannot write %s", BT_DEVICES_FILE);
  }
}

static void save_last(void)
{
  ensure_dir();
  if (!have_last) {
    unlink(BT_LAST_FILE);
    return;
  }
  char line[32];
  snprintf(line, sizeof(line), "%s\n", bd_addr_to_str(last_sink));
  if (write_atomic(BT_LAST_FILE, line) != 0) {
    logf_bt("cannot write %s", BT_LAST_FILE);
  }
}

static void load_files(void)
{
  char line[160];
  FILE *f = fopen(BT_DEVICES_FILE, "r");
  known_n = 0;
  if (f != NULL) {
    while (known_n < MAX_KNOWN && fgets(line, sizeof(line), f) != NULL) {
      if (line[0] == '#') {
        continue;
      }
      line[strcspn(line, "\r\n")] = '\0';
      char *p1 = strchr(line, '|');
      char *p2 = p1 ? strchr(p1 + 1, '|') : NULL;
      if (p2 == NULL) {
        continue;
      }
      *p1 = *p2 = '\0';
      known_t *k = &known[known_n];
      if (!sscanf_bd_addr(line, k->addr)) {
        continue;
      }
      k->cod = (uint32_t)strtoul(p1 + 1, NULL, 16);
      snprintf(k->name, sizeof(k->name), "%s", p2 + 1);
      known_n++;
    }
    fclose(f);
  }
  have_last = false;
  f = fopen(BT_LAST_FILE, "r");
  if (f != NULL) {
    if (fgets(line, sizeof(line), f) != NULL) {
      line[strcspn(line, "\r\n")] = '\0';
      have_last = sscanf_bd_addr(line, last_sink) != 0;
    }
    fclose(f);
  }
}

static void remember(const bd_addr_t a, uint32_t cod, const char *name)
{
  char nm[64]; /* name may point into known[] */
  snprintf(nm, sizeof(nm), "%s", name ? name : "");
  int i;
  for (i = 0; i < known_n; ++i) {
    if (bd_addr_cmp(known[i].addr, a) == 0) {
      break;
    }
  }
  if (i == known_n) {
    if (known_n == MAX_KNOWN) {
      memmove(&known[0], &known[1], sizeof(known[0]) * (MAX_KNOWN - 1)); /* drop the oldest */
      i = MAX_KNOWN - 1;
    } else {
      known_n++;
    }
    memset(&known[i], 0, sizeof(known[i]));
    bd_addr_copy(known[i].addr, a);
  }
  if (cod) {
    known[i].cod = cod;
  }
  if (nm[0]) {
    memcpy(known[i].name, nm, sizeof(nm));
  }
  save_known();
}

static void forget_known(const bd_addr_t a)
{
  for (int i = 0; i < known_n; ++i) {
    if (bd_addr_cmp(known[i].addr, a) == 0) {
      memmove(&known[i], &known[i + 1], sizeof(known[0]) * (size_t)(known_n - i - 1));
      known_n--;
      save_known();
      break;
    }
  }
  if (have_last && bd_addr_cmp(last_sink, a) == 0) {
    have_last = false;
    save_last();
  }
}

/* ---- Wi-Fi coexistence: AFH host channel classification ---- */

static const hci_cmd_t hci_set_afh_host_channel_classification = {
    HCI_OPCODE(OGF_CONTROLLER_BASEBAND, 0x3f), "1111111111"};

/* 2.4 GHz Wi-Fi channel the AIC stick is on, 0 when not associated or on 5 GHz */
static int wifi_24_channel(void)
{
  struct aicwf_wifi_status ws;
  memset(&ws, 0, sizeof(ws));
  if (aicwf_wifi_status(&ws) != 0 || !ws.connected) {
    return 0;
  }
  return (ws.channel >= 1 && ws.channel <= 14) ? ws.channel : 0;
}

/* exclude the Wi-Fi channel (+-11 MHz around its centre, plus a 2 MHz guard) from hopping */
static void afh_update(void)
{
  int ch = afh_enabled ? wifi_24_channel() : 0;
  if (ch == afh_channel || hci_get_state() != HCI_STATE_WORKING || !hci_can_send_command_packet_now()) {
    return;
  }
  uint8_t map[10];
  memset(map, 0xff, sizeof(map));
  map[9] = 0x7f; /* bit 79 reserved */
  if (ch) {
    int centre = ch == 14 ? 2484 : 2407 + 5 * ch;
    for (int k = 0; k < 79; ++k) {
      int f = 2402 + k;
      if (f >= centre - 13 && f <= centre + 13) {
        map[k / 8] &= (uint8_t)~(1u << (k % 8));
      }
    }
  }
  hci_send_cmd(&hci_set_afh_host_channel_classification, map[0], map[1], map[2], map[3], map[4], map[5], map[6],
               map[7], map[8], map[9]);
  afh_channel = ch;
  if (ch) {
    logf_bt("AFH: Wi-Fi channel %d kept out of Bluetooth hopping", ch);
  }
}

/* ---- media: SBC from bt_audio at the A2DP clock ---- */

static void media_send_packet(void)
{
  int frame_bytes = sbc_enc->sbc_buffer_length(&sbc_enc_state);
  int frames = media.storage_count / frame_bytes;
  media.storage[0] = (uint8_t)frames; /* SBC media payload header: number of frames */
  a2dp_source_stream_send_media_payload_rtp(media.a2dp_cid, media.local_seid, 0, media.rtp_timestamp, media.storage,
                                            media.storage_count + 1);
  media.rtp_timestamp += (uint32_t)frames * sbc_enc->num_audio_frames(&sbc_enc_state);
  media.storage_count = 0;
  media.ready_to_send = false;
  media.packets_sent++;
}

static void media_fill(void)
{
  unsigned per = sbc_enc->num_audio_frames(&sbc_enc_state);
  uint16_t frame_bytes = sbc_enc->sbc_buffer_length(&sbc_enc_state);
  static int16_t pcm[256 * 2];
  while (media.samples_ready >= per && (media.max_payload - media.storage_count) >= frame_bytes) {
    unsigned got = bt_audio_pull(pcm, per, (unsigned)media.rate, (unsigned)media.channels);
    media.silent_frames = got ? 0 : media.silent_frames + per;
    sbc_enc->encode_signed_16(&sbc_enc_state, pcm, &media.storage[1 + media.storage_count]);
    media.storage_count += sbc_enc->sbc_buffer_length(&sbc_enc_state);
    media.samples_ready -= per;
    if (media.silent_frames >= (uint32_t)media.rate * IDLE_STOP_MS / 1000u) {
      /* the producer stopped: send nothing (no silence streaming) until it is back */
      media.flowing = false;
      media.idle_since_ms = btstack_run_loop_get_time_ms();
      media.storage_count = 0;
      media.samples_ready = 0;
      return;
    }
  }
}

static void media_tick(btstack_timer_source_t *ts)
{
  btstack_run_loop_set_timer(ts, AUDIO_TICK_MS);
  btstack_run_loop_add_timer(ts);
  uint32_t now = btstack_run_loop_get_time_ms();
  if (!media.flowing) {
    if (bt_audio_level_ms() < PREBUFFER_MS) {
      return;
    }
    media.flowing = true;
    media.silent_frames = 0;
    media.samples_ready = 0;
    media.acc_missed = 0;
    media.time_sent_ms = now;
    if (media.idle_since_ms) { /* the RTP clock kept running while nothing was sent */
      media.rtp_timestamp += (uint32_t)((uint64_t)(now - media.idle_since_ms) * (uint32_t)media.rate / 1000u);
    }
    return;
  }
  uint32_t period = now - media.time_sent_ms;
  if (period > 100) {
    period = 100; /* the loop was held up: do not burst more than 100 ms */
  }
  media.time_sent_ms = now;
  uint32_t n = period * (uint32_t)media.rate;
  media.samples_ready += n / 1000u;
  media.acc_missed += n % 1000u;
  while (media.acc_missed >= 1000) {
    media.samples_ready++;
    media.acc_missed -= 1000;
  }
  if (media.ready_to_send) {
    return;
  }
  media_fill();
  if (media.flowing && media.storage_count + sbc_enc->sbc_buffer_length(&sbc_enc_state) > media.max_payload) {
    media.ready_to_send = true;
    a2dp_source_stream_endpoint_request_can_send_now(media.a2dp_cid, media.local_seid);
  }
}

static void media_start(void)
{
  media.max_payload = btstack_min(a2dp_max_media_payload_size(media.a2dp_cid, media.local_seid), SBC_STORAGE);
  media.storage_count = 0;
  media.ready_to_send = false;
  media.flowing = false;
  media.idle_since_ms = 0;
  bt_audio_flush();
  btstack_run_loop_remove_timer(&media.timer);
  btstack_run_loop_set_timer_handler(&media.timer, media_tick);
  btstack_run_loop_set_timer(&media.timer, AUDIO_TICK_MS);
  btstack_run_loop_add_timer(&media.timer);
}

static void media_stop(void)
{
  btstack_run_loop_remove_timer(&media.timer);
  media.flowing = false;
  media.storage_count = 0;
  media.ready_to_send = false;
  media.samples_ready = 0;
}

/* ---- connection management ---- */

static void reconnect_cancel(void)
{
  btstack_run_loop_remove_timer(&reconnect_timer);
  reconnect_armed = false;
}

static void reconnect_fire(btstack_timer_source_t *ts);

static void reconnect_schedule(void)
{
  if (!want_on || !autoconnect || user_disconnect || !have_last || media.signaling || media.establishing) {
    return;
  }
  reconnect_s = reconnect_s ? (reconnect_s * 2 > RECONNECT_MAX_S ? RECONNECT_MAX_S : reconnect_s * 2) : RECONNECT_MIN_S;
  btstack_run_loop_remove_timer(&reconnect_timer);
  btstack_run_loop_set_timer_handler(&reconnect_timer, reconnect_fire);
  btstack_run_loop_set_timer(&reconnect_timer, reconnect_s * 1000u);
  btstack_run_loop_add_timer(&reconnect_timer);
  reconnect_armed = true;
  logf_bt("reconnect to %s in %u s", bd_addr_to_str(last_sink), reconnect_s);
}

static uint8_t connect_sink(const bd_addr_t a)
{
  bd_addr_t addr;
  bd_addr_copy(addr, a);
  afh_update();
  uint8_t st = a2dp_source_establish_stream(addr, &media.a2dp_cid);
  if (st == ERROR_CODE_SUCCESS) {
    media.establishing = true;
    bd_addr_copy(media.addr, a);
  }
  return st;
}

static void reconnect_fire(btstack_timer_source_t *ts)
{
  (void)ts;
  reconnect_armed = false;
  if (!want_on || hci_get_state() != HCI_STATE_WORKING || media.signaling || media.establishing || !have_last) {
    return;
  }
  logf_bt("reconnecting %s %s", bd_addr_to_str(last_sink), known_name(last_sink));
  if (connect_sink(last_sink) != ERROR_CODE_SUCCESS) {
    reconnect_schedule();
  }
}

static void disconnect_all(void)
{
  if (media.avrcp_cid) {
    avrcp_disconnect(media.avrcp_cid);
  }
  if (media.a2dp_cid && (media.signaling || media.establishing)) {
    a2dp_source_disconnect(media.a2dp_cid);
  }
}

/* ---- power ---- */

static bool power_waiting; /* a power-on failed: retries are quiet until the controller is up */

static void power_try(btstack_timer_source_t *ts)
{
  (void)ts;
  if (!want_on || hci_get_state() != HCI_STATE_OFF) {
    return;
  }
  /* stick not attached (yet), or the DC variant without Bluetooth: BTSTACK_EVENT_POWERON_FAILED
   * re-arms the timer; the failure was reported once */
  if (power_waiting) {
    hci_dump_enable_log_level(HCI_DUMP_LOG_LEVEL_ERROR, 0);
  }
  hci_power_control(HCI_POWER_ON);
  hci_dump_enable_log_level(HCI_DUMP_LOG_LEVEL_ERROR, 1);
}

static void power_retry_later(void)
{
  btstack_run_loop_remove_timer(&power_timer);
  btstack_run_loop_set_timer_handler(&power_timer, power_try);
  btstack_run_loop_set_timer(&power_timer, POWER_RETRY_MS);
  btstack_run_loop_add_timer(&power_timer);
}

/* every second: a transport that failed under us (stick unplugged) is restarted, the Wi-Fi
 * channel is kept out of hopping, a suspended stream with audio waiting is restarted */
static void watch(btstack_timer_source_t *ts)
{
  btstack_run_loop_set_timer(ts, WATCH_MS);
  btstack_run_loop_add_timer(ts);
  struct hci_transport_aicwf_stats st;
  hci_transport_aicwf_get_stats(&st);
  if (st.failed && hci_get_state() == HCI_STATE_WORKING) {
    logf_bt("controller lost, restarting Bluetooth");
    hci_power_control(HCI_POWER_OFF);
    if (want_on) {
      power_retry_later();
    }
    return;
  }
  if (hci_get_state() == HCI_STATE_WORKING) {
    afh_update();
  }
  if (media.stream_open && !media.started && !media.user_paused &&
      (atomic_load(&tone_on) || bt_audio_level_ms() > 0)) {
    a2dp_source_start_stream(media.a2dp_cid, media.local_seid);
  }
}

/* ---- scan ---- */

static void name_request_next(void)
{
  while (name_next >= 0 && name_next < found_n) {
    found_t *f = &found[name_next];
    if (f->name[0] == '\0' && name_requests < MAX_NAME_REQUESTS) {
      name_requests++;
      if (gap_remote_name_request(f->addr, f->psrm, (uint16_t)(f->clock_offset | 0x8000)) == 0) {
        return; /* HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE continues */
      }
    }
    name_next++;
  }
  name_next = -1;
  scanning = false;
  /* scan result */
  outf("%-17s  %-6s  %4s  %s\n", "address", "class", "rssi", "name");
  for (int i = 0; i < found_n; ++i) {
    found_t *f = &found[i];
    char rssi[8] = "";
    if (f->have_rssi) {
      snprintf(rssi, sizeof(rssi), "%d", f->rssi);
    }
    bool audio = (f->cod & 0x1f00) == 0x0400; /* major device class Audio/Video */
    outf("%s  %06x  %4s  %s%s%s\n", bd_addr_to_str(f->addr), (unsigned)f->cod, rssi,
         f->name[0] ? f->name : "(no name)", audio ? "  [audio]" : "", has_link_key(f->addr) ? "  [paired]" : "");
  }
  outf("%d device(s)\n", found_n);
  finish(OP_SCAN);
}

static void scan_result(const uint8_t *packet)
{
  bd_addr_t a;
  gap_event_inquiry_result_get_bd_addr(packet, a);
  found_t *f = NULL;
  for (int i = 0; i < found_n; ++i) {
    if (bd_addr_cmp(found[i].addr, a) == 0) {
      f = &found[i];
    }
  }
  if (f == NULL) {
    if (found_n == MAX_FOUND) {
      return;
    }
    f = &found[found_n++];
    memset(f, 0, sizeof(*f));
    bd_addr_copy(f->addr, a);
  }
  f->cod = gap_event_inquiry_result_get_class_of_device(packet);
  f->psrm = gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
  f->clock_offset = gap_event_inquiry_result_get_clock_offset(packet);
  if (gap_event_inquiry_result_get_rssi_available(packet)) {
    f->have_rssi = true;
    f->rssi = gap_event_inquiry_result_get_rssi(packet);
  }
  if (gap_event_inquiry_result_get_name_available(packet)) {
    sanitize_name(f->name, sizeof(f->name), gap_event_inquiry_result_get_name(packet),
                  gap_event_inquiry_result_get_name_len(packet));
  }
}

/* ---- event handlers ---- */

static void tlv_open(void)
{
  ensure_dir();
  snprintf(tlv_path, sizeof(tlv_path), "%s/keys-%s.tlv", bt_dir, bd_addr_to_str_with_delimiter(local_addr, '-'));
  tlv_impl = btstack_tlv_posix_init_instance(&tlv_ctx, tlv_path);
  btstack_tlv_set_instance(tlv_impl, &tlv_ctx);
  hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv_impl, &tlv_ctx));
}

static void hci_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
  (void)channel;
  (void)size;
  if (packet_type != HCI_EVENT_PACKET) {
    return;
  }
  bd_addr_t a;
  switch (hci_event_packet_get_type(packet)) {
  case BTSTACK_EVENT_STATE: {
    uint8_t s = btstack_event_state_get_state(packet);
    if (s == HCI_STATE_WORKING) {
      power_waiting = false;
      gap_local_bd_addr(local_addr);
      if (tlv_impl == NULL) {
        tlv_open();
      }
      afh_channel = -1; /* resend the classification */
      afh_update();
      logf_bt("on, controller %s", bd_addr_to_str(local_addr));
      if (pending_op == OP_ON) {
        outf("on, controller %s\n", bd_addr_to_str(local_addr));
        finish(OP_ON);
      }
      reconnect_s = 0;
      user_disconnect = false;
      if (autoconnect && have_last && !media.signaling) {
        logf_bt("connecting last sink %s %s", bd_addr_to_str(last_sink), known_name(last_sink));
        if (connect_sink(last_sink) != ERROR_CODE_SUCCESS) {
          reconnect_schedule();
        }
      }
    } else if (s == HCI_STATE_OFF) {
      atomic_store(&tone_req, false);
      media_stop();
      atomic_store(&stream_ready, false);
      media.signaling = media.stream_open = media.started = media.establishing = false;
      media.a2dp_cid = media.avrcp_cid = 0;
      reconnect_cancel();
      if (tlv_impl != NULL) {
        btstack_tlv_posix_deinit(&tlv_ctx);
        tlv_impl = NULL;
      }
      logf_bt("off");
      if (pending_op == OP_OFF) {
        outf("off\n");
        finish(OP_OFF);
      }
    }
    break;
  }
  case BTSTACK_EVENT_POWERON_FAILED:
    if (!power_waiting) {
      logf_bt("waiting for the Bluetooth controller (AIC8800 D80 stick), retrying every %d s", POWER_RETRY_MS / 1000);
    }
    power_waiting = true;
    if (pending_op == OP_ON) {
      outf("no Bluetooth controller (AIC8800 D80 stick not attached or not up); retrying every %d s\n",
           POWER_RETRY_MS / 1000);
      finish(OP_ON);
    }
    if (want_on) {
      power_retry_later();
    }
    break;
  case HCI_EVENT_PIN_CODE_REQUEST:
    /* legacy pairing (pre-2.1 devices): the usual fixed PIN */
    hci_event_pin_code_request_get_bd_addr(packet, a);
    logf_bt("legacy pairing with %s: PIN 0000", bd_addr_to_str(a));
    gap_pin_code_response(a, "0000");
    break;
  case HCI_EVENT_USER_CONFIRMATION_REQUEST:
    /* SSP Just Works: no display, no keyboard */
    hci_event_user_confirmation_request_get_bd_addr(packet, a);
    gap_ssp_confirmation_response(a);
    break;
  case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
    hci_event_simple_pairing_complete_get_bd_addr(packet, a);
    logf_bt("pairing with %s: status 0x%02x", bd_addr_to_str(a), hci_event_simple_pairing_complete_get_status(packet));
    break;
  case GAP_EVENT_DEDICATED_BONDING_COMPLETED: {
    uint8_t st = gap_event_dedicated_bonding_completed_get_status(packet);
    gap_event_dedicated_bonding_completed_get_address(packet, a);
    if (st == ERROR_CODE_SUCCESS) {
      uint32_t cod = 0;
      for (int i = 0; i < found_n; ++i) {
        if (bd_addr_cmp(found[i].addr, a) == 0) {
          cod = found[i].cod;
        }
      }
      remember(a, cod, known_name(a));
      logf_bt("paired %s %s", bd_addr_to_str(a), known_name(a));
    }
    if (pending_op == OP_PAIR) {
      if (st == ERROR_CODE_SUCCESS) {
        outf("paired %s %s\n", bd_addr_to_str(a), known_name(a));
      } else {
        outf("pairing with %s failed: status 0x%02x\n", bd_addr_to_str(a), st);
      }
      finish(OP_PAIR);
    }
    break;
  }
  case GAP_EVENT_INQUIRY_RESULT:
    scan_result(packet);
    break;
  case GAP_EVENT_INQUIRY_COMPLETE:
    if (scanning) {
      name_next = 0;
      name_requests = 0;
      name_request_next();
    }
    break;
  case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
    hci_event_remote_name_request_complete_get_bd_addr(packet, a);
    if (hci_event_remote_name_request_complete_get_status(packet) == 0) {
      const char *nm = hci_event_remote_name_request_complete_get_remote_name(packet);
      for (int i = 0; i < found_n; ++i) {
        if (bd_addr_cmp(found[i].addr, a) == 0) {
          sanitize_name(found[i].name, sizeof(found[i].name), (const uint8_t *)nm, 248);
        }
      }
      for (int i = 0; i < known_n; ++i) {
        if (bd_addr_cmp(known[i].addr, a) == 0 && known[i].name[0] == '\0') {
          sanitize_name(known[i].name, sizeof(known[i].name), (const uint8_t *)nm, 248);
          save_known();
        }
      }
    }
    if (name_next >= 0) {
      name_next++;
      name_request_next();
    }
    break;
  default:
    break;
  }
}

static void a2dp_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
  (void)channel;
  (void)size;
  if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) {
    return;
  }
  bd_addr_t a;
  uint8_t st;
  switch (hci_event_a2dp_meta_get_subevent_code(packet)) {
  case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
    a2dp_subevent_signaling_connection_established_get_bd_addr(packet, a);
    st = a2dp_subevent_signaling_connection_established_get_status(packet);
    if (st != ERROR_CODE_SUCCESS) {
      logf_bt("A2DP connection to %s failed: status 0x%02x", bd_addr_to_str(a), st);
      media.establishing = false;
      media.a2dp_cid = 0;
      if (pending_op == OP_CONNECT) {
        outf("connection to %s failed: status 0x%02x%s\n", bd_addr_to_str(a), st,
             st == ERROR_CODE_PAGE_TIMEOUT ? " (page timeout: device off, out of range or busy)" : "");
        finish(OP_CONNECT);
      }
      reconnect_schedule();
      break;
    }
    media.a2dp_cid = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
    media.con = a2dp_subevent_signaling_connection_established_get_con_handle(packet);
    media.signaling = true;
    bd_addr_copy(media.addr, a);
    logf_bt("A2DP signaling up with %s %s", bd_addr_to_str(a), known_name(a));
    if (!media.establishing) {
      /* the sink connected to us (it reconnects to its last source): open the stream */
      connect_sink(a);
    }
    if (!media.avrcp_cid) {
      avrcp_connect(a, &media.avrcp_cid);
    }
    break;

  case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION: {
    if (a2dp_subevent_signaling_media_codec_sbc_configuration_get_a2dp_cid(packet) != media.a2dp_cid) {
      break;
    }
    media.remote_seid = a2dp_subevent_signaling_media_codec_sbc_configuration_get_remote_seid(packet);
    media.channels = a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
    media.rate = (int)a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
    media.block_length = a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
    media.subbands = a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
    media.min_bitpool = a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet);
    media.max_bitpool = a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);
    media.allocation = (btstack_sbc_allocation_method_t)(
        a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet) - 1);
    switch ((avdtp_channel_mode_t)a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet)) {
    case AVDTP_CHANNEL_MODE_JOINT_STEREO:
      media.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
      break;
    case AVDTP_CHANNEL_MODE_STEREO:
      media.channel_mode = SBC_CHANNEL_MODE_STEREO;
      break;
    case AVDTP_CHANNEL_MODE_DUAL_CHANNEL:
      media.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
      break;
    default:
      media.channel_mode = SBC_CHANNEL_MODE_MONO;
      break;
    }
    sbc_enc = btstack_sbc_encoder_bluedroid_init_instance(&sbc_enc_state);
    sbc_enc->configure(&sbc_enc_state, SBC_MODE_STANDARD, media.block_length, media.subbands, media.allocation,
                       media.rate, media.max_bitpool, media.channel_mode);
    logf_bt("SBC %d Hz, %d ch, mode %d, %d blocks, %d subbands, bitpool %d..%d", media.rate, media.channels,
            (int)media.channel_mode, media.block_length, media.subbands, media.min_bitpool, media.max_bitpool);
    break;
  }

  case A2DP_SUBEVENT_STREAM_ESTABLISHED:
    a2dp_subevent_stream_established_get_bd_addr(packet, a);
    st = a2dp_subevent_stream_established_get_status(packet);
    media.establishing = false;
    if (st != ERROR_CODE_SUCCESS) {
      logf_bt("A2DP stream to %s failed: status 0x%02x", bd_addr_to_str(a), st);
      if (pending_op == OP_CONNECT) {
        outf("stream to %s failed: status 0x%02x\n", bd_addr_to_str(a), st);
        finish(OP_CONNECT);
      }
      reconnect_schedule();
      break;
    }
    media.local_seid = a2dp_subevent_stream_established_get_local_seid(packet);
    media.stream_open = true;
    media.user_paused = false;
    reconnect_s = 0;
    reconnect_cancel();
    user_disconnect = false;
    bd_addr_copy(last_sink, a);
    have_last = true;
    save_last();
    {
      uint32_t cod = 0;
      for (int i = 0; i < found_n; ++i) {
        if (bd_addr_cmp(found[i].addr, a) == 0) {
          cod = found[i].cod;
        }
      }
      remember(a, cod, known_name(a));
    }
    atomic_store(&stream_ready, true);
    logf_bt("connected %s %s: SBC %d Hz", bd_addr_to_str(a), known_name(a), media.rate);
    if (pending_op == OP_CONNECT) {
      outf("connected %s %s, SBC %d Hz %s, bitpool %d\n", bd_addr_to_str(a), known_name(a), media.rate,
           media.channels == 1 ? "mono" : "stereo", media.max_bitpool);
      finish(OP_CONNECT);
    }
    a2dp_source_start_stream(media.a2dp_cid, media.local_seid);
    break;

  case A2DP_SUBEVENT_STREAM_STARTED:
    media.started = true;
    play_status = AVRCP_PLAYBACK_STATUS_PLAYING;
    if (media.avrcp_cid) {
      avrcp_target_set_playback_status(media.avrcp_cid, play_status);
    }
    media_start();
    break;

  case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
    if (media.started && media.storage_count) {
      media_send_packet();
    } else {
      media.ready_to_send = false;
    }
    break;

  case A2DP_SUBEVENT_STREAM_SUSPENDED:
    media.started = false;
    play_status = AVRCP_PLAYBACK_STATUS_PAUSED;
    if (media.avrcp_cid) {
      avrcp_target_set_playback_status(media.avrcp_cid, play_status);
    }
    media_stop();
    logf_bt("stream suspended by %s", bd_addr_to_str(media.addr));
    break;

  case A2DP_SUBEVENT_STREAM_RELEASED:
    media.started = false;
    media.stream_open = false;
    atomic_store(&stream_ready, false);
    play_status = AVRCP_PLAYBACK_STATUS_STOPPED;
    media_stop();
    break;

  case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
    if (a2dp_subevent_signaling_connection_released_get_a2dp_cid(packet) != media.a2dp_cid) {
      break;
    }
    logf_bt("disconnected %s %s", bd_addr_to_str(media.addr), known_name(media.addr));
    media.signaling = media.stream_open = media.started = media.establishing = false;
    atomic_store(&stream_ready, false);
    media_stop();
    media.a2dp_cid = 0;
    if (pending_op == OP_DISCONNECT) {
      outf("disconnected %s\n", bd_addr_to_str(media.addr));
      finish(OP_DISCONNECT);
    }
    reconnect_schedule();
    break;
  default:
    break;
  }
}

static void avrcp_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
  (void)channel;
  (void)size;
  if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) {
    return;
  }
  switch (packet[2]) {
  case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED:
    if (avrcp_subevent_connection_established_get_status(packet) != ERROR_CODE_SUCCESS) {
      media.avrcp_cid = 0;
      return;
    }
    media.avrcp_cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
    avrcp_target_support_event(media.avrcp_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
    avrcp_target_support_event(media.avrcp_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
    avrcp_target_set_now_playing_info(media.avrcp_cid, &track, 1);
    avrcp_target_set_playback_status(media.avrcp_cid, play_status);
    break;
  case AVRCP_SUBEVENT_CONNECTION_RELEASED:
    media.avrcp_cid = 0;
    break;
  default:
    break;
  }
}

/* headset buttons: play starts the stream, pause/stop suspends it */
static void avrcp_target_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
  (void)channel;
  (void)size;
  if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) {
    return;
  }
  switch (packet[2]) {
  case AVRCP_SUBEVENT_PLAY_STATUS_QUERY:
    avrcp_target_play_status(media.avrcp_cid, 0xffffffff, 0xffffffff, play_status);
    break;
  case AVRCP_SUBEVENT_OPERATION: {
    if (!avrcp_subevent_operation_get_button_pressed(packet) || !media.stream_open) {
      break;
    }
    avrcp_operation_id_t op = (avrcp_operation_id_t)avrcp_subevent_operation_get_operation_id(packet);
    if (op == AVRCP_OPERATION_ID_PLAY) {
      media.user_paused = false;
      atomic_store(&stream_ready, true);
      if (!media.started) {
        a2dp_source_start_stream(media.a2dp_cid, media.local_seid);
      }
    } else if (op == AVRCP_OPERATION_ID_PAUSE || op == AVRCP_OPERATION_ID_STOP) {
      media.user_paused = true;
      atomic_store(&stream_ready, false);
      if (media.started) {
        a2dp_source_pause_stream(media.a2dp_cid, media.local_seid);
      }
    }
    break;
  }
  default:
    break;
  }
}

/* ---- shell requests (Bluetooth thread) ---- */

static void op_status(void)
{
  HCI_STATE s = hci_get_state();
  outf("state      %s%s\n", s == HCI_STATE_WORKING ? "on" : (s == HCI_STATE_OFF ? "off" : "starting"),
       want_on ? (s == HCI_STATE_OFF ? ", enabled: waiting for the controller (AIC8800 D80 stick)" : "")
               : " (disabled)");
  if (s == HCI_STATE_WORKING) {
    outf("controller %s\n", bd_addr_to_str(local_addr));
  }
  if (media.signaling || media.establishing) {
    outf("sink       %s %s: %s\n", bd_addr_to_str(media.addr), known_name(media.addr),
         media.started ? "streaming"
                       : (media.stream_open ? (media.user_paused ? "connected, paused from the headset"
                                                                 : "connected, stream suspended")
                                            : "connecting"));
  } else {
    outf("sink       none\n");
  }
  if (media.stream_open) {
    outf("codec      SBC %d Hz %s, bitpool %d, %d blocks, %d subbands, %u packets\n", media.rate,
         media.channels == 1 ? "mono" : "stereo", media.max_bitpool, media.block_length, media.subbands,
         (unsigned)media.packets_sent);
  }
  struct bt_audio_stats as;
  bt_audio_get_stats(&as);
  outf("audio      %s%s, buffered %u ms, input %u Hz x%u, underrun %llu, overflow %llu, catch-up %llu\n",
       media.flowing ? "flowing" : "idle", atomic_load(&tone_on) ? " (test tone)" : "", bt_audio_level_ms(), as.in_rate,
       as.in_channels, (unsigned long long)as.underrun, (unsigned long long)as.overflow,
       (unsigned long long)as.catchup);
  outf("autoconnect %s, last sink %s%s\n", autoconnect ? "on" : "off", have_last ? bd_addr_to_str(last_sink) : "none",
       reconnect_armed ? " (reconnect pending)" : "");
  int wc = wifi_24_channel();
  if (wc) {
    outf("wifi       2.4 GHz channel %d: Bluetooth audio and 2.4 GHz Wi-Fi share the antenna, expect\n"
         "           2-3x Wi-Fi latency while streaming; 5 GHz avoids it%s\n",
         wc, afh_channel == wc ? " (AFH: channel excluded)" : "");
  }
  struct hci_transport_aicwf_stats ts;
  hci_transport_aicwf_get_stats(&ts);
  outf("hci        rx %u, tx %u, tx errors %u, rx dropped %u, resync %u\n", (unsigned)ts.rx_packets,
       (unsigned)ts.tx_packets, (unsigned)ts.tx_errors, (unsigned)ts.rx_dropped, (unsigned)ts.rx_resync);
}

static void op_devices(void)
{
  int n = 0;
  outf("%-17s  %-6s  %-6s  %s\n", "address", "class", "keys", "name");
  for (int i = 0; i < known_n; ++i) {
    bool last = have_last && bd_addr_cmp(last_sink, known[i].addr) == 0;
    bool conn = (media.signaling || media.establishing) && bd_addr_cmp(media.addr, known[i].addr) == 0;
    outf("%s  %06x  %-6s  %s%s%s\n", bd_addr_to_str(known[i].addr), (unsigned)known[i].cod,
         hci_get_state() == HCI_STATE_WORKING ? (has_link_key(known[i].addr) ? "yes" : "no") : "?", known[i].name,
         last ? "  [last]" : "", conn ? "  [connected]" : "");
    n++;
  }
  /* link keys without a devices.txt line (paired by a sink that connected to us) */
  if (hci_get_state() == HCI_STATE_WORKING) {
    btstack_link_key_iterator_t it;
    bd_addr_t ka;
    link_key_t key;
    link_key_type_t type;
    if (gap_link_key_iterator_init(&it)) {
      while (gap_link_key_iterator_get_next(&it, ka, key, &type)) {
        bool listed = false;
        for (int i = 0; i < known_n; ++i) {
          listed = listed || bd_addr_cmp(known[i].addr, ka) == 0;
        }
        if (!listed) {
          outf("%s  %-6s  %-6s  %s\n", bd_addr_to_str(ka), "", "yes", "(unknown)");
          n++;
        }
      }
      gap_link_key_iterator_done(&it);
    }
  }
  outf("%d device(s)\n", n);
}

static void handle_request(void *ctx)
{
  (void)ctx;
  out_len = 0;
  out[0] = '\0';
  if (pending_op) {
    pending_op = 0; /* the previous command's shell gave up waiting; it completes on its own */
  }
  pending_seq = req.seq;
  pending_op = req.op;
  HCI_STATE s = hci_get_state();
  bool on = s == HCI_STATE_WORKING;

  switch (req.op) {
  case OP_ON:
    want_on = true;
    autoconnect = cfg_bool("bt.autoconnect", true);
    if (on) {
      outf("already on, controller %s\n", bd_addr_to_str(local_addr));
      finish(OP_ON);
    } else if (s == HCI_STATE_OFF) {
      power_try(NULL); /* finishes on BTSTACK_EVENT_STATE or BTSTACK_EVENT_POWERON_FAILED */
    }
    return;
  case OP_OFF:
    want_on = false;
    atomic_store(&tone_req, false);
    btstack_run_loop_remove_timer(&power_timer);
    reconnect_cancel();
    if (s == HCI_STATE_OFF) {
      outf("off\n");
      finish(OP_OFF);
    } else {
      hci_power_control(HCI_POWER_OFF);
    }
    return;
  case OP_STATUS:
    op_status();
    break;
  case OP_DEVICES:
    op_devices();
    break;
  default:
    if (!on) {
      outf("Bluetooth is off (bt on)\n");
      break;
    }
    switch (req.op) {
    case OP_SCAN: {
      if (scanning) {
        outf("a scan is already running\n");
        break;
      }
      int units = (req.arg * 100 + 127) / 128; /* inquiry length in 1.28 s units */
      units = units < 1 ? 1 : (units > 48 ? 48 : units);
      found_n = 0;
      name_next = -1;
      afh_update();
      if (gap_inquiry_start((uint8_t)units) != 0) {
        outf("inquiry not started (busy)\n");
        break;
      }
      scanning = true;
      return; /* finishes after the inquiry and the name requests */
    }
    case OP_PAIR:
      if (gap_dedicated_bonding(req.addr, 0) != 0) {
        outf("pairing not started (busy or already connected)\n");
        break;
      }
      return;
    case OP_CONNECT:
      if (media.signaling || media.establishing) {
        outf("already %s %s (bt disconnect first)\n", media.stream_open ? "connected to" : "connecting to",
             bd_addr_to_str(media.addr));
        break;
      }
      reconnect_cancel();
      user_disconnect = false;
      {
        uint8_t st = connect_sink(req.addr);
        if (st != ERROR_CODE_SUCCESS) {
          outf("connect not started: status 0x%02x\n", st);
          break;
        }
      }
      return; /* finishes on stream established or failure */
    case OP_DISCONNECT:
      user_disconnect = true;
      reconnect_cancel();
      if (!media.signaling && !media.establishing) {
        outf("not connected\n");
        break;
      }
      if (req.has_addr && bd_addr_cmp(req.addr, media.addr) != 0) {
        outf("not connected to %s\n", bd_addr_to_str(req.addr));
        break;
      }
      atomic_store(&tone_req, false);
      disconnect_all();
      return;
    case OP_FORGET:
      if ((media.signaling || media.establishing) && bd_addr_cmp(req.addr, media.addr) == 0) {
        user_disconnect = true;
        disconnect_all();
      }
      gap_drop_link_key_for_bd_addr(req.addr);
      forget_known(req.addr);
      outf("forgot %s\n", bd_addr_to_str(req.addr));
      break;
    case OP_TONE:
      if (!media.stream_open) {
        outf("no sink connected (bt connect <addr>)\n");
        break;
      }
      media.user_paused = false;
      atomic_store(&stream_ready, true);
      pthread_mutex_lock(&tone_lock);
      atomic_store(&tone_req, true);
      pthread_cond_signal(&tone_cond);
      pthread_mutex_unlock(&tone_lock);
      if (!media.started) {
        a2dp_source_start_stream(media.a2dp_cid, media.local_seid);
      }
      outf("1 kHz test tone to %s %s (bt stop ends it)\n", bd_addr_to_str(media.addr), known_name(media.addr));
      break;
    case OP_STOP:
      atomic_store(&tone_req, false);
      outf("test tone stopped\n");
      break;
    default:
      break;
    }
  }
  finish(req.op);
}

/* ---- test tone: a producer like the station's radio audio (16 kHz mono, 20 ms blocks) ---- */

#define TONE_RATE 16000
#define TONE_BLOCK (TONE_RATE / 50)

static void sleep_until(struct timespec *t)
{
#ifdef __APPLE__
  struct timespec now, d;
  clock_gettime(CLOCK_MONOTONIC, &now);
  d.tv_sec = t->tv_sec - now.tv_sec;
  d.tv_nsec = t->tv_nsec - now.tv_nsec;
  if (d.tv_nsec < 0) {
    d.tv_sec--;
    d.tv_nsec += 1000000000L;
  }
  if (d.tv_sec >= 0) {
    nanosleep(&d, NULL);
  }
#else
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, t, NULL) == EINTR) {
  }
#endif
}

static void *tone_main(void *arg)
{
  (void)arg;
  static int16_t block[TONE_BLOCK];
  /* 1 kHz at 16 kHz = 16 samples per period, -12 dBFS */
  static const int16_t period[16] = {0,     3135,  5793,  7568,  8192,  7568,  5793,  3135,
                                     0,     -3135, -5793, -7568, -8192, -7568, -5793, -3135};
  for (int i = 0; i < TONE_BLOCK; ++i) {
    block[i] = period[i % 16];
  }
  for (;;) {
    pthread_mutex_lock(&tone_lock);
    while (!atomic_load(&tone_req)) {
      pthread_cond_wait(&tone_cond, &tone_lock);
    }
    pthread_mutex_unlock(&tone_lock);

    atomic_store(&tone_on, true);
    while (atomic_load(&writers) != 0) { /* a station write in flight: microseconds */
      sched_yield();
    }
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    while (atomic_load(&tone_req)) {
      /* keep the ring at the steady-state level: write a block every 20 ms */
      if (bt_audio_fill_ms() < BT_AUDIO_HIGH_MS - 100) {
        bt_audio_push(block, TONE_BLOCK, TONE_RATE, 1);
      }
      t.tv_nsec += 20 * 1000000L;
      if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
      }
      sleep_until(&t);
    }
    atomic_store(&tone_on, false);
  }
  return NULL;
}

/* ---- public audio API (any thread) ---- */

size_t nexus_bt_audio_write_ch(const int16_t *pcm, size_t frames, unsigned rate, unsigned channels)
{
  if (!atomic_load_explicit(&stream_ready, memory_order_relaxed) || atomic_load(&tone_on)) {
    return 0;
  }
  atomic_fetch_add(&writers, 1);
  size_t n = 0;
  if (!atomic_load(&tone_on)) {
    n = bt_audio_push(pcm, frames, rate, channels);
  }
  atomic_fetch_sub(&writers, 1);
  return n;
}

size_t nexus_bt_audio_write(const int16_t *pcm, size_t frames, unsigned rate)
{
  return nexus_bt_audio_write_ch(pcm, frames, rate, 1);
}

int nexus_bt_audio_active(void)
{
  return atomic_load(&stream_ready) ? 1 : 0;
}

/* ---- Bluetooth thread ---- */

static void log_message(int level, const char *fmt, va_list ap)
{
  (void)level;
  printf("bt: ");
  vprintf(fmt, ap);
  printf("\n");
}

static const hci_dump_t log_impl = {NULL, NULL, log_message};

static void services_init(void)
{
  hci_set_master_slave_policy(0); /* reconnecting headsets: let them be the peripheral */
  hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
  gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
  gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
  gap_set_bondable_mode(1);
  gap_ssp_set_auto_accept(1);
  gap_connectable_control(1); /* paired sinks reconnect to us */
  gap_discoverable_control(0);
  gap_set_class_of_device(0x200408); /* Audio service, Audio/Video major */
  {
    static char name[48]; /* BTstack keeps the pointer */
    char host[32] = "";
    if (gethostname(host, sizeof(host)) != 0 || host[0] == '\0') {
      snprintf(host, sizeof(host), "pluto");
    }
    snprintf(name, sizeof(name), "Nexus-BS %s", host);
    gap_set_local_name(name);
  }

  l2cap_init();
  sdp_init();

  a2dp_source_init();
  a2dp_source_register_packet_handler(&a2dp_handler);
  sbc_caps[3] = (uint8_t)sbc_bitpool;
  avdtp_stream_endpoint_t *ep = a2dp_source_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC, sbc_caps,
                                                                   sizeof(sbc_caps), sbc_config, sizeof(sbc_config));
  if (ep == NULL) {
    logf_bt("no stream endpoint");
    return;
  }
  avdtp_set_preferred_sampling_frequency(ep, 48000);
  avdtp_set_preferred_channel_mode(ep, AVDTP_SBC_JOINT_STEREO);
  media.local_seid = avdtp_local_seid(ep);

  avrcp_init();
  avrcp_register_packet_handler(&avrcp_handler);
  avrcp_target_init();
  avrcp_target_register_packet_handler(&avrcp_target_handler);

  memset(sdp_a2dp_source, 0, sizeof(sdp_a2dp_source));
  a2dp_source_create_sdp_record(sdp_a2dp_source, sdp_create_service_record_handle(), AVDTP_SOURCE_FEATURE_MASK_PLAYER,
                                NULL, NULL);
  sdp_register_service(sdp_a2dp_source);
  memset(sdp_avrcp_target, 0, sizeof(sdp_avrcp_target));
  avrcp_target_create_sdp_record(sdp_avrcp_target, sdp_create_service_record_handle(),
                                 AVRCP_FEATURE_MASK_CATEGORY_PLAYER_OR_RECORDER, NULL, NULL);
  sdp_register_service(sdp_avrcp_target);

  hci_cb.callback = &hci_handler;
  hci_add_event_handler(&hci_cb);
}

static void *bt_main(void *arg)
{
  (void)arg;
  btstack_memory_init();
  btstack_run_loop_init(btstack_run_loop_rtems_get_instance());
  hci_dump_init(&log_impl);
  hci_dump_enable_packet_log(false);
  hci_transport_aicwf_set_thread(BT_READER_PRIO, BT_CPU);
  hci_init(hci_transport_aicwf_instance(), NULL);
  services_init();
  load_files();

  const char *en = nexus_config_get("bt.enabled");
  if (en != NULL && en[0] != '\0') {
    want_on = cfg_bool("bt.enabled", false);
  } else {
    want_on = cfg_bool("bluetooth", false); /* [SYSTEM] bluetooth = on|off */
  }
  autoconnect = cfg_bool("bt.autoconnect", true);
  afh_enabled = cfg_bool("bt.afh", true);

  btstack_run_loop_set_timer_handler(&watch_timer, watch);
  btstack_run_loop_set_timer(&watch_timer, WATCH_MS);
  btstack_run_loop_add_timer(&watch_timer);
  if (want_on) {
    logf_bt("enabled: powering on%s", have_last && autoconnect ? ", then reconnecting the last sink" : "");
    power_try(NULL);
  }
  atomic_store(&started, true);
  btstack_run_loop_execute();
  return NULL;
}

static int spawn(void *(*fn)(void *), int prio, size_t stack)
{
  pthread_attr_t at;
  struct sched_param sp = {.sched_priority = prio};
  pthread_t t;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, stack);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
#ifdef __rtems__ /* host test runs: default scheduling */
  pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&at, SCHED_FIFO);
  pthread_attr_setschedparam(&at, &sp);
#else
  (void)sp;
#endif
#ifdef __rtems__
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  CPU_SET(BT_CPU, &cpus);
  pthread_attr_setaffinity_np(&at, sizeof(cpus), &cpus);
#endif
  int rc = pthread_create(&t, &at, fn, NULL);
  pthread_attr_destroy(&at);
  return rc;
}

void nexus_bt_start(void)
{
  static bool once;
  if (once) {
    return;
  }
  once = true;
  snprintf(bt_dir, sizeof(bt_dir), "%s/bt", NEXUS_DATA_DIR);
  snprintf(devices_path, sizeof(devices_path), "%s/devices.txt", bt_dir);
  snprintf(last_path, sizeof(last_path), "%s/last", bt_dir);
  const char *bp = nexus_config_get("bt.bitpool");
  if (bp != NULL && bp[0] != '\0') {
    long v = strtol(bp, NULL, 10);
    sbc_bitpool = v < 19 ? 19 : (v > NEXUS_SBC_MAX_BITPOOL ? NEXUS_SBC_MAX_BITPOOL : (unsigned)v);
  }
  if (spawn(tone_main, BT_TONE_PRIO, 8 * 1024) != 0 || spawn(bt_main, BT_LOOP_PRIO, 64 * 1024) != 0) {
    printf("bt: threads not started\n");
  }
}

/* ---- shell (shell thread) ---- */

static int bt_call(int op, const bd_addr_t addr, bool has_addr, int arg, int timeout_ms)
{
  if (!atomic_load(&started)) {
    printf("bt: not started yet\n");
    return 1;
  }
  pthread_mutex_lock(&call_lock);
  pthread_mutex_lock(&done_lock);
  unsigned my = ++req_counter;
  req.op = op;
  req.seq = my;
  req.arg = arg;
  req.has_addr = has_addr;
  if (has_addr) {
    memcpy(req.addr, addr, sizeof(bd_addr_t));
  }
  pthread_mutex_unlock(&done_lock);

  req_cb.callback = handle_request;
  req_cb.context = NULL;
  btstack_run_loop_execute_on_main_thread(&req_cb);

  struct timespec dl;
  clock_gettime(CLOCK_REALTIME, &dl);
  dl.tv_sec += timeout_ms / 1000;
  dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (dl.tv_nsec >= 1000000000L) {
    dl.tv_sec++;
    dl.tv_nsec -= 1000000000L;
  }
  int rc = 0;
  pthread_mutex_lock(&done_lock);
  while (done_seq != my && rc != ETIMEDOUT) {
    rc = pthread_cond_timedwait(&done_cond, &done_lock, &dl);
  }
  bool done = done_seq == my;
  static char copy[sizeof(out)];
  if (done) {
    memcpy(copy, out, sizeof(out));
  }
  pthread_mutex_unlock(&done_lock);
  if (done) {
    fputs(copy, stdout);
  } else {
    printf("bt: still in progress, the console shows the result\n");
  }
  pthread_mutex_unlock(&call_lock);
  return done ? 0 : 1;
}

static int parse_addr(const char *s, bd_addr_t a)
{
  if (s == NULL || !sscanf_bd_addr(s, a)) {
    printf("bt: bad address \"%s\" (AA:BB:CC:DD:EE:FF)\n", s ? s : "");
    return -1;
  }
  return 0;
}

static void usage(void)
{
  printf("usage: bt on|off|status|scan [secs]|pair <addr>|connect <addr>|disconnect [addr]|devices|\n"
         "          forget <addr>|play tone|stop\n");
}

static int shell_bt(int argc, char **argv)
{
  bd_addr_t a;
  const char *c = argc >= 2 ? argv[1] : "status";
  if (strcmp(c, "on") == 0) {
    if (nexus_config_set("bt.enabled", "1") != 0) {
      printf("bt: enabled for this boot only (configuration not stored)\n");
    }
    return bt_call(OP_ON, NULL, false, 0, 15000);
  }
  if (strcmp(c, "off") == 0) {
    if (nexus_config_set("bt.enabled", "0") != 0) {
      printf("bt: disabled for this boot only (configuration not stored)\n");
    }
    return bt_call(OP_OFF, NULL, false, 0, 10000);
  }
  if (strcmp(c, "status") == 0) {
    return bt_call(OP_STATUS, NULL, false, 0, 3000);
  }
  if (strcmp(c, "devices") == 0) {
    return bt_call(OP_DEVICES, NULL, false, 0, 3000);
  }
  if (strcmp(c, "scan") == 0) {
    int secs = argc >= 3 ? atoi(argv[2]) : 10;
    secs = secs < 2 ? 2 : (secs > 60 ? 60 : secs);
    printf("bt: scanning %d s (headphones must be in pairing mode; AirPods: lid open, button held)\n", secs);
    fflush(stdout);
    /* inquiry + up to MAX_NAME_REQUESTS name requests of up to ~5 s each */
    return bt_call(OP_SCAN, NULL, false, secs, secs * 1000 + 3000 + MAX_NAME_REQUESTS * 5500);
  }
  if (strcmp(c, "pair") == 0) {
    if (parse_addr(argc >= 3 ? argv[2] : NULL, a) != 0) {
      return 1;
    }
    return bt_call(OP_PAIR, a, true, 0, 40000);
  }
  if (strcmp(c, "connect") == 0) {
    if (parse_addr(argc >= 3 ? argv[2] : NULL, a) != 0) {
      return 1;
    }
    return bt_call(OP_CONNECT, a, true, 0, 40000);
  }
  if (strcmp(c, "disconnect") == 0) {
    bool has = argc >= 3;
    if (has && parse_addr(argv[2], a) != 0) {
      return 1;
    }
    return bt_call(OP_DISCONNECT, has ? a : NULL, has, 0, 10000);
  }
  if (strcmp(c, "forget") == 0) {
    if (parse_addr(argc >= 3 ? argv[2] : NULL, a) != 0) {
      return 1;
    }
    return bt_call(OP_FORGET, a, true, 0, 5000);
  }
  if (strcmp(c, "play") == 0) {
    if (argc < 3 || strcmp(argv[2], "tone") != 0) {
      usage();
      return 1;
    }
    return bt_call(OP_TONE, NULL, false, 0, 5000);
  }
  if (strcmp(c, "stop") == 0) {
    return bt_call(OP_STOP, NULL, false, 0, 3000);
  }
  usage();
  return 1;
}

void nexus_bt_shell_register(void)
{
  rtems_shell_add_cmd("bt", "misc",
                      "bt on|off|status|scan [secs]|pair <addr>|connect <addr>|disconnect [addr]|devices|"
                      "forget <addr>|play tone|stop: Bluetooth audio",
                      shell_bt);
}
