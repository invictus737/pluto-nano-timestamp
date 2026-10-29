/*
 * Host run of the Bluetooth manager against the scripted controller (fake_board.c):
 * bt status / on / scan / devices / off, the AFH command and the /data/bt files.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "nexus_bt.h"
#include "rtems/shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char host_data_dir[];
extern rtems_shell_command_t host_bt_cmd;
extern unsigned host_afh_commands;
extern int host_controller_present;
extern int host_bt_presence;

static int run(const char *line)
{
  char buf[128], *argv[8];
  int argc = 0;
  snprintf(buf, sizeof(buf), "%s", line);
  for (char *t = strtok(buf, " "); t && argc < 8; t = strtok(NULL, " ")) {
    argv[argc++] = t;
  }
  printf("> %s\n", line);
  fflush(stdout);
  int rc = host_bt_cmd(argc, argv);
  fflush(stdout);
  return rc;
}

int main(int argc, char **argv)
{
  int fails = 0;
  snprintf(host_data_dir, 256, "%s", argc > 1 ? argv[1] : "/tmp/nexus-bt-host");
  mkdir(host_data_dir, 0700);
  nexus_bt_shell_register();
  nexus_bt_start();
  sleep(1);
  fails += run("bt status") != 0;
  host_bt_presence = 2; /* DC stick: said so, keeps waiting */
  fails += run("bt on") != 0;
  fails += run("bt status") != 0;
  host_bt_presence = -1;
  host_controller_present = 0; /* stick not attached yet: on reports it and retries */
  fails += run("bt on") != 0;
  host_controller_present = 1;
  sleep(4);
  fails += run("bt on") != 0; /* "already on" after the retry */
  fails += run("bt status") != 0;
  fails += run("bt scan 2") != 0;
  fails += run("bt devices") != 0;
  fails += run("bt connect zz") == 0; /* bad address refused */
  fails += run("bt off") != 0;
  char p[300];
  snprintf(p, sizeof(p), "%s/bt", host_data_dir); /* the TLV file appears with the first key */
  struct stat st;
  if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) {
    printf("FAIL: no %s\n", p);
    fails++;
  }
  if (host_afh_commands == 0) {
    printf("FAIL: no AFH classification sent\n");
    fails++;
  }
  printf(fails ? "host_main: %d FAILED\n" : "host_main: ok\n", fails);
  return fails != 0;
}
