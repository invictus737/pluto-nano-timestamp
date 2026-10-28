/*
 * Internal glue between the RTEMS shim files of the AIC8800 port.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AICWF_SHIM_H
#define AICWF_SHIM_H

/* usbh_shim.c: a class driver's connect() returned success (aic-usbh thread) */
void aicwf_wlan_attach_done(void);
/* aicwf_fw_files.c: publish the embedded firmware under AIC8800_WIFI_FIRMWARE_PATH */
int aicwf_fw_install(void);

#endif /* AICWF_SHIM_H */
