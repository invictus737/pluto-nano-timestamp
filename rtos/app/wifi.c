/*
 * Wi-Fi station on an AICSemi AIC8800 USB stick (rtos/libbsd/aic8800).
 *
 * The stick's firmware associates, the driver's embedded supplicant does
 * WPA/WPA2/WPA3-Personal, and the station appears as the Ethernet interface "wlan0";
 * dhcpcd (started for all interfaces in main.c) configures it when its link comes up.
 * The driver keeps reconnecting (link loss, firmware restart, replug) while a network is
 * set.
 *
 * Shell: wifi status | wifi scan | wifi join <ssid> [passphrase] | wifi leave
 *        (connect/disconnect are aliases; without an AIC stick but with a net80211 stick
 *        the command goes to the wpa_supplicant path in netcmd.c)
 *        | wifi wait [seconds]
 * Diagnostics: wifi fwdiag | wifi debug on|off | wifi fw auto|sdk5|legacy
 *        | wifi rescan | wifi reprobe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rtems/shell.h>

#include "aicwf_wifi.h"
#include "netcmd.h"

int nexus_wifi_start(const char *ssid, const char *psk);
int nexus_wifi_scan(void);
int nexus_wifi_status(void);

static void print_mac(const uint8_t *mac)
{
  printf("%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int nexus_wifi_status(void)
{
  struct aicwf_wifi_status st;

  if (aicwf_wifi_status(&st) != 0)
    return -1;
  printf("wifi: stick %s, %s %s, ", st.device_present ? "present" : "absent",
         AICWF_WIFI_IFNAME, st.station_ready ? "up" : "not created");
  if (st.station_ready) {
    printf("MAC ");
    print_mac(st.mac);
    printf(", ");
  }
  if (st.connected)
    printf("connected to \"%s\" channel %d %d dBm\n", st.ssid, st.channel, st.rssi);
  else if (st.want_connected)
    printf("connecting to \"%s\" (%u attempts, last result %d)\n", st.ssid,
           st.connect_attempts, st.last_error);
  else
    printf("no network set\n");
  return st.connected;
}

int nexus_wifi_scan(void)
{
  struct aicwf_wifi_bss bss[32];
  int n = aicwf_wifi_scan(bss, 32, 12000);
  int i;

  if (n < 0) {
    printf("wifi: scan failed (%d)%s\n", n,
           n == -8 ? ": no station (is the stick attached?)" : "");
    return n;
  }
  printf("%-32s %-17s %4s %5s %s\n", "SSID", "BSSID", "CH", "dBm", "SECURITY");
  for (i = 0; i < n; i++) {
    printf("%-32s ", bss[i].ssid[0] ? bss[i].ssid : "<hidden>");
    print_mac(bss[i].bssid);
    printf(" %4d %5d %s\n", bss[i].channel, bss[i].rssi, bss[i].security_name);
  }
  return n;
}

static int wifi_cmd(int argc, char **argv)
{
  struct aicwf_wifi_status ast;

  /* AIC driver diagnostics always go to the AIC driver, whatever else is plugged in */
  int aic_only = argc > 1 && (strcmp(argv[1], "fw") == 0 || strcmp(argv[1], "reprobe") == 0 ||
                              strcmp(argv[1], "fwdiag") == 0 || strcmp(argv[1], "debug") == 0 ||
                              strcmp(argv[1], "rescan") == 0);
  if (!aic_only && nx_wifi80211_present() &&
      (aicwf_wifi_status(&ast) != 0 || !ast.device_present))
    return nx_wifi80211_cmd(argc, argv);
  if (argc >= 2 && strcmp(argv[1], "connect") == 0)
    argv[1] = "join";
  else if (argc >= 2 && strcmp(argv[1], "disconnect") == 0)
    argv[1] = "leave";
  if (argc < 2 || strcmp(argv[1], "status") == 0) {
    nexus_wifi_status();
    return 0;
  }
  if (strcmp(argv[1], "scan") == 0)
    return nexus_wifi_scan() < 0;
  if (strcmp(argv[1], "join") == 0 && (argc == 3 || argc == 4)) {
    int rc = aicwf_wifi_connect(argv[2], argc == 4 ? argv[3] : NULL);

    if (rc != 0)
      printf("wifi: invalid network (SSID 1..32 bytes, passphrase 8..63 characters)\n");
    else
      printf("wifi: joining \"%s\"; 'wifi wait' or 'wifi status' to follow\n", argv[2]);
    return rc != 0;
  }
  if (strcmp(argv[1], "leave") == 0) {
    aicwf_wifi_disconnect();
    return 0;
  }
  if (strcmp(argv[1], "wait") == 0) {
    int seconds = argc > 2 ? atoi(argv[2]) : 30;
    int up = aicwf_wifi_wait_connected(seconds * 1000);

    nexus_wifi_status();
    return up ? 0 : 1;
  }
  if (strcmp(argv[1], "fwdiag") == 0) {
    aicwf_wifi_fwdiag();
    nexus_wifi_status();
    return 0;
  }
  if (strcmp(argv[1], "debug") == 0 && argc == 3) {
    aicwf_wifi_set_debug(strcmp(argv[2], "on") == 0);
    printf("wifi: driver debug log %s\n", strcmp(argv[2], "on") == 0 ? "on" : "off");
    return 0;
  }
  if (strcmp(argv[1], "fw") == 0 && argc == 3) {
    int mode = strcmp(argv[2], "auto") == 0 ? 0 : strcmp(argv[2], "sdk5") == 0 ? 1 :
               strcmp(argv[2], "legacy") == 0 ? 2 : -1;

    if (mode < 0 || aicwf_wifi_set_fw_mode(mode) != 0) {
      printf("wifi: fw auto|sdk5|legacy\n");
      return 1;
    }
    printf("wifi: D80 firmware set %s from the next probe ('wifi reprobe' or replug)\n", argv[2]);
    return 0;
  }
  if (strcmp(argv[1], "rescan") == 0) {
    printf("wifi: %d empty USB port(s) power-cycled, bus re-explored\n", aicwf_wifi_usb_rescan());
    return 0;
  }
  if (strcmp(argv[1], "reprobe") == 0) {
    int rc = aicwf_wifi_reprobe();

    printf("wifi: re-enumerate %s (%d)\n", rc == 0 ? "started" : "failed", rc);
    return rc != 0;
  }
  printf("usage: wifi [status | scan | join <ssid> [passphrase] | leave | wait [s]\n"
         "            | fwdiag | debug on|off | fw auto|sdk5|legacy | rescan | reprobe]\n");
  return 1;
}

int nexus_wifi_start(const char *ssid, const char *psk)
{
  static int shell_registered;
  int rc;

  rc = aicwf_wifi_start();
  if (rc != 0) {
    printf("wifi: driver start failed (%d)\n", rc);
    return rc;
  }
  if (!shell_registered) {
    shell_registered = 1;
    rtems_shell_add_cmd("wifi", "network", "wifi status|scan|join <ssid> [psk]|leave|wait [s]|fwdiag|debug on|off|fw auto|sdk5|legacy|rescan|reprobe",
                        wifi_cmd);
  }
  if (ssid != NULL && ssid[0] != '\0') {
    rc = aicwf_wifi_connect(ssid, psk);
    if (rc != 0)
      printf("wifi: invalid network settings for \"%s\"\n", ssid);
  }
  return rc;
}
