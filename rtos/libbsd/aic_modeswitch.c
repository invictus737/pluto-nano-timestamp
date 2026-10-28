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
#include <dev/usb/usb_hub.h>

#define AIC_VID 0xa69c
#define PANDORA_VID 0x1111
#define PANDORA_PID 0x1111

static const uint8_t cdb_allow_removal[6] = { 0x1e, 0, 0, 0, 0, 0 };
static const uint8_t cdb_stop_eject[6] = { 0x1b, 0, 0, 0, 0x02, 0 };
static const uint8_t cdb_test_unit_ready[6] = { 0, 0, 0, 0, 0, 0 };
static const uint8_t cdb_pandora_f3[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0xf3 };
static const uint8_t cdb_pandora_f2[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0xf2 };

static eventhandler_tag aicms_etag;

/*
 * Mode switch as the AIC vendor tooling does it (sequence decoded by the RT-Smart K230 port,
 * canmv-k230/rtsmart aic8800_usb.c): the fake-storage firmware ignores bulk-only commands until
 * the MSC class init (GET_MAX_LUN), wants to see the medium probed, then switches on the private
 * 16-byte CDB FD..F3 (identify, 5 bytes) followed by FD..F2 ("Set_CS1_0"); a plain eject leaves
 * a DC stick unusable. The device then needs a port re-enumeration. Total well under a second.
 * The eject stays only as the fallback for firmware that does not answer the private command.
 */
static const uint8_t cdb_inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
static const uint8_t cdb_read_capacity[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t cdb_stop_eject_immed[6] = { 0x1b, 0x01, 0, 0, 0x02, 0 };

static usb_error_t
aicms_get_max_lun(struct usb_device *udev, uint8_t ifno)
{
	struct usb_device_request req;
	uint8_t lun = 0;

	req.bmRequestType = UT_READ_CLASS_INTERFACE;
	req.bRequest = 0xfe; /* bulk-only GET_MAX_LUN */
	USETW(req.wValue, 0);
	req.wIndex[0] = ifno;
	req.wIndex[1] = 0;
	USETW(req.wLength, 1);
	return (usbd_do_request(udev, NULL, &req, &lun));
}

static usb_error_t
aicms_switch(struct usb_device *udev, uint8_t ifno, uint16_t vid, uint16_t pid)
{
	static uint8_t data[512];
	usb_error_t err;
	int i;

	(void)aicms_get_max_lun(udev, ifno); /* a STALL here is normal */
	(void)usb_msc_cmd_in(udev, 0, cdb_inquiry, sizeof(cdb_inquiry), data, 36);
	for (i = 0; i < 8; ++i) {
		if (usb_msc_eject_cdb(udev, 0, cdb_test_unit_ready,
		    sizeof(cdb_test_unit_ready)) == 0)
			break;
		pause("aicms", hz / 20 > 0 ? hz / 20 : 1);
	}
	if (usb_msc_cmd_in(udev, 0, cdb_read_capacity, sizeof(cdb_read_capacity),
	    data, 8) == 0) {
		uint32_t bs = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
		    ((uint32_t)data[6] << 8) | data[7];
		uint8_t read10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0 };
		if (bs != 0 && bs <= sizeof(data))
			(void)usb_msc_cmd_in(udev, 0, read10, sizeof(read10), data, bs);
	}
	err = usb_msc_cmd_in(udev, 0, cdb_pandora_f3, sizeof(cdb_pandora_f3), data, 5);
	if (err == 0) {
		err = usb_msc_eject_cdb(udev, 0, cdb_pandora_f2, sizeof(cdb_pandora_f2));
		if (err == 0) {
			printf("aic8800: %04x:%04x vendor switch accepted (id %02x%02x%02x%02x%02x)\n",
			    vid, pid, data[0], data[1], data[2], data[3], data[4]);
			pause("aicms", hz / 2 > 0 ? hz / 2 : 1); /* settle, then re-enumerate */
			usbd_start_re_enumerate(udev);
			return (0);
		}
	}
	if (vid == PANDORA_VID)
		return (err); /* 1111:1111 is shared: never eject a stranger's storage */
	(void)usb_msc_eject_cdb(udev, 0, cdb_allow_removal, sizeof(cdb_allow_removal));
	err = usb_msc_eject_cdb(udev, 0, cdb_stop_eject, sizeof(cdb_stop_eject));
	if (err != 0)
		err = usb_msc_eject_cdb(udev, 0, cdb_stop_eject_immed,
		    sizeof(cdb_stop_eject_immed));
	return (err);
}

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

	err = aicms_switch(udev, iface->idesc->bInterfaceNumber, vid, pid);
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
