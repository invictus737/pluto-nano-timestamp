/*
 * Station load measurement (rtos-optimizer "measure first"): per-thread CPU share and per-CPU idle
 * over an interval, from the RTEMS per-thread CPU time (no sampling, no hooks on the hot path),
 * plus nexus2's own per-stage timing (rf-front stage_timing.rs) when the station is linked in.
 *
 *   nexus load [seconds]   per-thread CPU % over the interval (default 5 s), CPU0/CPU1 busy/idle
 *   nexus load log <s|0>   period of the one-line summary in the log (default 60 s, 0 = off)
 *   nexus stages [reset]   nexus2 per-stage timing: count, mean/p99/max us, over-budget
 *
 * Interrupt time is charged to the interrupted thread (RTEMS), so an I/O thread's share on the
 * interrupt CPU includes the DMA/USB interrupts it was preempted by.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE /* pthread_*affinity_np, CPU_SET */
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rtems.h>
#include <rtems/score/percpu.h>
#include <rtems/score/threadimpl.h>
#include <rtems/score/todimpl.h>

#include "nexus_station.h"

#define LOAD_MAX_THREADS 192
#define LOAD_NAME 24
#define LOAD_CPUS 2

typedef struct {
  rtems_id id;
  char name[LOAD_NAME];
  Timestamp_Control used;
  uint8_t idle;
  uint8_t cpu;
} load_thread;

typedef struct {
  Timestamp_Control uptime;
  size_t n;
  load_thread t[LOAD_MAX_THREADS];
} load_snapshot;

typedef struct {
  rtems_id id;
  char name[LOAD_NAME];
  uint32_t permille10; /* share of one CPU in 0.01 % */
  uint8_t cpu;
} load_row;

static bool load_visit(Thread_Control *th, void *arg)
{
  load_snapshot *s = arg;
  if (s->n >= LOAD_MAX_THREADS) {
    return true;
  }
  load_thread *t = &s->t[s->n++];
  t->id = th->Object.id;
  (void)_Thread_Get_name(th, t->name, sizeof(t->name));
  t->used = _Thread_Get_CPU_time_used(th);
  t->idle = th->is_idle ? 1 : 0;
  t->cpu = (uint8_t)_Per_CPU_Get_index(_Thread_Get_CPU(th));
  return false;
}

static void load_take(load_snapshot *s)
{
  s->n = 0;
  rtems_task_iterate(load_visit, s);
  _TOD_Get_uptime(&s->uptime);
}

static const load_thread *load_find(const load_snapshot *s, rtems_id id)
{
  for (size_t i = 0; i < s->n; ++i) {
    if (s->t[i].id == id) {
      return &s->t[i];
    }
  }
  return NULL;
}

/* share of one CPU in 0.01 % */
static uint32_t load_share(Timestamp_Control used, Timestamp_Control wall)
{
  if (wall <= 0 || used <= 0) {
    return 0;
  }
  return (uint32_t)(((uint64_t)used * 10000u) / (uint64_t)wall);
}

static int row_cmp(const void *a, const void *b)
{
  const load_row *x = a, *y = b;
  return x->permille10 < y->permille10 ? 1 : x->permille10 > y->permille10 ? -1 : 0;
}

/* rows sorted by share (busy threads only), idle share per CPU; returns the row count */
static size_t load_diff(const load_snapshot *a, const load_snapshot *b, load_row *rows,
                        uint32_t idle[LOAD_CPUS], Timestamp_Control *wall)
{
  size_t n = 0;
  *wall = b->uptime - a->uptime;
  idle[0] = idle[1] = 0;
  for (size_t i = 0; i < b->n; ++i) {
    const load_thread *now = &b->t[i];
    const load_thread *then = load_find(a, now->id);
    Timestamp_Control d = now->used - (then != NULL ? then->used : 0);
    uint32_t share = load_share(d, *wall);
    if (now->idle) {
      if (now->cpu < LOAD_CPUS) {
        idle[now->cpu] += share;
      }
      continue;
    }
    rows[n].id = now->id;
    memcpy(rows[n].name, now->name, LOAD_NAME);
    rows[n].permille10 = share;
    rows[n].cpu = now->cpu;
    ++n;
  }
  qsort(rows, n, sizeof(rows[0]), row_cmp);
  return n;
}

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

static rtems_task_priority prio_of(rtems_id id)
{
  rtems_id sched;
  rtems_task_priority p = 0;
  if (rtems_task_get_scheduler(id, &sched) == RTEMS_SUCCESSFUL) {
    (void)rtems_task_get_priority(id, sched, &p);
  }
  return p;
}

static void print_pct(char *out, size_t cap, uint32_t v)
{
  snprintf(out, cap, "%" PRIu32 ".%02" PRIu32, v / 100u, v % 100u);
}

static load_snapshot shell_a, shell_b;
static load_row shell_rows[LOAD_MAX_THREADS];
static pthread_mutex_t shell_lock = PTHREAD_MUTEX_INITIALIZER;

static void load_report(unsigned seconds)
{
  pthread_mutex_lock(&shell_lock);
  load_take(&shell_a);
  sleep(seconds);
  load_take(&shell_b);
  uint32_t idle[LOAD_CPUS];
  Timestamp_Control wall;
  size_t n = load_diff(&shell_a, &shell_b, shell_rows, idle, &wall);
  uint32_t ncpu = rtems_scheduler_get_processor_maximum();
  printf("nexus load over %u s, layout %s\n", seconds, nexus_station_layout_name(nexus_station_layout));
  for (uint32_t c = 0; c < ncpu && c < LOAD_CPUS; ++c) {
    char b[16], i[16];
    uint32_t id = idle[c] > 10000u ? 10000u : idle[c];
    print_pct(b, sizeof(b), 10000u - id);
    print_pct(i, sizeof(i), id);
    printf("  CPU%" PRIu32 ": busy %6s %%  idle %6s %%\n", c, b, i);
  }
  printf("  %-10s %-24s %3s %3s %4s %7s\n", "id", "name", "cpu", "aff", "prio", "%cpu");
  uint32_t rest = 0;
  for (size_t r = 0; r < n; ++r) {
    if (r >= 40 || shell_rows[r].permille10 < 5) { /* < 0.05 % */
      rest += shell_rows[r].permille10;
      continue;
    }
    char p[16];
    print_pct(p, sizeof(p), shell_rows[r].permille10);
    printf("  0x%08" PRIx32 " %-24s %3u %3s %4" PRIu32 " %7s\n", (uint32_t)shell_rows[r].id,
           shell_rows[r].name[0] ? shell_rows[r].name : "(unnamed)", shell_rows[r].cpu,
           aff_text(shell_rows[r].id), (uint32_t)prio_of(shell_rows[r].id), p);
  }
  char p[16];
  print_pct(p, sizeof(p), rest);
  printf("  %-10s %-24s %3s %3s %4s %7s\n", "", "(others)", "", "", "", p);
  pthread_mutex_unlock(&shell_lock);
}

/* ---- periodic one-line summary ---- */

static volatile unsigned log_period_s = 60;
static load_snapshot log_a, log_b;
static load_row log_rows[LOAD_MAX_THREADS];
static char log_stage[512];

static void *load_logger(void *arg)
{
  (void)arg;
  load_take(&log_a);
  for (;;) {
    unsigned period = log_period_s;
    sleep(period == 0 ? 5 : period);
    if (log_period_s == 0) {
      load_take(&log_a);
      continue;
    }
    load_take(&log_b);
    uint32_t idle[LOAD_CPUS];
    Timestamp_Control wall;
    size_t n = load_diff(&log_a, &log_b, log_rows, idle, &wall);
    char line[768];
    size_t o = 0;
    char b0[16], b1[16];
    print_pct(b0, sizeof(b0), 10000u - (idle[0] > 10000u ? 10000u : idle[0]));
    print_pct(b1, sizeof(b1), 10000u - (idle[1] > 10000u ? 10000u : idle[1]));
    o += (size_t)snprintf(line + o, sizeof(line) - o, "load %us %s: cpu0 %s%% cpu1 %s%% |",
                          (unsigned)(wall >> 32), nexus_station_layout_name(nexus_station_layout), b0, b1);
    for (size_t r = 0; r < n && r < 6 && o < sizeof(line) - 40; ++r) {
      char p[16];
      print_pct(p, sizeof(p), log_rows[r].permille10);
      o += (size_t)snprintf(line + o, sizeof(line) - o, " %s@%u %s", log_rows[r].name[0] ? log_rows[r].name : "?",
                            log_rows[r].cpu, p);
    }
    log_stage[0] = '\0';
    if (nexus2_stage_summary != NULL && nexus2_stage_summary(log_stage, sizeof(log_stage)) > 0 &&
        o < sizeof(line) - 4) {
      snprintf(line + o, sizeof(line) - o, " | %s", log_stage);
    }
    printf("%s\n", line);
    memcpy(&log_a, &log_b, sizeof(log_a));
  }
  return NULL;
}

void nexus_load_logger_start(void)
{
  static int started;
  if (started) {
    return;
  }
  started = 1;
  pthread_attr_t at;
  struct sched_param sp = {.sched_priority = 3};
  cpu_set_t set;
  pthread_t t;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 16 * 1024);
  pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&at, SCHED_FIFO);
  pthread_attr_setschedparam(&at, &sp);
  CPU_ZERO(&set);
  CPU_SET(0, &set);
  pthread_attr_setaffinity_np(&at, sizeof(set), &set);
  if (pthread_create(&t, &at, load_logger, NULL) == 0) {
    pthread_setname_np(t, "nexus-load");
  } else {
    started = 0;
  }
  pthread_attr_destroy(&at);
}

static char stage_buf[4096];

int nexus_load_shell(int argc, char **argv)
{
  /* argv[0] = "nexus", argv[1] = "load" | "stages" */
  if (argc >= 2 && strcmp(argv[1], "stages") == 0) {
    if (nexus2_stage_report == NULL) {
      printf("nexus stages: nexus2 not linked in this image\n");
      return 1;
    }
    int reset = argc >= 3 && strcmp(argv[2], "reset") == 0;
    pthread_mutex_lock(&shell_lock);
    size_t n = nexus2_stage_report(stage_buf, sizeof(stage_buf), reset);
    fwrite(stage_buf, 1, n < sizeof(stage_buf) ? n : sizeof(stage_buf), stdout);
    pthread_mutex_unlock(&shell_lock);
    return 0;
  }
  if (argc >= 4 && strcmp(argv[2], "log") == 0) {
    log_period_s = (unsigned)strtoul(argv[3], NULL, 0);
    nexus_load_logger_start();
    printf("nexus load: summary every %u s%s\n", log_period_s, log_period_s ? "" : " (off)");
    return 0;
  }
  unsigned s = argc >= 3 ? (unsigned)strtoul(argv[2], NULL, 0) : 5;
  load_report(s == 0 ? 1 : s > 300 ? 300 : s);
  return 0;
}

/* ---- nexus2 environment switches (NEXUS2_*, RUST_LOG) ----
 * RTEMS starts with an empty environment, so none of the switches the Linux Pluto station runs
 * with (contrib/pluto/apps/start.sh) are set unless given here. They apply at the next
 * `nexus start`; `nexus env save` keeps them in /data/nexus2/nexus2.env, loaded at every start.
 * NEXUS2_RTEMS_* belong to the layout (`nexus layout`) and are never saved. */

#define NEXUS2_ENV_FILE "/data/nexus2/nexus2.env"
extern char **environ;

static int env_switch(const char *kv)
{
  return (strncmp(kv, "NEXUS2_", 7) == 0 && strncmp(kv, "NEXUS2_RTEMS_", 13) != 0) ||
         strncmp(kv, "NEXUS2_RTEMS_IO_CPU=", 20) == 0 || strncmp(kv, "RUST_LOG=", 9) == 0;
}

/* NAME=VALUE sets, NAME= unsets; 0 ok */
static int env_put(const char *kv)
{
  const char *eq = strchr(kv, '=');
  if (eq == NULL || eq == kv || (size_t)(eq - kv) >= 64) {
    return -1;
  }
  char name[64];
  memcpy(name, kv, (size_t)(eq - kv));
  name[eq - kv] = '\0';
  if (!env_switch(kv)) {
    return -1;
  }
  return eq[1] == '\0' ? unsetenv(name) : setenv(name, eq + 1, 1);
}

/* the switches the Linux Pluto reference station is validated with, without the Linux-only
 * ones (PIPELINE_RT: Linux RT group scheduling; MANAGE_SVXLINK; FUSED_RX_ANALYSIS: linux-gated) */
static const char *const env_pluto[] = {
  "RUST_LOG=warn",
  "NEXUS2_PLUTO_RX_LATENCY_US=2000",
  "NEXUS2_PLUTO_ANALYSIS_Q15=1",
  "NEXUS2_PLUTO_ANALYSIS_PAIR=1",
  "NEXUS2_PLUTO_SYNTHESIS_HISTORY=1",
  "NEXUS2_DRIVE_WAKE=1",
  "NEXUS2_IDLE_GATE=1",
  "NEXUS2_TX_BATCH=4",
  "NEXUS2_TX_SILENT=1",
  "NEXUS2_PLUTO_PARK_INACTIVE_RX=1",
  "NEXUS2_PLUTO_RUNNING_LEAD12=1",
  "NEXUS2_PLUTO_RUNNING_LEAD16=1",
  "NEXUS2_PLUTO_DMR_RX_CREDIT=1",
  "NEXUS2_PLUTO_FM_RX1PACKET=1",
  "NEXUS2_PLUTO_TX_RESAMPLER_BLOCK=1",
  "NEXUS2_PLUTO_RX_RESAMPLER_BLOCK=1",
  NULL,
};

void nexus_env_load_file(void)
{
  FILE *f = fopen(NEXUS2_ENV_FILE, "r");
  if (f == NULL) {
    return;
  }
  char line[160];
  int n = 0;
  while (fgets(line, sizeof(line), f) != NULL) {
    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] != '\0' && line[0] != '#' && env_put(line) == 0) {
      ++n;
    }
  }
  fclose(f);
  printf("nexus2: %d switch(es) from %s\n", n, NEXUS2_ENV_FILE);
}

static void env_list(void)
{
  int n = 0;
  for (char **e = environ; e != NULL && *e != NULL; ++e) {
    if (strncmp(*e, "NEXUS2_", 7) == 0 || strncmp(*e, "RUST_LOG=", 9) == 0) {
      printf("  %s\n", *e);
      ++n;
    }
  }
  if (n == 0) {
    printf("  (no NEXUS2_* switches set)\n");
  }
}

int nexus_env_shell(int argc, char **argv)
{
  /* argv[0] = "nexus", argv[1] = "env" */
  if (argc < 3) {
    env_list();
    printf("nexus env NAME=VALUE|NAME= ... | pluto | save | clear  (applies at the next start)\n");
    return 0;
  }
  if (strcmp(argv[2], "pluto") == 0) {
    for (const char *const *kv = env_pluto; *kv != NULL; ++kv) {
      (void)env_put(*kv);
    }
    env_list();
    return 0;
  }
  if (strcmp(argv[2], "save") == 0) {
    FILE *f = fopen(NEXUS2_ENV_FILE ".tmp", "w");
    if (f == NULL) {
      printf("nexus env: cannot write %s\n", NEXUS2_ENV_FILE);
      return 1;
    }
    fprintf(f, "# nexus2 switches, loaded at every `nexus start` (`nexus env save`)\n");
    for (char **e = environ; e != NULL && *e != NULL; ++e) {
      if (env_switch(*e)) {
        fprintf(f, "%s\n", *e);
      }
    }
    int ok = fclose(f) == 0 && rename(NEXUS2_ENV_FILE ".tmp", NEXUS2_ENV_FILE) == 0;
    printf("nexus env: %s %s\n", ok ? "saved to" : "FAILED to save", NEXUS2_ENV_FILE);
    return ok ? 0 : 1;
  }
  if (strcmp(argv[2], "clear") == 0) {
    for (int again = 1; again;) {
      again = 0;
      for (char **e = environ; e != NULL && *e != NULL; ++e) {
        if (env_switch(*e)) {
          char kv[160];
          snprintf(kv, sizeof(kv), "%.*s=", (int)strcspn(*e, "="), *e);
          (void)env_put(kv);
          again = 1;
          break;
        }
      }
    }
    (void)unlink(NEXUS2_ENV_FILE);
    printf("nexus env: cleared, %s removed\n", NEXUS2_ENV_FILE);
    return 0;
  }
  for (int i = 2; i < argc; ++i) {
    if (env_put(argv[i]) != 0) {
      printf("nexus env: %s: expected NEXUS2_NAME=VALUE or RUST_LOG=VALUE (NEXUS2_RTEMS_* is the layout's)\n", argv[i]);
      return 1;
    }
  }
  env_list();
  return 0;
}
