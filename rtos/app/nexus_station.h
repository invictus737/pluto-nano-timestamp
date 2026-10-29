/*
 * Station (nexus2) placement and load measurement on the Pluto RTOS.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NEXUS_STATION_H
#define NEXUS_STATION_H

#include <stddef.h>

/* [SYSTEM] nexus_layout = light | cpu1 | float (posix_compat.c, rf-front rt_thread.rs) */
#define NEXUS_LAYOUT_LIGHT 0
#define NEXUS_LAYOUT_CPU1 1
#define NEXUS_LAYOUT_FLOAT 2

extern int nexus_station_layout;
extern int nexus_station_base_prio;
/* the calling thread and every thread it creates belong to the station */
void nexus_station_mark_self(void);
const char *nexus_station_layout_name(int layout);
int nexus_station_layout_parse(const char *v);

/* nexus_load.c: `nexus load`/`nexus stages` and the 60 s summary line */
int nexus_load_shell(int argc, char **argv);
void nexus_load_logger_start(void);
/* `nexus env`: NEXUS2_* switches for the next start, /data/nexus2/nexus2.env */
int nexus_env_shell(int argc, char **argv);
void nexus_env_load_file(void);
/* console log ring (posix_compat.c): stdout/stderr of every thread, readable over SSH */
int nexus_log_shell(int argc, char **argv);

/* nexus2 (Rust, rf-front stage_timing.rs); weak: absent in images without nexus2 */
size_t nexus2_stage_report(char *buf, size_t cap, int reset) __attribute__((weak));
size_t nexus2_stage_summary(char *buf, size_t cap) __attribute__((weak));

#endif
