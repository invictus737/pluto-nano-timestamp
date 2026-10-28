/*
 * AIC8800 Wi-Fi station control for the application (RTEMS threads).
 *
 * The RT-Smart driver's firmware owns the 802.11 MAC; the RT-Thread WLAN offload
 * framework's embedded supplicant does WPA/WPA2/WPA3-Personal. The station is a plain
 * Ethernet-style ifnet ("wlan0") once the stick has been brought up; DHCP is up to the
 * caller (dhcpcd picks the interface up when its link comes up).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AICWF_WIFI_H
#define AICWF_WIFI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AICWF_WIFI_IFNAME "wlan0"

struct aicwf_wifi_bss {
	char ssid[33];
	uint8_t bssid[6];
	int16_t channel;
	int16_t rssi;           /* dBm */
	uint8_t band_5ghz;
	uint32_t security;      /* rt_wlan_security_t */
	const char *security_name;
};

struct aicwf_wifi_status {
	int device_present;     /* a stick is attached and its firmware runs */
	int station_ready;      /* wlan0 exists */
	int connected;          /* associated and the controlled port is open */
	int want_connected;     /* credentials are set: reconnects automatically */
	char ssid[33];
	int16_t rssi;
	int16_t channel;
	uint8_t mac[6];
	unsigned connect_attempts;
	int last_error;
};

/* Start the driver (firmware files, USB class drivers). Idempotent, non-blocking. */
int aicwf_wifi_start(void);
/*
 * Set the network to join (psk NULL or "" = open network) and keep the station connected
 * across disconnects, firmware restarts and stick replugs. Non-blocking.
 */
int aicwf_wifi_connect(const char *ssid, const char *psk);
/* Forget the network and disconnect. */
int aicwf_wifi_disconnect(void);
/* Blocking scan (up to timeout_ms); returns the number of entries written, or < 0. */
int aicwf_wifi_scan(struct aicwf_wifi_bss *out, int max, int timeout_ms);
int aicwf_wifi_status(struct aicwf_wifi_status *status);
/* Wait until connected (1), timeout (0) or error (< 0). */
int aicwf_wifi_wait_connected(int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* AICWF_WIFI_H */
