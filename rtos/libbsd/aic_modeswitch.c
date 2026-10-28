/*
 * AICSemi AIC8800 USB Wi-Fi/Bluetooth adapters start as a "driver CD" mass-storage device.
 * Before umass can claim it, send the switch command; the stick re-enumerates as the real
 * Wi-Fi/BT device (a69c:8d80/8d81, 368b:8d81, a69c:88dc). Same mechanism as FreeBSD's u3g
 * auto-install hook (usb_dev_configured event, UAA_DEV_EJECTING), same commands as the Linux
 * firmware's usb_modeswitch.d entries:
 *   a69c:5721..572f  StandardEject (ALLOW MEDIUM REMOVAL, START STOP UNIT eject)
 *   1111:1111        AIC8800D80 "Pandora" clone: vendor CDBs FD..F3 then FD..F2
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <machine/rtems-bsd-kernel-space.h>

#include <sys/stdint.h>
#include <sys/stddef.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sysctl.h>
#include <sys/sx.h>
#include <sys/unistd.h>
#include <sys/callout.h>
#include <sys/malloc.h>
#include <sys/priv.h>
#include <sys/eventhandler.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_msctest.h>

#define AIC_VID 0xa69c
#define PANDORA_VID 0x1111
#define PANDORA_PID 0x1111

static const uint8_t cdb_allow_removal[6] = { 0x1e, 0, 0, 0, 0, 0 };
static const uint8_t cdb_stop_eject[6] = { 0x1b, 0, 0, 0, 0x02, 0 };
static const uint8_t cdb_pandora_f3[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0xf3 };
static const uint8_t cdb_pandora_f2[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0xf2 };

static eventhandler_tag aicms_etag;

static int
aicms_is_storage_mode(uint16_t vid, uint16_t pid)
{
	if (vid == PANDORA_VID && pid == PANDORA_PID)
		return (1);
	return (vid == AIC_VID && pid >= 0x5721 && pid <= 0x572f);
}

static void
aicms_autoinst(void *arg, struct usb_device *udev, struct usb_attach_arg *uaa)
{
	struct usb_interface *iface;
	usb_error_t err;
	uint16_t vid, pid;

	(void)arg;
	if (uaa->dev_state != UAA_DEV_READY)
		return;
	vid = UGETW(udev->ddesc.idVendor);
	pid = UGETW(udev->ddesc.idProduct);
	if (!aicms_is_storage_mode(vid, pid))
		return;
	iface = usbd_get_iface(udev, 0);
	if (iface == NULL || iface->idesc == NULL ||
	    iface->idesc->bInterfaceClass != UICLASS_MASS)
		return;

	if (vid == PANDORA_VID) {
		err = usb_msc_eject_cdb(udev, 0, cdb_pandora_f3, sizeof(cdb_pandora_f3));
		if (err == 0)
			err = usb_msc_eject_cdb(udev, 0, cdb_pandora_f2,
			    sizeof(cdb_pandora_f2));
	} else {
		(void)usb_msc_eject_cdb(udev, 0, cdb_allow_removal,
		    sizeof(cdb_allow_removal));
		err = usb_msc_eject_cdb(udev, 0, cdb_stop_eject, sizeof(cdb_stop_eject));
	}
	printf("aic8800: %04x:%04x storage mode, switch command %s\n", vid, pid,
	    err == 0 ? "sent" : usbd_errstr(err));
	if (err == 0)
		uaa->dev_state = UAA_DEV_EJECTING;
}

static int
aicms_probe(device_t dev)
{
	(void)dev;
	return (ENXIO);
}

static int
aicms_loaded(struct module *mod, int what, void *arg)
{
	(void)mod;
	(void)arg;
	switch (what) {
	case MOD_LOAD:
		aicms_etag = EVENTHANDLER_REGISTER(usb_dev_configured,
		    aicms_autoinst, NULL, EVENTHANDLER_PRI_ANY);
		break;
	case MOD_UNLOAD:
		EVENTHANDLER_DEREGISTER(usb_dev_configured, aicms_etag);
		break;
	default:
		return (EOPNOTSUPP);
	}
	return (0);
}

static device_method_t aicms_methods[] = {
	DEVMETHOD(device_probe, aicms_probe),
	DEVMETHOD_END
};

static driver_t aicms_driver = {
	.name = "aicms",
	.methods = aicms_methods,
	.size = 0,
};

DRIVER_MODULE(aicms, uhub, aicms_driver, aicms_loaded, NULL);
MODULE_DEPEND(aicms, usb, 1, 1, 1);
MODULE_VERSION(aicms, 1);
