/*
 * RT-Thread WLAN device layer (wlan_dev) for the RT-Smart AIC8800 driver on RTEMS.
 *
 * Replaces RT-Thread's wlan_dev/wlan_mgnt/wlan_prot/lwIP layers: the WLAN offload
 * framework registers its station and AP devices here; the station is started when the
 * USB attachment completes, gets the libbsd ifnet "wlan0" (if_aicwf_usb.c), and is kept
 * connected to the configured network by the aic-wlan thread (scan for the SSID, join
 * the strongest BSS with the security it advertises, retry with backoff).
 *
 * Self-heal (aic-heal thread, as a Linux USB reset + driver rebind): the stick is
 * re-enumerated (port reset, enumeration, firmware download, station start, rejoin; the same
 * as `wifi reprobe`) when WLAN_HEAL_CMD_FAILS firmware commands in a row failed, when bulk
 * OUT completed but no bulk IN for WLAN_HEAL_IN_SILENCE_MS, or when the station made no
 * progress (no scan result, join event or start) for WLAN_HEAL_STALL_MS while a network is
 * configured. Recoveries are spaced by an exponential backoff (WLAN_HEAL_BACKOFF_MIN_MS up to
 * WLAN_HEAL_BACKOFF_MAX_MS, reset after WLAN_HEAL_STABLE_MS connected); one log line each.
 * Only while aicwf_self_heal is set (default AICWF_SELF_HEAL_DEFAULT, rtconfig.h: off).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtthread.h>
#include <wlan_dev.h>
#include <wlan_offload.h>
#include <wlan_offload_control.h>

#include "aicwf_usbif.h"
#include "aicwf_wifi.h"
#include "aicwf_shim.h"

#define DBG_TAG "aicwf.wlan"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define WLAN_SCAN_MAX           64
#define WLAN_SCAN_TIMEOUT_MS    12000
#define WLAN_CONNECT_TIMEOUT_MS 30000
#define WLAN_RETRY_MIN_MS       2000
#define WLAN_RETRY_MAX_MS       30000

void aicwf_usbh_start(void);

/* D80 firmware set (rtsmart/aic8800/aic8800_firmware.c, AICWF_RTEMS):
 * 0 auto (MCU1 units get the legacy SDK V3 set), 1 SDK 5.0 only, 2 legacy only */
int aicwf_d80_fw_mode;

#define WLAN_USB_WATCHDOG_FIRST_MS  5000
#define WLAN_USB_WATCHDOG_SECOND_MS 15000

#define WLAN_HEAL_CMD_FAILS         3
#define WLAN_HEAL_IN_SILENCE_MS     30000
#define WLAN_HEAL_STALL_MS          60000
#define WLAN_HEAL_BACKOFF_MIN_MS    30000
#define WLAN_HEAL_BACKOFF_MAX_MS    300000
#define WLAN_HEAL_STABLE_MS         300000

#ifndef AICWF_SELF_HEAL_DEFAULT
#define AICWF_SELF_HEAL_DEFAULT 0
#endif
/* self-heal switch (the application may set it before or after aicwf_wifi_start()) */
int aicwf_self_heal = AICWF_SELF_HEAL_DEFAULT;

/* CherryUSB adapter (rtos/cherryusb/aic/aicwf_cherry.c); absent with the libbsd USB stack */
unsigned aicwf_usbh_in_silence_ms(void) __attribute__((weak));

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond;
static pthread_once_t g_cond_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_scan_lock = PTHREAD_MUTEX_INITIALIZER;

static struct rt_wlan_device *g_sta;    /* registered station device */
static struct rt_wlan_device *g_ap;
static rt_bool_t g_sta_ready;           /* initialised, wlan0 attached */
static int g_uses;                      /* calls in progress into g_sta / g_ifnet */
static struct aicwf_ifnet *g_ifnet;
static rt_uint8_t g_mac[6];

static rt_bool_t g_link_up;
static int g_connect_result = -1;       /* 0 pending, 1 ok, -1 failed */
static rt_bool_t g_scan_done;
static struct rt_wlan_info g_scan[WLAN_SCAN_MAX];
static int g_scan_count;

static rt_bool_t g_want;                /* credentials set */
static char g_ssid[RT_WLAN_SSID_MAX_LENGTH + 1];
static char g_psk[RT_WLAN_PASSWORD_MAX_LENGTH + 1];
static rt_uint32_t g_generation;        /* bumped on every credential change */
static rt_tick_t g_next_attempt;
static rt_uint32_t g_backoff_ms = WLAN_RETRY_MIN_MS;
static unsigned g_attempts;
static int g_last_error;
static struct rt_wlan_info g_current;

static rt_bool_t g_started;

/* self-heal (g_lock, except the command streak) */
static int g_cmd_fail_streak;           /* __atomic: firmware command threads */
static rt_tick_t g_progress;            /* last sign of a working station */
static rt_tick_t g_link_since;
static rt_tick_t g_heal_next;
static rt_uint32_t g_heal_backoff_ms = WLAN_HEAL_BACKOFF_MIN_MS;
static unsigned g_heals;

static void shim_now_deadline(struct timespec *ts, int ms)
{
    rt_shim_deadline(ts, ms);
}

static void wlan_cond_init(void)
{
    rt_shim_cond_init(&g_cond);
}

/* every entry point that can wait or signal runs this first */
static void wlan_init_once(void)
{
    pthread_once(&g_cond_once, wlan_cond_init);
}

/* take a reference on the station; g_lock held */
static struct rt_wlan_device *sta_get_locked(void)
{
    if (!g_sta || !g_sta_ready)
    {
        return RT_NULL;
    }
    g_uses++;
    return g_sta;
}

static void sta_put(void)
{
    pthread_mutex_lock(&g_lock);
    if (--g_uses == 0)
    {
        pthread_cond_broadcast(&g_cond);
    }
    pthread_mutex_unlock(&g_lock);
}

static const char *security_name(rt_wlan_security_t security)
{
    switch ((int)security)
    {
    case SECURITY_OPEN:
        return "open";
    case SECURITY_WEP_PSK:
    case SECURITY_WEP_SHARED:
        return "WEP";
    case SECURITY_WPA_TKIP_PSK:
    case SECURITY_WPA_AES_PSK:
        return "WPA-PSK";
    case SECURITY_WPA2_AES_PSK:
    case SECURITY_WPA2_TKIP_PSK:
    case SECURITY_WPA2_MIXED_PSK:
        return "WPA2-PSK";
    case SECURITY_WPA_WPA2_MIXED_PSK:
        return "WPA/WPA2-PSK";
    case SECURITY_WPA2_AES_PSK_SHA256:
        return "WPA2-PSK-SHA256";
    case SECURITY_WPA3_SAE:
    case SECURITY_WPA3_AES_PSK:
        return "WPA3-SAE";
    case SECURITY_WPA2_WPA3_MIXED_PSK:
        return "WPA2/WPA3";
    default:
        return (security & IEEE_8021X_ENABLED) ? "enterprise" : "other";
    }
}

/* ---- RT-Thread wlan_dev API used by the WLAN offload framework ---- */

rt_err_t rt_wlan_dev_register_auto(struct rt_wlan_device *wlan, const char *model_name,
                                   rt_wlan_mode_t mode, rt_wlan_transport_t transport,
                                   const struct rt_wlan_dev_ops *ops, void *user_data)
{
    wlan_init_once();
    if (!wlan || !ops || (mode != RT_WLAN_STATION && mode != RT_WLAN_AP))
    {
        return -RT_EINVAL;
    }
    memset(&wlan->device, 0, sizeof(wlan->device));
    snprintf(wlan->device.parent.name, sizeof(wlan->device.parent.name), "phy0-%s",
             mode == RT_WLAN_STATION ? "sta" : "ap");
    wlan->device.parent.type = RT_Object_Class_Device | RT_Object_Class_Static;
    wlan->device.type = RT_Device_Class_NetIf;
    wlan->device.user_data = user_data;
    memset(wlan->model_name, 0, sizeof(wlan->model_name));
    if (model_name)
    {
        strncpy(wlan->model_name, model_name, sizeof(wlan->model_name) - 1);
    }
    wlan->mode = RT_WLAN_NONE;
    wlan->registered_mode = mode;
    wlan->transport = transport;
    wlan->radio_index = 0;
    rt_list_init(&wlan->registration_list);
    rt_mutex_init(&wlan->lock, wlan->device.parent.name, RT_IPC_FLAG_PRIO);
    memset(wlan->handler_table, 0, sizeof(wlan->handler_table));
    wlan->pormisc_callback = RT_NULL;
    wlan->mgnt_filter_callback = RT_NULL;
    wlan->ops = ops;
    wlan->flags = 0;
    wlan->netdev = RT_NULL;
    wlan->prot = RT_NULL;
    wlan->user_data = user_data;

    pthread_mutex_lock(&g_lock);
    if (mode == RT_WLAN_STATION)
    {
        g_sta = wlan;
        g_sta_ready = RT_FALSE;
    }
    else
    {
        g_ap = wlan;
    }
    pthread_mutex_unlock(&g_lock);
    LOG_I("registered %s (%s)", wlan->device.parent.name, wlan->model_name);
    return RT_EOK;
}

rt_err_t rt_wlan_dev_unregister(struct rt_wlan_device *wlan)
{
    wlan_init_once();
    struct aicwf_ifnet *ifnet = RT_NULL;

    if (!wlan)
    {
        return -RT_EINVAL;
    }
    pthread_mutex_lock(&g_lock);
    if (wlan == g_sta)
    {
        g_sta_ready = RT_FALSE;
        g_link_up = RT_FALSE;
        g_connect_result = -1;
        g_scan_done = RT_TRUE;
        ifnet = g_ifnet;
        g_ifnet = RT_NULL;
        pthread_cond_broadcast(&g_cond);
    }
    pthread_mutex_unlock(&g_lock);
    if (ifnet)
    {
        /* drains the transmit task, which may be inside wlan_send() */
        aicwf_ifnet_detach(ifnet);
    }
    pthread_mutex_lock(&g_lock);
    while (wlan == g_sta && g_uses > 0)
    {
        pthread_cond_wait(&g_cond, &g_lock);
    }
    if (wlan == g_sta)
    {
        g_sta = RT_NULL;
    }
    if (wlan == g_ap)
    {
        g_ap = RT_NULL;
    }
    pthread_mutex_unlock(&g_lock);
    rt_mutex_detach(&wlan->lock);
    wlan->device.parent.type = RT_Object_Class_Null;
    wlan->device.ref_count = 0;
    wlan->ops = RT_NULL;
    wlan->mode = RT_WLAN_NONE;
    LOG_I("unregistered %s", wlan->device.parent.name);
    return RT_EOK;
}

void rt_wlan_dev_indicate_event_handle(struct rt_wlan_device *device, rt_wlan_dev_event_t event,
                                       struct rt_wlan_buff *buff)
{
    struct aicwf_ifnet *ifnet = RT_NULL;
    int link = -1;

#ifdef AICWF_SHIM_TRACE
    printf("wlan event %d on %s (sta %p dev %p)\n", (int)event,
           device ? device->device.parent.name : "?", (void *)g_sta, (void *)device);
#endif
    pthread_mutex_lock(&g_lock);
    if (device != g_sta)
    {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    switch (event)
    {
    case RT_WLAN_DEV_EVT_SCAN_REPORT:
        if (buff && buff->data && buff->len >= (rt_int32_t)sizeof(struct rt_wlan_info))
        {
            const struct rt_wlan_info *info = buff->data;
            int i;

            for (i = 0; i < g_scan_count; i++)
            {
                if (memcmp(g_scan[i].bssid, info->bssid, 6) == 0)
                {
                    break;
                }
            }
            if (i < WLAN_SCAN_MAX)
            {
                g_scan[i] = *info;
                if (i == g_scan_count)
                {
                    g_scan_count++;
                }
            }
        }
        break;
    case RT_WLAN_DEV_EVT_SCAN_DONE:
        g_scan_done = RT_TRUE;
        break;
    case RT_WLAN_DEV_EVT_CONNECT:
        g_link_up = RT_TRUE;
        g_link_since = rt_tick_get();
        g_progress = g_link_since;
        g_connect_result = 1;
        g_backoff_ms = WLAN_RETRY_MIN_MS;
        link = 1;
        break;
    case RT_WLAN_DEV_EVT_CONNECT_FAIL:
        g_connect_result = -1;
        g_progress = rt_tick_get();
        break;
    case RT_WLAN_DEV_EVT_DISCONNECT:
        if (g_link_up)
        {
            LOG_W("disconnected from %s", g_current.ssid.val);
        }
        g_link_up = RT_FALSE;
        if (g_connect_result == 0)
        {
            g_connect_result = -1;
        }
        g_progress = rt_tick_get();
        g_next_attempt = rt_tick_get() + WLAN_RETRY_MIN_MS;
        link = 0;
        break;
    default:
        break;
    }
    if (link >= 0 && g_ifnet)
    {
        ifnet = g_ifnet;
        g_uses++;
    }
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    if (ifnet)
    {
        aicwf_ifnet_link(ifnet, link);
        sta_put();
    }
    if (link == 1)
    {
        LOG_I("connected to %s (channel %d, %d dBm)", g_current.ssid.val,
              g_current.channel, g_current.rssi);
    }
}

rt_err_t rt_wlan_dev_report_data(struct rt_wlan_device *device, void *buff, int len)
{
    struct aicwf_ifnet *ifnet = RT_NULL;
    int result;

    pthread_mutex_lock(&g_lock);
    if (device == g_sta && g_ifnet)
    {
        ifnet = g_ifnet;
        g_uses++;
    }
    pthread_mutex_unlock(&g_lock);
    if (!ifnet)
    {
        return -RT_EIO;
    }
    result = aicwf_ifnet_input(ifnet, buff, (uint32_t)len);
    sta_put();
    return result == 0 ? RT_EOK : -RT_ERROR;
}

void rt_wlan_dev_promisc_handler(struct rt_wlan_device *device, void *data, int len)
{
    (void)device;
    (void)data;
    (void)len;
}

void rt_wlan_dev_mgnt_filter_handler(struct rt_wlan_device *device, void *data, int len)
{
    (void)device;
    (void)data;
    (void)len;
}

/* /dev/wlanctl (userspace supplicant control) does not exist on RTEMS: the embedded
 * supplicant handles Personal security; other control events are dropped. */
rt_err_t rt_wlan_offload_control_register(struct rt_wlan_offload_radio *radio, const char *name)
{
    (void)radio;
    (void)name;
    return RT_EOK;
}

rt_err_t rt_wlan_offload_control_unregister(struct rt_wlan_offload_radio *radio)
{
    (void)radio;
    return RT_EOK;
}

void rt_wlan_offload_control_report_event(struct rt_wlan_offload_radio *radio,
                                          const struct rt_wlan_offload_event *event)
{
    (void)radio;
    (void)event;
}

rt_err_t rt_wlan_offload_control_get_name(const struct rt_wlan_offload_radio *radio, char *name,
                                          rt_size_t size)
{
    (void)radio;
    (void)name;
    (void)size;
    return -RT_ENOSYS;
}

/* ---- station bring-up, called on the aic-usbh thread after attach ---- */

static int ifnet_tx(void *arg, const void *frame, uint32_t len)
{
    struct rt_wlan_device *wlan;
    int result;

    (void)arg;
    pthread_mutex_lock(&g_lock);
    wlan = sta_get_locked();
    pthread_mutex_unlock(&g_lock);
    if (!wlan)
    {
        return -1;
    }
    result = wlan->ops->wlan_send(wlan, (void *)frame, (int)len);
    sta_put();
    return result == RT_EOK ? 0 : -1;
}

void aicwf_wlan_attach_done(void)
{
    struct rt_wlan_device *wlan;
    struct aicwf_ifnet *ifnet;
    rt_uint8_t mac[6];
    rt_err_t result;

    pthread_mutex_lock(&g_lock);
    wlan = g_sta;
    if (!wlan || g_sta_ready)
    {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_uses++;
    pthread_mutex_unlock(&g_lock);

    result = wlan->ops->wlan_init(wlan);
    if (result == RT_EOK)
    {
        result = wlan->ops->wlan_mode(wlan, RT_WLAN_STATION);
    }
    if (result == RT_EOK)
    {
        memset(mac, 0, sizeof(mac));
        result = wlan->ops->wlan_get_mac(wlan, mac);
    }
    if (result != RT_EOK)
    {
        LOG_E("station start failed: %d", result);
        pthread_mutex_lock(&g_lock);
        g_last_error = result;
        pthread_mutex_unlock(&g_lock);
        sta_put();
        return;
    }
    wlan->mode = RT_WLAN_STATION;
    ifnet = aicwf_ifnet_attach(AICWF_WIFI_IFNAME, mac, ifnet_tx, RT_NULL);
    pthread_mutex_lock(&g_lock);
    memcpy(g_mac, mac, sizeof(g_mac));
    g_ifnet = ifnet;
    g_sta_ready = ifnet != RT_NULL;
    g_link_up = RT_FALSE;
    g_next_attempt = rt_tick_get();
    g_backoff_ms = WLAN_RETRY_MIN_MS;
    g_progress = rt_tick_get();
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    if (!ifnet)
    {
        LOG_E("cannot create %s", AICWF_WIFI_IFNAME);
    }
    sta_put();
}

/* ---- scanning and joining ---- */

/* returns the number of results, < 0 on error; g_scan_lock held by the caller */
static int do_scan(const char *ssid, int timeout_ms)
{
    struct rt_scan_info info;
    struct rt_wlan_device *wlan;
    struct timespec deadline;
    rt_err_t result;
    int count;

    pthread_mutex_lock(&g_lock);
    wlan = sta_get_locked();
    if (wlan)
    {
        g_scan_count = 0;
        g_scan_done = RT_FALSE;
    }
    pthread_mutex_unlock(&g_lock);
    if (!wlan)
    {
        return -RT_EIO;
    }
    memset(&info, 0, sizeof(info));
    info.channel_min = -1;
    info.channel_max = -1;
    info.band = RT_802_11_BAND_UNKNOWN;
    if (ssid && ssid[0])
    {
        info.ssid.len = (rt_uint8_t)strnlen(ssid, RT_WLAN_SSID_MAX_LENGTH);
        memcpy(info.ssid.val, ssid, info.ssid.len);
    }
    result = wlan->ops->wlan_scan(wlan, &info);
    sta_put();
    if (result != RT_EOK)
    {
        return result;
    }
    shim_now_deadline(&deadline, timeout_ms);
    pthread_mutex_lock(&g_lock);
    while (!g_scan_done)
    {
        if (pthread_cond_timedwait(&g_cond, &g_lock, &deadline) == ETIMEDOUT)
        {
            break;
        }
    }
    count = g_scan_done ? g_scan_count : -RT_ETIMEOUT;
    if (count >= 0)
    {
        g_progress = rt_tick_get(); /* the firmware answers */
    }
    pthread_mutex_unlock(&g_lock);
    return count;
}

static rt_err_t do_join(rt_uint32_t generation)
{
    struct rt_wlan_device *wlan;
    struct rt_sta_info sta;
    struct rt_wlan_info best;
    struct timespec deadline;
    char ssid[RT_WLAN_SSID_MAX_LENGTH + 1];
    char psk[RT_WLAN_PASSWORD_MAX_LENGTH + 1];
    int count, i, found = 0;
    rt_err_t result;

    pthread_mutex_lock(&g_lock);
    memcpy(ssid, g_ssid, sizeof(ssid));
    memcpy(psk, g_psk, sizeof(psk));
    pthread_mutex_unlock(&g_lock);

    pthread_mutex_lock(&g_scan_lock);
    count = do_scan(ssid, WLAN_SCAN_TIMEOUT_MS);
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < count && i < g_scan_count; i++)
    {
        if (g_scan[i].ssid.len == strlen(ssid) &&
            memcmp(g_scan[i].ssid.val, ssid, g_scan[i].ssid.len) == 0 &&
            (!found || g_scan[i].rssi > best.rssi))
        {
            best = g_scan[i];
            found = 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_scan_lock);
    if (count < 0)
    {
        LOG_W("scan for %s failed: %d", ssid, count);
        return count;
    }
    if (!found)
    {
        LOG_W("network %s not found (%d networks seen)", ssid, count);
        return -RT_EEMPTY;
    }
    if (best.security != SECURITY_OPEN && !psk[0])
    {
        LOG_E("%s needs a passphrase (%s)", ssid, security_name(best.security));
        return -RT_EINVAL;
    }
    memset(&sta, 0, sizeof(sta));
    sta.ssid = best.ssid;
    sta.key.len = (rt_uint8_t)strlen(psk);
    memcpy(sta.key.val, psk, sta.key.len);
    memcpy(sta.bssid, best.bssid, sizeof(sta.bssid));
    sta.channel = (rt_uint16_t)best.channel;
    sta.band = best.band;
    sta.security = best.security;
    LOG_I("joining %s %02x:%02x:%02x:%02x:%02x:%02x channel %d %d dBm %s",
          ssid, best.bssid[0], best.bssid[1], best.bssid[2], best.bssid[3], best.bssid[4],
          best.bssid[5], best.channel, best.rssi, security_name(best.security));

    pthread_mutex_lock(&g_lock);
    if (generation != g_generation)
    {
        pthread_mutex_unlock(&g_lock);
        return -RT_EINTR;
    }
    wlan = sta_get_locked();
    g_connect_result = 0;
    g_current = best;
    g_attempts++;
    pthread_mutex_unlock(&g_lock);
    if (!wlan)
    {
        return -RT_EIO;
    }
    result = wlan->ops->wlan_join(wlan, &sta);
    sta_put();
    memset(&sta.key, 0, sizeof(sta.key));
    memset(psk, 0, sizeof(psk));
    if (result != RT_EOK)
    {
        LOG_W("join request failed: %d", result);
        return result;
    }
    shim_now_deadline(&deadline, WLAN_CONNECT_TIMEOUT_MS);
    pthread_mutex_lock(&g_lock);
    while (g_connect_result == 0 && generation == g_generation)
    {
        if (pthread_cond_timedwait(&g_cond, &g_lock, &deadline) == ETIMEDOUT)
        {
            break;
        }
    }
    result = g_connect_result == 1 ? RT_EOK : -RT_ETIMEOUT;
    if (g_connect_result == -1)
    {
        result = -RT_ERROR;
    }
    pthread_mutex_unlock(&g_lock);
    if (result != RT_EOK)
    {
        LOG_W("association with %s failed: %d", ssid, result);
    }
    return result;
}

static void wlan_thread(void *parameter)
{
    (void)parameter;
    pthread_mutex_lock(&g_lock);
    for (;;)
    {
        rt_int32_t wait_ms;
        rt_uint32_t generation;
        rt_err_t result;

        if (!g_want || !g_sta || !g_sta_ready || g_link_up || g_connect_result == 0)
        {
            pthread_cond_wait(&g_cond, &g_lock);
            continue;
        }
        wait_ms = (rt_int32_t)(g_next_attempt - rt_tick_get());
        if (wait_ms > 0)
        {
            struct timespec deadline;

            shim_now_deadline(&deadline, wait_ms);
            pthread_cond_timedwait(&g_cond, &g_lock, &deadline);
            continue;
        }
        generation = g_generation;
        g_connect_result = 0;
        pthread_mutex_unlock(&g_lock);
        result = do_join(generation);
        pthread_mutex_lock(&g_lock);
        g_last_error = result;
        if (result != RT_EOK)
        {
            if (g_connect_result == 0)
            {
                g_connect_result = -1;
            }
            g_next_attempt = rt_tick_get() + g_backoff_ms;
            g_backoff_ms = g_backoff_ms * 2 > WLAN_RETRY_MAX_MS ? WLAN_RETRY_MAX_MS
                                                              : g_backoff_ms * 2;
        }
    }
}

/* ---- self-heal ---- */

/* every firmware command result (rtsmart/aic8800/aic8800_wifi.c aic8800_protocol_command) */
void aicwf_wifi_command_result(int result)
{
    if (result == RT_EOK)
    {
        __atomic_store_n(&g_cmd_fail_streak, 0, __ATOMIC_RELAXED);
    }
    else if (result != -RT_EINVAL)
    {
        __atomic_add_fetch(&g_cmd_fail_streak, 1, __ATOMIC_RELAXED);
    }
}

static void heal_thread(void *parameter)
{
    (void)parameter;
    for (;;)
    {
        const char *reason = RT_NULL;
        unsigned silence = 0, heal;
        rt_uint32_t next_s;
        rt_tick_t now;
        int streak, result;

        rt_thread_mdelay(1000);
        if (!__atomic_load_n(&aicwf_self_heal, __ATOMIC_RELAXED))
        {
            continue;
        }
        if (aicwf_usbh_attach_count() == 0)
        {
            continue; /* no stick yet: usb_watchdog_thread's business */
        }
        streak = __atomic_load_n(&g_cmd_fail_streak, __ATOMIC_RELAXED);
        if (aicwf_usbh_in_silence_ms)
        {
            silence = aicwf_usbh_in_silence_ms();
        }
        now = rt_tick_get();
        pthread_mutex_lock(&g_lock);
        if (g_link_up && (rt_int32_t)(now - g_link_since) >= WLAN_HEAL_STABLE_MS)
        {
            g_heal_backoff_ms = WLAN_HEAL_BACKOFF_MIN_MS;
        }
        if (streak >= WLAN_HEAL_CMD_FAILS)
        {
            reason = "firmware commands failing";
        }
        else if (g_sta_ready && silence >= WLAN_HEAL_IN_SILENCE_MS)
        {
            reason = "bulk OUT without any bulk IN";
        }
        else if (g_want && !g_link_up && (rt_int32_t)(now - g_progress) >= WLAN_HEAL_STALL_MS)
        {
            reason = g_sta_ready ? "no scan/join progress" : "station did not start";
        }
        if (!reason || (rt_int32_t)(now - g_heal_next) < 0)
        {
            pthread_mutex_unlock(&g_lock);
            continue;
        }
        heal = ++g_heals;
        next_s = g_heal_backoff_ms / 1000U;
        g_heal_next = now + g_heal_backoff_ms;
        g_heal_backoff_ms = g_heal_backoff_ms * 2 > WLAN_HEAL_BACKOFF_MAX_MS ?
                                WLAN_HEAL_BACKOFF_MAX_MS : g_heal_backoff_ms * 2;
        g_progress = now; /* the re-enumeration gets its own WLAN_HEAL_STALL_MS */
        pthread_mutex_unlock(&g_lock);
        __atomic_store_n(&g_cmd_fail_streak, 0, __ATOMIC_RELAXED);
        result = aicwf_usbh_reprobe();
        LOG_W("self-heal #%u: %s (commands failed %d, bulk IN silent %u ms): re-enumerating "
              "the stick (%d); next at the earliest in %u s",
              heal, reason, streak, silence, result, (unsigned)next_s);
    }
}

/* ---- application API (aicwf_wifi.h) ---- */

/*
 * At a cold boot with the stick already plugged in, the controller has been seen to miss
 * it until a replug. If no AIC interface has attached a few seconds after the driver
 * started, power-cycle the empty root ports and re-explore (twice at most).
 */
static void usb_watchdog_thread(void *parameter)
{
    int round;

    (void)parameter;
    for (round = 0; round < 2; round++)
    {
        rt_thread_mdelay(round == 0 ? WLAN_USB_WATCHDOG_FIRST_MS :
                                      WLAN_USB_WATCHDOG_SECOND_MS - WLAN_USB_WATCHDOG_FIRST_MS);
        if (aicwf_usbh_attach_count() != 0)
        {
            return;
        }
        LOG_W("no AIC8800 interface %d s after start: power-cycling empty USB ports",
              round == 0 ? WLAN_USB_WATCHDOG_FIRST_MS / 1000 : WLAN_USB_WATCHDOG_SECOND_MS / 1000);
        LOG_W("USB rescan: %d port(s) cycled", aicwf_usb_rescan(0));
    }
}

int aicwf_wifi_set_debug(int on)
{
    rt_shim_log_level = on ? DBG_LOG : -1;
    return 0;
}

int aicwf_wifi_set_fw_mode(int mode)
{
    if (mode < 0 || mode > 2)
    {
        return -RT_EINVAL;
    }
    aicwf_d80_fw_mode = mode;
    return 0;
}

void aicwf_wifi_fwdiag(void)
{
    static const char *const modes[] = { "auto (MCU1 -> legacy SDK V3)", "SDK 5.0", "legacy SDK V3" };

    printf("wifi: D80 firmware set: %s; log level %s; self-heal %s, %u done (firmware command "
           "failure streak %d)\n", modes[aicwf_d80_fw_mode % 3],
           rt_shim_log_level >= DBG_LOG ? "debug" : "normal", aicwf_self_heal ? "on" : "off", g_heals,
           __atomic_load_n(&g_cmd_fail_streak, __ATOMIC_RELAXED));
    aicwf_fw_dump();
    aicwf_usbh_dump();
}

int aicwf_wifi_usb_rescan(void)
{
    return aicwf_usb_rescan(0); /* explore only: a port power cycle from this thread hung the shell */
}

int aicwf_wifi_reprobe(void)
{
    return aicwf_usbh_reprobe();
}

int aicwf_wifi_start(void)
{
    wlan_init_once();
    rt_thread_t thread;

    pthread_mutex_lock(&g_lock);
    if (g_started)
    {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    g_started = RT_TRUE;
    pthread_mutex_unlock(&g_lock);
    if (aicwf_fw_install() != 0)
    {
        LOG_W("firmware files incomplete; a stick needing a download will not start");
    }
    thread = rt_thread_create("aic-wlan", wlan_thread, RT_NULL, 16384, 16, 0);
    if (!thread || rt_thread_startup(thread) != RT_EOK)
    {
        LOG_E("station thread failed to start");
        return -1;
    }
    aicwf_usbh_start();
    thread = rt_thread_create("aic-usbw", usb_watchdog_thread, RT_NULL, 8192, 20, 0);
    if (!thread || rt_thread_startup(thread) != RT_EOK)
    {
        LOG_W("USB watchdog thread failed to start");
    }
    thread = rt_thread_create("aic-heal", heal_thread, RT_NULL, 8192, 20, 0);
    if (!thread || rt_thread_startup(thread) != RT_EOK)
    {
        LOG_W("self-heal thread failed to start");
    }
    return 0;
}

int aicwf_wifi_connect(const char *ssid, const char *psk)
{
    wlan_init_once();
    size_t ssid_len = ssid ? strlen(ssid) : 0;
    size_t psk_len = psk ? strlen(psk) : 0;

    if (!ssid_len || ssid_len > RT_WLAN_SSID_MAX_LENGTH || psk_len > RT_WLAN_PASSWORD_MAX_LENGTH ||
        (psk_len && psk_len < 8))
    {
        return -RT_EINVAL;
    }
    pthread_mutex_lock(&g_lock);
    memset(g_ssid, 0, sizeof(g_ssid));
    memset(g_psk, 0, sizeof(g_psk));
    memcpy(g_ssid, ssid, ssid_len);
    if (psk_len)
    {
        memcpy(g_psk, psk, psk_len);
    }
    g_want = RT_TRUE;
    g_generation++;
    g_progress = rt_tick_get();
    g_next_attempt = rt_tick_get();
    g_backoff_ms = WLAN_RETRY_MIN_MS;
    if (g_connect_result == 0)
    {
        g_connect_result = -1;
    }
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int aicwf_wifi_disconnect(void)
{
    wlan_init_once();
    struct rt_wlan_device *wlan;
    rt_err_t result = RT_EOK;

    pthread_mutex_lock(&g_lock);
    g_want = RT_FALSE;
    g_generation++;
    memset(g_psk, 0, sizeof(g_psk));
    wlan = g_link_up ? sta_get_locked() : RT_NULL;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    if (wlan)
    {
        result = wlan->ops->wlan_disconnect(wlan);
        sta_put();
    }
    return result;
}

int aicwf_wifi_scan(struct aicwf_wifi_bss *out, int max, int timeout_ms)
{
    wlan_init_once();
    int count, i;

    if (!out || max <= 0)
    {
        return -RT_EINVAL;
    }
    pthread_mutex_lock(&g_scan_lock);
    count = do_scan(RT_NULL, timeout_ms > 0 ? timeout_ms : WLAN_SCAN_TIMEOUT_MS);
    if (count > 0)
    {
        pthread_mutex_lock(&g_lock);
        for (i = 0; i < count && i < max && i < g_scan_count; i++)
        {
            const struct rt_wlan_info *info = &g_scan[i];
            struct aicwf_wifi_bss *bss = &out[i];

            memset(bss, 0, sizeof(*bss));
            memcpy(bss->ssid, info->ssid.val,
                   info->ssid.len > RT_WLAN_SSID_MAX_LENGTH ? RT_WLAN_SSID_MAX_LENGTH
                                                            : info->ssid.len);
            memcpy(bss->bssid, info->bssid, sizeof(bss->bssid));
            bss->channel = info->channel;
            bss->rssi = info->rssi;
            bss->band_5ghz = info->band == RT_802_11_BAND_5GHZ;
            bss->security = (uint32_t)info->security;
            bss->security_name = security_name(info->security);
        }
        count = i;
        pthread_mutex_unlock(&g_lock);
    }
    pthread_mutex_unlock(&g_scan_lock);
    return count;
}

int aicwf_wifi_status(struct aicwf_wifi_status *status)
{
    wlan_init_once();
    if (!status)
    {
        return -RT_EINVAL;
    }
    memset(status, 0, sizeof(*status));
    pthread_mutex_lock(&g_lock);
    status->device_present = g_sta != RT_NULL;
    status->station_ready = g_sta_ready;
    status->connected = g_link_up;
    status->want_connected = g_want;
    memcpy(status->ssid, g_link_up ? (const char *)g_current.ssid.val : g_ssid,
           sizeof(status->ssid) - 1);
    status->rssi = g_link_up ? g_current.rssi : 0;
    status->channel = g_link_up ? g_current.channel : 0;
    memcpy(status->mac, g_mac, sizeof(status->mac));
    status->connect_attempts = g_attempts;
    status->last_error = g_last_error;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int aicwf_wifi_wait_connected(int timeout_ms)
{
    wlan_init_once();
    struct timespec deadline;
    int result;

    shim_now_deadline(&deadline, timeout_ms > 0 ? timeout_ms : 0);
    pthread_mutex_lock(&g_lock);
    while (!g_link_up && timeout_ms > 0)
    {
        if (pthread_cond_timedwait(&g_cond, &g_lock, &deadline) == ETIMEDOUT)
        {
            break;
        }
    }
    result = g_link_up ? 1 : 0;
    pthread_mutex_unlock(&g_lock);
    return result;
}
