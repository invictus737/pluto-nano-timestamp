/* host check stand-in for rtos/app/config_store.h: same API, /data under $NEXUS_BT_TEST_DATA */
#ifndef NEXUS_CONFIG_STORE_H
#define NEXUS_CONFIG_STORE_H
extern char host_data_dir[];
#define NEXUS_DATA_DIR host_data_dir
const char *nexus_config_get(const char *key);
int nexus_config_set(const char *key, const char *value);
#endif
