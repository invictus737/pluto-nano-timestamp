/*
 * GET /metrics collectors (metrics.h): computed on request in the HTTP thread (CPU0, low
 * priority). Reuses what exists: RTEMS per-thread CPU time (as nexus_load.c), nexus2's stage
 * table, the AD9363 driver readback, the CherryUSB counters, getifaddrs, the AIC Wi-Fi status
 * and the heap statistics. Nothing periodic is added; the radio threads are never stopped.
 *
 * Cost per request (no stacks): one thread iteration (~100 threads), the stage table copy, a
 * few AD9363 SPI reads under the driver lock (hw=0 skips them), the heap free-list walk. The
 * stack watermarks scan every stack's unused area (megabytes through the shared L2 cache), so
 * they are only computed with full=1 / stacks=1.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE /* pthread_*affinity_np, CPU_SET */
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h> /* optind */

#include <rtems.h>
#include <rtems/malloc.h>
#include <rtems/score/heapimpl.h>
#include <rtems/score/percpu.h>
#include <rtems/score/protectedheap.h>
#include <rtems/score/threadimpl.h>
#include <rtems/score/todimpl.h>
#include <rtems/shell.h>
#include <rtems/stackchk.h>
#include <rtems/version.h>

#include "metrics.h"
#include "netcmd.h"
#include "nexus_station.h"

/* defined in main.c only for the nexus2 station image (NEXUS_RUST_MAIN) */
extern int nexus_station_layout __attribute__((weak));
extern const char *nexus_station_layout_name(int layout) __attribute__((weak));

#include "../libbsd/aic8800/aicwf_wifi.h"

/* ---- build identity (rtos/app/Makefile passes these; "unknown" in other builds) ---- */
#ifndef NEXUS_BUILD_GIT
#define NEXUS_BUILD_GIT "unknown"
#endif
#ifndef NEXUS_BUILD_RUN
#define NEXUS_BUILD_RUN ""
#endif
#ifndef NEXUS_BUILD_NEXUS_REF
#define NEXUS_BUILD_NEXUS_REF ""
#endif
#ifdef NEXUS_USB_CHERRY
#define METRICS_USB_STACK "cherry"
#else
#define METRICS_USB_STACK "libbsd"
#endif

/* ---- optional providers: weak, so the base image (no nexus2, no native SDR driver, libbsd
 * USB) links without them ---- */
extern void metrics_usb_json(jw_t *w) __attribute__((weak));
extern size_t nexus_log_read(uint32_t since, char *buf, size_t cap, uint32_t *next, uint32_t *oldest)
    __attribute__((weak));
extern int aicwf_wifi_status(struct aicwf_wifi_status *status) __attribute__((weak));
/* native AD9363 driver (rtos/sdr/pluto_rtems/pluto_hw.h), linked in station images */
extern int pluto_hw_is_open(void) __attribute__((weak));
extern double pluto_hw_get_rate(int tx) __attribute__((weak));
extern uint64_t pluto_hw_get_lo(int tx) __attribute__((weak));
extern int pluto_hw_get_rx_agc(void) __attribute__((weak));
extern int pluto_hw_get_rx_gain(int32_t *db) __attribute__((weak));
extern uint32_t pluto_hw_get_tx_atten_mdb(void) __attribute__((weak));
extern int pluto_hw_tx_overflow(void) __attribute__((weak));
extern int pluto_hw_temp_mdeg(int32_t *mdeg) __attribute__((weak));
extern uint32_t pluto_hw_tx_discarded(void) __attribute__((weak));
extern uint32_t pluto_hw_get_tx_fir_scale(void) __attribute__((weak));
/* CherryUSB Ethernet adapters (rtos/cherryusb/net/usbnet.h) */
extern int nexus_usbnet_status(char *buf, size_t cap) __attribute__((weak));

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

static double mono_s(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---- shell command capture ---- */

int metrics_shell_capture(int argc, char **argv, char *buf, size_t cap)
{
  if (cap == 0) {
    return -1;
  }
  buf[0] = '\0';
  rtems_shell_cmd_t *cmd = argc > 0 ? rtems_shell_lookup_cmd(argv[0]) : NULL;
  if (cmd == NULL || cmd->command == NULL) {
    return -1;
  }
  /* newlib's stdout is per thread (struct _reent of the executing thread): only this thread's
   * output goes to the buffer */
  FILE *f = fmemopen(buf, cap, "w");
  if (f == NULL) {
    return -1;
  }
  FILE *saved = stdout;
  stdout = f;
  optind = 0; /* libbsd-style commands parse with the global getopt (main.c NX_GETOPT_SAFE) */
  int rc = cmd->command(argc, argv);
  fflush(f);
  stdout = saved;
  long pos = ftell(f);
  fclose(f);
  buf[pos >= 0 && (size_t)pos < cap ? (size_t)pos : cap - 1] = '\0';
  return rc;
}

/* ---- per-thread CPU time (same source as nexus_load.c: no sampling, no hooks) ---- */

#define M_THREADS 192
#define M_NAME 24

typedef struct {
  rtems_id id;
  char name[M_NAME];
  Timestamp_Control used;
  uint8_t idle;
  uint8_t cpu;
} m_thread;

typedef struct {
  Timestamp_Control uptime;
  size_t n;
  m_thread t[M_THREADS];
} m_snap;

static bool snap_visit(Thread_Control *th, void *arg)
{
  m_snap *s = arg;
  if (s->n >= M_THREADS) {
    return true;
  }
  m_thread *t = &s->t[s->n++];
  t->id = th->Object.id;
  (void)_Thread_Get_name(th, t->name, sizeof(t->name));
  t->used = _Thread_Get_CPU_time_used(th);
  t->idle = th->is_idle ? 1 : 0;
  t->cpu = (uint8_t)_Per_CPU_Get_index(_Thread_Get_CPU(th));
  return false;
}

static void snap_take(m_snap *s)
{
  s->n = 0;
  rtems_task_iterate(snap_visit, s);
  _TOD_Get_uptime(&s->uptime);
}

static const m_thread *snap_find(const m_snap *s, rtems_id id)
{
  for (size_t i = 0; i < s->n; ++i) {
    if (s->t[i].id == id) {
      return &s->t[i];
    }
  }
  return NULL;
}

typedef struct {
  rtems_id id;
  char name[M_NAME];
  uint32_t share; /* 0.01 % of one CPU */
  uint8_t cpu;
} m_row;

static int row_cmp(const void *a, const void *b)
{
  const m_row *x = a, *y = b;
  return x->share < y->share ? 1 : x->share > y->share ? -1 : 0;
}

static uint32_t share_of(Timestamp_Control used, Timestamp_Control wall)
{
  if (wall <= 0 || used <= 0) {
    return 0;
  }
  return (uint32_t)(((uint64_t)used * 10000u) / (uint64_t)wall);
}

/* the previous request's snapshot: CPU shares are over the interval between two requests (the
 * poller's period); the first request reports the shares since boot */
static m_snap snap_prev, snap_now;
static m_row rows[M_THREADS];

static const char *aff_text(rtems_id id)
{
  cpu_set_t set;
  CPU_ZERO(&set);
  if (rtems_task_get_affinity(id, sizeof(set), &set) != RTEMS_SUCCESSFUL) {
    return "?";
  }
  int c0 = CPU_ISSET(0, &set), c1 = CPU_ISSET(1, &set);
  return c0 && c1 ? "01" : c1 ? "1" : c0 ? "0" : "-";
}

static int prio_of(rtems_id id)
{
  rtems_id sched;
  rtems_task_priority p = 0;
  if (rtems_task_get_scheduler(id, &sched) != RTEMS_SUCCESSFUL ||
      rtems_task_get_priority(id, sched, &p) != RTEMS_SUCCESSFUL) {
    return -1;
  }
  return (int)p;
}

static void cpu_json(jw_t *w, int top)
{
  snap_take(&snap_now);
  Timestamp_Control wall = snap_now.uptime - snap_prev.uptime;
  uint32_t idle[2] = {0, 0}, idle_boot[2] = {0, 0};
  size_t n = 0;
  for (size_t i = 0; i < snap_now.n; ++i) {
    const m_thread *t = &snap_now.t[i];
    const m_thread *p = snap_find(&snap_prev, t->id);
    uint32_t sh = share_of(t->used - (p != NULL ? p->used : 0), wall);
    if (t->idle) {
      if (t->cpu < 2) {
        idle[t->cpu] += sh;
        idle_boot[t->cpu] += share_of(t->used, snap_now.uptime);
      }
      continue;
    }
    rows[n].id = t->id;
    memcpy(rows[n].name, t->name, M_NAME);
    rows[n].share = sh;
    rows[n].cpu = t->cpu;
    ++n;
  }
  qsort(rows, n, sizeof(rows[0]), row_cmp);
  uint32_t ncpu = rtems_scheduler_get_processor_maximum();

  jw_obj(w, "cpu");
  jw_uint(w, "n", ncpu);
  jw_num(w, "interval_s", (double)wall / 4294967296.0, 3); /* Timestamp_Control: sbintime */
  jw_arr(w, "busy_pct");
  for (uint32_t c = 0; c < ncpu && c < 2; ++c) {
    jw_num(w, NULL, (10000u - (idle[c] > 10000u ? 10000u : idle[c])) / 100.0, 2);
  }
  jw_arr_end(w);
  jw_arr(w, "busy_pct_boot");
  for (uint32_t c = 0; c < ncpu && c < 2; ++c) {
    jw_num(w, NULL, (10000u - (idle_boot[c] > 10000u ? 10000u : idle_boot[c])) / 100.0, 2);
  }
  jw_arr_end(w);
  jw_uint(w, "threads", snap_now.n);
  jw_obj_end(w);

  jw_arr(w, "threads");
  uint32_t rest = 0;
  for (size_t r = 0; r < n; ++r) {
    if ((int)r >= top) {
      rest += rows[r].share;
      continue;
    }
    jw_obj(w, NULL);
    jw_str(w, "name", rows[r].name[0] ? rows[r].name : "(unnamed)");
    char id[12];
    snprintf(id, sizeof(id), "%08" PRIx32, (uint32_t)rows[r].id);
    jw_str(w, "id", id);
    jw_uint(w, "cpu", rows[r].cpu);
    jw_str(w, "aff", aff_text(rows[r].id));
    jw_int(w, "prio", prio_of(rows[r].id));
    jw_num(w, "pct", rows[r].share / 100.0, 2);
    jw_obj_end(w);
  }
  jw_arr_end(w);
  jw_num(w, "threads_rest_pct", rest / 100.0, 2);
  memcpy(&snap_prev, &snap_now, sizeof(snap_prev));
}

/* ---- nexus2 ---- */

static char text_buf[8192]; /* stage table / shell capture scratch (HTTP thread only) */

static void nexus2_json(jw_t *w)
{
  jw_obj(w, "nexus2");
  jw_bool(w, "linked", nexus2_stage_report != NULL);
  /* `nexus` without a subcommand prints "nexus2: running|stopped, layout X" first */
  const char *state = "absent";
  char layout[16] = "";
  static char st[16];
  if (rtems_shell_lookup_cmd("nexus") != NULL) {
    char *av[] = {"nexus", NULL};
    char tmp[1024];
    if (metrics_shell_capture(1, av, tmp, sizeof(tmp)) >= 0 &&
        sscanf(tmp, "nexus2: %15[a-z], layout %15s", st, layout) >= 1) {
      state = st;
    } else {
      state = "unknown";
    }
  }
  jw_str(w, "state", state);
  /* the station symbols exist only in the nexus2 image (weak below): the base image reports "none" */
  jw_str(w, "layout", layout[0] ? layout
                     : (&nexus_station_layout != NULL && nexus_station_layout_name != NULL)
                         ? nexus_station_layout_name(nexus_station_layout)
                         : "none");
  jw_obj_end(w);

  if (nexus2_stage_report == NULL) {
    jw_null(w, "stages");
    return;
  }
  size_t n = nexus2_stage_report(text_buf, sizeof(text_buf), 0);
  text_buf[n < sizeof(text_buf) ? n : sizeof(text_buf) - 1] = '\0';
  jw_obj(w, "stages");
  (void)metrics_stage_table_json(w, text_buf);
  jw_obj_end(w);
}

/* ---- AD9363 (driver lock: a few SPI reads) ---- */

static void ad9363_json(jw_t *w, int hw)
{
  if (pluto_hw_is_open == NULL) {
    jw_null(w, "ad9363");
    return;
  }
  jw_obj(w, "ad9363");
  int open = pluto_hw_is_open();
  jw_bool(w, "open", open);
  if (open && hw) {
    jw_num(w, "rate_hz", pluto_hw_get_rate(0), 1);
    jw_uint(w, "rx_lo_hz", pluto_hw_get_lo(0));
    jw_uint(w, "tx_lo_hz", pluto_hw_get_lo(1));
    int agc = pluto_hw_get_rx_agc();
    jw_str(w, "rx_agc", agc == 0 ? "manual" : agc == 2 ? "slow_attack" : agc < 0 ? "error" : "other");
    int32_t g = 0;
    int rc = pluto_hw_get_rx_gain(&g);
    if (rc == 0) {
      jw_int(w, "rx_gain_db", g);
    } else {
      jw_null(w, "rx_gain_db");
    }
    /* the gain readback is only taken while the ENSM reads FDD (-EAGAIN otherwise) */
    jw_str(w, "ensm", rc == 0 ? "fdd" : rc == -EAGAIN ? "not_fdd" : "unknown");
    jw_num(w, "tx_atten_db", pluto_hw_get_tx_atten_mdb() / 1000.0, 2);
    int ovf = pluto_hw_tx_overflow();
    if (ovf >= 0) {
      jw_int(w, "tx_overflow_bits", ovf);
    } else {
      jw_null(w, "tx_overflow_bits");
    }
    jw_uint(w, "tx_discarded", pluto_hw_tx_discarded());
    if (pluto_hw_get_tx_fir_scale != NULL) {
      jw_uint(w, "tx_fir_scale", pluto_hw_get_tx_fir_scale());
    }
    int32_t mdeg;
    if (pluto_hw_temp_mdeg(&mdeg) == 0) {
      jw_num(w, "temp_c", mdeg / 1000.0, 1);
    } else {
      jw_null(w, "temp_c");
    }
  }
  /* RX/TX dBFS levels are not kept by the driver: nexus2's dashboard has the per-call RSSI */
  jw_obj_end(w);
}

/* ---- Zynq XADC die temperature through the PS XADC interface (devcfg 0xF8007100) ----
 * The XADC runs its default sequence (temperature and supplies) after configuration; the PS
 * interface is enabled once (Linux xilinx-xadc zynq setup) and read with a bounded poll. */
#define XADC_CFG 0xF8007100u
#define XADC_INTSTS 0xF8007104u
#define XADC_MSTS 0xF800710Cu
#define XADC_CMDFIFO 0xF8007110u
#define XADC_RDFIFO 0xF8007114u
#define XADC_MCTL 0xF8007118u
#define XADC_CFG_ENABLE (1u << 31)
#define XADC_MSTS_DFIFOE (1u << 8)
#define XADC_MSTS_DFIFO_LVL(s) (((s) >> 12) & 0xfu)
#define XADC_CMD_READ(reg) ((1u << 26) | ((uint32_t)(reg) << 16))

static int xadc_temp(double *c)
{
  static int state; /* 0 untried, 1 ready, -1 gave up */
  if (state < 0) {
    return -1;
  }
  if (!((REG32(0xf800700cu) >> 2) & 1u)) {
    return -1; /* PL not configured: no XADC clock */
  }
  if (state == 0) {
    if (!(REG32(XADC_CFG) & XADC_CFG_ENABLE)) {
      REG32(XADC_MCTL) = 1u << 4; /* reset the interface */
      REG32(XADC_MCTL) = 0;
      REG32(XADC_INTSTS) = 0xffffffffu;
      /* enable, read/write edges, TCK = PCAP / 16 (DCLK <= 50 MHz whatever the PCAP rate), IGAP 20 */
      REG32(XADC_CFG) = XADC_CFG_ENABLE | (1u << 13) | (1u << 12) | (3u << 8) | 20u;
    }
    state = 1;
  }
  for (int i = 0; i < 16 && !(REG32(XADC_MSTS) & XADC_MSTS_DFIFOE); ++i) {
    (void)REG32(XADC_RDFIFO); /* stale data */
  }
  REG32(XADC_CMDFIFO) = XADC_CMD_READ(0x00); /* temperature */
  REG32(XADC_CMDFIFO) = 0;                   /* NOP shifts the result out */
  double t0 = mono_s();
  while (XADC_MSTS_DFIFO_LVL(REG32(XADC_MSTS)) < 2) {
    if (mono_s() - t0 > 0.002) {
      state = -1; /* never again: something else owns the interface */
      return -1;
    }
  }
  (void)REG32(XADC_RDFIFO);
  uint32_t v = REG32(XADC_RDFIFO) & 0xffffu;
  *c = (double)(v >> 4) * 503.975 / 4096.0 - 273.15;
  return *c > -60.0 && *c < 150.0 ? 0 : -1;
}

/* ---- network ---- */

static void net_json(jw_t *w, int events)
{
  jw_obj(w, "net");
  struct ifaddrs *ifa = NULL;
  jw_arr(w, "ifaces");
  if (getifaddrs(&ifa) == 0) {
    char seen[16][IFNAMSIZ];
    int nseen = 0;
    for (struct ifaddrs *i = ifa; i != NULL; i = i->ifa_next) {
      int dup = 0;
      for (int j = 0; j < nseen && !dup; ++j) {
        dup = strcmp(seen[j], i->ifa_name) == 0;
      }
      if (dup || nseen >= 16 || strcmp(i->ifa_name, "lo0") == 0) {
        continue;
      }
      snprintf(seen[nseen++], IFNAMSIZ, "%s", i->ifa_name);
      jw_obj(w, NULL);
      jw_str(w, "name", i->ifa_name);
      jw_bool(w, "up", (i->ifa_flags & IFF_UP) != 0);
      jw_bool(w, "running", (i->ifa_flags & IFF_RUNNING) != 0);
      const struct if_data *d = NULL;
      for (struct ifaddrs *k = ifa; k != NULL; k = k->ifa_next) {
        if (strcmp(k->ifa_name, i->ifa_name) == 0 && k->ifa_addr != NULL &&
            k->ifa_addr->sa_family == AF_LINK && k->ifa_data != NULL) {
          d = k->ifa_data;
        }
      }
      if (d != NULL) {
        jw_str(w, "link", d->ifi_link_state == LINK_STATE_UP ? "up" :
                          d->ifi_link_state == LINK_STATE_DOWN ? "down" : "unknown");
        jw_uint(w, "mtu", d->ifi_mtu);
        jw_uint(w, "ipackets", d->ifi_ipackets);
        jw_uint(w, "ierrors", d->ifi_ierrors);
        jw_uint(w, "opackets", d->ifi_opackets);
        jw_uint(w, "oerrors", d->ifi_oerrors);
        jw_uint(w, "iqdrops", d->ifi_iqdrops);
      }
      jw_arr(w, "ipv4");
      for (struct ifaddrs *k = ifa; k != NULL; k = k->ifa_next) {
        if (strcmp(k->ifa_name, i->ifa_name) == 0 && k->ifa_addr != NULL &&
            k->ifa_addr->sa_family == AF_INET) {
          char a[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, &((struct sockaddr_in *)k->ifa_addr)->sin_addr, a, sizeof(a));
          jw_str(w, NULL, a);
        }
      }
      jw_arr_end(w);
      jw_obj_end(w);
    }
    freeifaddrs(ifa);
  }
  jw_arr_end(w);

  char gw[INET_ADDRSTRLEN], gif[IFNAMSIZ];
  if (nx_net_default_route(gw, sizeof(gw), gif, sizeof(gif)) == 0) {
    jw_obj(w, "default_route");
    jw_str(w, "gw", gw);
    jw_str(w, "if", gif);
    jw_obj_end(w);
  } else {
    jw_null(w, "default_route");
  }

  jw_arr(w, "dns");
  FILE *f = fopen("/etc/resolv.conf", "r");
  if (f != NULL) {
    char line[128], ns[64];
    while (fgets(line, sizeof(line), f) != NULL) {
      if (sscanf(line, "nameserver %63s", ns) == 1) {
        jw_str(w, NULL, ns);
      }
    }
    fclose(f);
  }
  jw_arr_end(w);

  struct aicwf_wifi_status ws;
  if (aicwf_wifi_status != NULL && aicwf_wifi_status(&ws) == 0) {
    jw_obj(w, "wifi");
    jw_bool(w, "present", ws.device_present);
    jw_bool(w, "station", ws.station_ready);
    jw_bool(w, "connected", ws.connected);
    jw_bool(w, "want", ws.want_connected);
    char ssid[sizeof(ws.ssid) + 1];
    memcpy(ssid, ws.ssid, sizeof(ws.ssid));
    ssid[sizeof(ws.ssid)] = '\0';
    jw_str(w, "ssid", ssid);
    if (ws.connected) {
      jw_int(w, "rssi_dbm", ws.rssi);
      jw_int(w, "channel", ws.channel);
    } else {
      jw_null(w, "rssi_dbm");
      jw_null(w, "channel");
    }
    jw_uint(w, "connect_attempts", ws.connect_attempts);
    jw_int(w, "last_error", ws.last_error);
    jw_obj_end(w);
  } else {
    jw_null(w, "wifi");
  }

  if (nexus_usbnet_status != NULL) {
    char ub[512];
    if (nexus_usbnet_status(ub, sizeof(ub)) > 0) {
      jw_str(w, "usb_ethernet", ub);
    }
  }

  if (events) {
    /* `net status` keeps the routing-socket events (netcmd.c); its "recent events:" block */
    jw_arr(w, "events");
    char *av[] = {"net", "status", NULL};
    if (metrics_shell_capture(2, av, text_buf, sizeof(text_buf)) >= 0) {
      char *p = strstr(text_buf, "recent events:\n");
      for (char *l = p != NULL ? p + 15 : NULL; l != NULL && *l == ' ';) {
        char *nl = strchr(l, '\n');
        if (nl != NULL) {
          *nl = '\0';
        }
        while (*l == ' ') {
          ++l;
        }
        jw_str(w, NULL, l);
        l = nl != NULL ? nl + 1 : NULL;
      }
    }
    jw_arr_end(w);
  }
  jw_obj_end(w);
}

/* ---- heap: statistics are read without walking the heap; the free list walk gives the
 * largest block (allocations on CPU1 wait for the heap lock meanwhile: few free blocks) ---- */

static void heap_json(jw_t *w)
{
  jw_obj(w, "heap");
  Heap_Control *h = RTEMS_Malloc_Heap;
  if (h != NULL) {
    jw_uint(w, "size", h->stats.size);
    jw_uint(w, "free", h->stats.free_size);
    jw_uint(w, "min_free", h->stats.min_free_size);
    jw_uint(w, "free_blocks", h->stats.free_blocks);
    jw_uint(w, "used_blocks", h->stats.used_blocks);
    jw_uint(w, "failed_allocs", h->stats.failed_allocs);
    Heap_Information fi;
    if (_Protected_heap_Get_free_information(h, &fi)) {
      jw_uint(w, "largest_free", fi.largest);
    }
  }
  jw_obj_end(w);
}

/* ---- stacks (full=1 only) ---- */

#define M_STACKS 160
typedef struct {
  char name[M_NAME];
  uint32_t size, used;
} m_stack;
static m_stack stacks[M_STACKS];
static size_t nstacks;

static void stack_visit(const rtems_stack_checker_info *info, void *arg)
{
  (void)arg;
  if (nstacks >= M_STACKS || info->used == UINTPTR_MAX) {
    return;
  }
  m_stack *s = &stacks[nstacks++];
  if (info->name != NULL && info->name[0] != '\0') {
    snprintf(s->name, sizeof(s->name), "%s", info->name);
  } else {
    snprintf(s->name, sizeof(s->name), "%08" PRIx32, (uint32_t)info->id);
  }
  s->size = (uint32_t)info->size;
  s->used = (uint32_t)info->used;
}

static int stack_cmp(const void *a, const void *b)
{
  const m_stack *x = a, *y = b;
  /* highest fill first */
  uint64_t fx = x->size ? (uint64_t)x->used * 1000000u / x->size : 0;
  uint64_t fy = y->size ? (uint64_t)y->used * 1000000u / y->size : 0;
  return fx < fy ? 1 : fx > fy ? -1 : 0;
}

static void stacks_json(jw_t *w, int top)
{
  nstacks = 0;
  rtems_stack_checker_iterate(stack_visit, NULL);
  qsort(stacks, nstacks, sizeof(stacks[0]), stack_cmp);
  jw_arr(w, "stacks");
  for (size_t i = 0; i < nstacks && (int)i < top; ++i) {
    jw_obj(w, NULL);
    jw_str(w, "name", stacks[i].name);
    jw_uint(w, "size", stacks[i].size);
    jw_uint(w, "used", stacks[i].used);
    jw_num(w, "pct", stacks[i].size ? 100.0 * stacks[i].used / stacks[i].size : 0.0, 1);
    jw_obj_end(w);
  }
  jw_arr_end(w);
}

/* ---- the document ---- */

size_t metrics_json(char *buf, size_t cap, const metrics_opts *o)
{
  jw_t w;
  jw_init(&w, buf, cap);
  jw_obj(&w, NULL);
  jw_str(&w, "schema", NEXUS_METRICS_SCHEMA);
  double up = mono_s();
  jw_num(&w, "uptime_s", up, 3);
  {
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    jw_num(&w, "unix_s", (double)rt.tv_sec + rt.tv_nsec * 1e-9, 3);
  }

  jw_obj(&w, "build");
  jw_str(&w, "git", NEXUS_BUILD_GIT);
  jw_str(&w, "ci_run", NEXUS_BUILD_RUN[0] ? NEXUS_BUILD_RUN : NULL);
  jw_str(&w, "nexus_ref", NEXUS_BUILD_NEXUS_REF[0] ? NEXUS_BUILD_NEXUS_REF : NULL);
  jw_str(&w, "built", __DATE__ " " __TIME__);
  jw_str(&w, "rtems", rtems_version());
  jw_str(&w, "usb_stack", METRICS_USB_STACK);
  jw_str(&w, "image", nexus2_stage_report != NULL ? "station" : "base");
  jw_obj_end(&w);

  cpu_json(&w, o->top);
  nexus2_json(&w);
  ad9363_json(&w, o->hw);
  if (metrics_usb_json != NULL) {
    metrics_usb_json(&w);
  } else {
    jw_null(&w, "usb");
  }
  net_json(&w, o->events);
  heap_json(&w);
  if (o->stacks) {
    stacks_json(&w, o->top > 24 ? o->top : 24);
  }

  jw_obj(&w, "temp");
  double t;
  if (xadc_temp(&t) == 0) {
    jw_num(&w, "zynq_c", t, 1);
  } else {
    jw_null(&w, "zynq_c");
  }
  jw_obj_end(&w);

  jw_obj(&w, "log");
  if (nexus_log_read != NULL) {
    uint32_t next = 0, oldest = 0;
    (void)nexus_log_read(0, NULL, 0, &next, &oldest); /* cap 0: offsets only */
    jw_uint(&w, "next", next);
    jw_uint(&w, "oldest", oldest);
  } else {
    jw_null(&w, "next");
  }
  jw_obj_end(&w);

  jw_num(&w, "collect_ms", (mono_s() - up) * 1000.0, 2);
  jw_obj_end(&w);
  return jw_finish(&w);
}
