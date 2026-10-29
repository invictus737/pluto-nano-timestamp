/*
 * Nexus-BS RTOS network control: per-interface static/DHCP from the configuration (through
 * dhcpcd's own `static` support), the `net` shell command, the net80211 Wi-Fi supervisor
 * (wlan clone + wpa_supplicant with a control socket) and the tun device for nexus2.
 *
 * Call order in Init (main.c):
 *   start_dhcpcd():  nx_net_dhcpcd_conf_write(); rtems_dhcpcd_start(nx_net_dhcpcd_config());
 *   after nexus_config_init(): nx_net_start();
 *   Wi-Fi block: nx_wifi80211_start(ssid, psk)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NEXUS_NETCMD_H
#define NEXUS_NETCMD_H

#include <stddef.h>

#include <rtems/dhcpcd.h>

/* (re)write /etc/dhcpcd.conf from the configuration and the runtime overrides; dhcpcd reads it
 * again for an interface whenever that interface arrives or gets its carrier back */
void nx_net_dhcpcd_conf_write(void);
/* dhcpcd start configuration: reports when the dhcpcd task ends (it cannot be restarted) */
const rtems_dhcpcd_config *nx_net_dhcpcd_config(void);
/* configuration loaded: apply static settings to interfaces already present, follow config
 * changes, network sysctls, register the `net` shell command */
void nx_net_start(void);

/* block until a non-loopback interface has an IPv4 address (routing-socket events, no polling
 * while the event monitor of nx_net_start() runs); fills its name and address */
int nx_net_wait_inet(char *ifn, size_t icap, char *addr, size_t acap);

/* net80211 USB Wi-Fi sticks (rtwn, run, ...): clone wlan1/wlan2, run wpa_supplicant */
void nx_wifi80211_start(const char *ssid, const char *psk);
/* `wifi` subcommands for the net80211 path; 1 = a net80211 stick is present */
int nx_wifi80211_present(void);
int nx_wifi80211_cmd(int argc, char **argv);

#endif
