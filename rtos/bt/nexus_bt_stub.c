/*
 * Bluetooth stub for images built without BTstack (rtos/bt/fetch-btstack.sh not run): the API
 * of nexus_bt.h exists, the shell command says why there is no Bluetooth.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "nexus_bt.h"

#include <rtems/shell.h>
#include <stdio.h>

void nexus_bt_start(void)
{
}

static int bt_cmd(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  printf("bt: not in this image (built without BTstack)\n");
  return 1;
}

void nexus_bt_shell_register(void)
{
  rtems_shell_add_cmd("bt", "misc", "bt: Bluetooth (not in this image)", bt_cmd);
}

size_t nexus_bt_audio_write(const int16_t *pcm, size_t frames, unsigned rate)
{
  (void)pcm;
  (void)frames;
  (void)rate;
  return 0;
}

size_t nexus_bt_audio_write_ch(const int16_t *pcm, size_t frames, unsigned rate, unsigned channels)
{
  (void)pcm;
  (void)frames;
  (void)rate;
  (void)channels;
  return 0;
}

int nexus_bt_audio_active(void)
{
  return 0;
}
