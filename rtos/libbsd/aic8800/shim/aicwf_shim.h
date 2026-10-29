/*
 * Internal glue between the RTEMS shim files of the AIC8800 port.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AICWF_SHIM_H
#define AICWF_SHIM_H

/* usbh_shim.c: a class driver's connect() returned success (aic-usbh thread) */
void aicwf_wlan_attach_done(void);
/* usbh_shim.c: diagnostics */
unsigned aicwf_usbh_attach_count(void);
void aicwf_usbh_dump(void);
int aicwf_usbh_reprobe(void);
/* VID:PID of the first attached AIC device; 0 when none */
int aicwf_usbh_first_id(rt_uint16_t *vid, rt_uint16_t *pid);
/*
 * Recovery-chain timing (wlan_dev_shim.c): each stage is stamped once per chain; the chain
 * opens at LOST (the station's device detached) or PLUG (an AIC device arrived with none
 * open) and one line with every step is printed when wlan0 gets its address.
 */
enum aicwf_timing_stage {
    AICWF_T_LOST,       /* station device detached (USB reset, unplug) */
    AICWF_T_PLUG,       /* first AIC interface of the chain connected (storage, loader or runtime) */
    AICWF_T_FW_START,   /* firmware download started (loader) */
    AICWF_T_FW_DONE,    /* firmware started, waiting for the runtime device */
    AICWF_T_RUNTIME,    /* runtime device attached (WLAN offload radio registered) */
    AICWF_T_STATION,    /* station started, wlan0 created */
    AICWF_T_SCAN,       /* the network was found */
    AICWF_T_ASSOC,      /* associated, keys installed (4-way done): link up */
    AICWF_T_DHCP,       /* wlan0 got its IPv4 address */
    AICWF_T_COUNT
};
void aicwf_timing_mark(int stage);
/* cherryusb/aic/aicwf_cherry.c (weak): the USB host is resetting/restarting the controller or
 * re-enumerating a device; the self-heal waits */
int aicwf_usbh_busy(void) __attribute__((weak));
/* rtsmart/aic8800/aic8800_firmware.c: the runtime attached to the firmware of this boot that was
 * still running (no loader); drop_warm makes the next runtime attach reload it */
int aic8800_firmware_warm_attach(void);
void aic8800_firmware_drop_warm(void);

/* aicwf_fw_files.c: "wifi fwdiag" listing */
void aicwf_fw_dump(void);
/* aicwf_fw_files.c: publish the embedded firmware under AIC8800_WIFI_FIRMWARE_PATH */
int aicwf_fw_install(void);

#endif /* AICWF_SHIM_H */
