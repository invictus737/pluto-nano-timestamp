/*
 * Nexus-BS RTOS persistent configuration: JFFS2 on the QSPI "nvmfs" region (0x120000, 896 KiB)
 * mounted at /data, the key=value store /data/nexus.conf, the USB drive drop-in (config.txt and
 * nexus2.toml) through the media server, U-Boot environment mirroring and the `config` shell
 * command. Formats and keys: rtos/config/README.md.
 *
 * Call order in Init: nexus_config_early() BEFORE rtems_bsd_initialize() (the media server must
 * be listening when the USB drive attaches at boot), then nexus_config_init() (QSPI + JFFS2
 * mount, which may take a while) in parallel with USB enumeration; drive drop-ins wait for it.
 * nexus_config_shell_register() any time.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NEXUS_CONFIG_STORE_H
#define NEXUS_CONFIG_STORE_H

#define NEXUS_DATA_DIR "/data"
#define NEXUS_CONFIG_FILE "/data/nexus.conf"
#define NEXUS_STATION_DIR "/data/nexus2"

/* mount /data (format the region if it does not mount), load /data/nexus.conf, seed the
 * build-time Wi-Fi default, start the media server; 0 when /data is persistent, -1 when the
 * configuration only lives in RAM (flash problem: logged) */
int nexus_config_init(void);
/* starts the media server only (cheap); nexus_config_init() calls it too if needed */
void nexus_config_early(void);

/* the value or NULL; the pointer stays valid for the life of the system */
const char *nexus_config_get(const char *key);
/* validated like a config.txt line; value NULL or "" = back to the default. Persists, mirrors
 * U-Boot keys, notifies. 0 ok, -1 invalid or not stored */
int nexus_config_set(const char *key, const char *value);

/* Wi-Fi network by priority: index 0 = ssid_wlan (highest), 1..4 = ssid_wlan1..4. Returns 0 and
 * the pointers (psk may be "" = open network), -1 when that slot is empty */
int nexus_config_wifi(int index, const char **ssid, const char **psk);
int nexus_uboot_env_set(const char *const *kv_pairs, int n); /* raw U-Boot variables */

/* called (from the thread that applied the change) after every applied change; up to 8 */
void nexus_config_on_change(void (*cb)(void));

/* base station files: /data/nexus2/nexus2.toml (previous version: nexus2.toml.bak) */
const char *nexus_config_station_dir(void);
/* restore nexus2.toml.bak over nexus2.toml (the station rejected the new file); 0 ok */
int nexus_config_station_rollback(void);

/* write the effective configuration (secrets masked) as <dir>/config-current.txt; dir NULL =
 * the last USB drive mounted; 0 ok */
int nexus_config_export(const char *dir);

void nexus_config_shell_register(void);

#endif
