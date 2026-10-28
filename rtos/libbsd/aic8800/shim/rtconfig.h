/*
 * Build configuration of the RT-Smart AIC8800 driver on RTEMS/libbsd (the Kconfig
 * values of rtsmart/aic8800/Kconfig and the RT-Thread WLAN offload framework).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AICWF_RTCONFIG_H
#define AICWF_RTCONFIG_H

#define RT_NAME_MAX                     16
#define RT_ALIGN_SIZE                   8
#define RT_TICK_PER_SECOND              1000
#define RT_USING_HEAP
#define RT_USING_SYSTEM_WORKQUEUE
#define RT_SYSTEM_WORKQUEUE_STACKSIZE   8192
#define RT_SYSTEM_WORKQUEUE_PRIORITY    23
#define RT_USING_WIFI

/* WLAN offload framework: station with the embedded WPA/WPA2/WPA3-Personal supplicant.
 * The AIC driver declares a control device; its /dev/wlanctl implementation is replaced by
 * the stubs in wlan_dev_shim.c (no userspace supplicant on RTEMS). Not built: the embedded
 * hostapd and RT-Thread's WLAN management, protocol (lwIP) and work-thread layers. */
#define RT_WLAN_OFFLOAD
#define RT_WLAN_OFFLOAD_EMBEDDED_WPA2
#define RT_WLAN_OFFLOAD_CONTROL

/* AIC8800 driver (Kconfig defaults unless noted) */
#define RT_USING_AIC8800_WIFI
#define AIC8800_WIFI_TRANSPORT_USB
/* the driver's bulk OUT recovery calls usbh_dwc2_reset_endpoint(); usbh_shim.c maps it to
 * the libbsd stall clearing */
#define CHERRY_USB_HC_DRV_DWC2
#define AIC8800_WIFI_FIRMWARE_D80
#define AIC8800_WIFI_FIRMWARE_DC_DW
#ifndef AIC8800_WIFI_FIRMWARE_PATH
#define AIC8800_WIFI_FIRMWARE_PATH              "/bin/firmware"
#endif
/* the driver knows CN and US tables only; CN allows 2.4 GHz channels 1-13 as in the EU */
#define AIC8800_WIFI_COUNTRY_CODE               "CN"
#define AIC8800_WIFI_5GHZ
#define AIC8800_WIFI_COMMAND_TIMEOUT_MS         5000
#define AIC8800_WIFI_MGMT_CONFIRM_TIMEOUT_MS    1000
#define AIC8800_WIFI_TX_BUFFER_SIZE             2048
#define AIC8800_WIFI_RX_REORDER_FLOWS           128
#define AIC8800_WIFI_RX_REORDER_SLOTS           128
#define AIC8800_WIFI_RX_REORDER_WINDOW          64
#define AIC8800_WIFI_RX_REORDER_TIMEOUT_MS      50
#define AIC8800_WIFI_TCP_ACK_FILTER
#define AIC8800_WIFI_THERMAL_POLL
#define AIC8800_WIFI_THERMAL_CONTROL
#define AIC8800_WIFI_FORCE_FIRMWARE_DOWNLOAD
#define AIC8800_WIFI_RX_BUFFER_SIZE             2048
#define AIC8800_WIFI_DATA_RX_URBS               20
#define AIC8800_WIFI_MESSAGE_RX_URBS            20
#define AIC8800_WIFI_DATA_TX_URBS               2
#define AIC8800_WIFI_USB_TX_AGGREGATE_FRAMES    10
#define AIC8800_WIFI_USB_TX_AGGREGATE_SIZE      16384
#define AIC8800_WIFI_USB_TX_QUEUE_DEPTH         64
#define AIC8800_WIFI_USB_TX_AGGREGATE_WAIT_MS   1
#define AIC8800_WIFI_USB_TX_THREAD_STACK_SIZE   8192            /* Kconfig 4096 */
#define AIC8800_WIFI_USB_TX_THREAD_PRIORITY     14
#define AIC8800_WIFI_TX_WAIT_MS                 100
#define AIC8800_WIFI_TX_WATCHDOG_MS             1000
#define AIC8800_WIFI_RX_RECOVERY_ERRORS         64
#define AIC8800_WIFI_BLE
#define AIC8800_BT_HCI_RX_BUFFER_SIZE           8192
#define AIC8800_BT_USB_RX_BUFFER_SIZE           4096
#define AIC8800_BT_RX_THREAD_STACK_SIZE         8192            /* Kconfig 4096 */
#define AIC8800_BT_RX_THREAD_PRIORITY           15
#define AIC8800_WIFI_RX_THREAD_STACK_SIZE       16384           /* Kconfig 12288 */
#define AIC8800_WIFI_RX_THREAD_PRIORITY         12
#define AIC8800_WIFI_ATTACH_THREAD_STACK_SIZE   16384           /* Kconfig 12288 */
#define AIC8800_WIFI_ATTACH_THREAD_PRIORITY     14
#define AIC8800_WIFI_LOG_LEVEL_INFO
/* The RT-Smart transport assumes a single-core build (README "requires a single-core
 * RT-Thread build"): every thread of the port runs on this CPU of the SMP system. The
 * global "interrupt" lock also serialises the critical sections with the libbsd side. */
#define AICWF_THREAD_CPU                        0
/*
 * TX power: the userconfig files are capped at 15 dBm by ../fetch-firmware.sh (as the
 * Linux image does); the country table is not used as a hard cap (Kconfig default).
 */

#endif /* AICWF_RTCONFIG_H */
