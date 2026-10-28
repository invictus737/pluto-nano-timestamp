/*
 * aicwf: libbsd (FreeBSD USB host) attachment for AICSemi AIC8800 Wi-Fi/Bluetooth USB
 * devices, used by the RT-Smart AIC8800 driver that runs on RTEMS threads above it.
 *
 * This file is the only kernel-space part of the port:
 *  - it binds the vendor Wi-Fi interface (ff/ff/ff) and the Bluetooth HCI interface
 *    (e0/01/01) of a69c:* and 368b:* devices (the storage-mode switch is aic_modeswitch.c),
 *  - gives every bulk/interrupt endpoint a pool of FreeBSD USB transfers that implement
 *    CherryUSB-style asynchronous requests (see aicwf_usbif.h),
 *  - and provides the Ethernet-style "wlan0" ifnet the station is exposed through.
 *
 * Attach and detach never wait for the driver: they publish the interface to the client
 * class (which only queues work) and return. Detach cancels every transfer, fails
 * later calls with AICWF_USBIF_ENODEV and keeps the interface object alive until the
 * client releases it, so a late client call can never touch freed USB state.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <machine/rtems-bsd-kernel-space.h>

#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/queue.h>
#include <sys/priority.h>
#include <sys/taskqueue.h>
#include <machine/atomic.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_types.h>
#include <net/ethernet.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>
#include <dev/usb/usbdi_util.h>
#include <dev/usb/usb_core.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_device.h>
#include <dev/usb/usb_request.h>
#include <dev/usb/usb_hub.h>
#include <dev/usb/usb_util.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_transfer.h>
#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>

#include "aicwf_usbif.h"

#define AICWF_VID               0xa69c
#define AICWF_VID_V2            0x368b

#define AICWF_XFER_BULK_IN      24      /* RT-Smart arms 20 data/message RX URBs (+ loader) */
#define AICWF_XFER_BULK_OUT     8
#define AICWF_XFER_INTR         2
#define AICWF_MAX_XFER          128     /* request token: 8-bit index */
#define AICWF_BUF_BULK_IN       4096    /* RT-Smart RX URBs are 2048 bytes */
#define AICWF_BUF_BULK_OUT      16384   /* TX aggregate maximum */
#define AICWF_BUF_INTR          1024
#define AICWF_CTRL_TIMEOUT_MAX  5000

MALLOC_DEFINE(M_AICWF, "aicwf", "AIC8800 USB attachment");

struct aicwf_usbif;

struct aicwf_req {
	struct aicwf_usbif *usbif;
	struct usb_xfer *xfer;
	uint8_t ep;
	uint8_t busy;
	uint8_t started;
	uint8_t zlp;
	uint16_t gen;
	void *buf;
	uint32_t len;
	uint32_t timeout;
	aicwf_usbif_done_t done;
	void *arg;
};

struct aicwf_usbif {
	struct mtx mtx;
	volatile u_int refs;
	int gone;
	int announced;
	int ctrl_busy;
	struct usb_device *udev;
	struct aicwf_usbif_info info;
	int nreq;
	struct usb_xfer *xfer[AICWF_MAX_XFER];
	struct aicwf_req req[AICWF_MAX_XFER];
	TAILQ_ENTRY(aicwf_usbif) link;
};

struct aicwf_softc {
	device_t dev;
	struct aicwf_usbif *usbif;
};

static struct mtx aicwf_reg_mtx;
MTX_SYSINIT(aicwf_reg, &aicwf_reg_mtx, "aicwf registry", MTX_DEF);
static TAILQ_HEAD(, aicwf_usbif) aicwf_live = TAILQ_HEAD_INITIALIZER(aicwf_live);
static const struct aicwf_usbif_class *aicwf_class;

static device_probe_t aicwf_match;
static device_attach_t aicwf_attach;
static device_detach_t aicwf_detach;
static usb_callback_t aicwf_xfer_cb;

static int
aicwf_status(usb_error_t error, int gone)
{
	switch (error) {
	case USB_ERR_NORMAL_COMPLETION:
		return (0);
	case USB_ERR_CANCELLED:
		return (gone ? AICWF_USBIF_ENODEV : AICWF_USBIF_ESHUTDOWN);
	case USB_ERR_TIMEOUT:
		return (AICWF_USBIF_ETIMEOUT);
	case USB_ERR_STALLED:
		return (AICWF_USBIF_ESTALL);
	case USB_ERR_NOMEM:
		return (AICWF_USBIF_ENOMEM);
	case USB_ERR_INVAL:
		return (AICWF_USBIF_EINVAL);
	case USB_ERR_NOT_CONFIGURED:
	case USB_ERR_NO_PIPE:
		return (AICWF_USBIF_ENODEV);
	default:
		return (gone ? AICWF_USBIF_ENODEV : AICWF_USBIF_EIO);
	}
}

static void
aicwf_usbif_ref(struct aicwf_usbif *u)
{
	atomic_add_int(&u->refs, 1);
}

void
aicwf_usbif_release(struct aicwf_usbif *u)
{
	if (u == NULL)
		return;
	if (atomic_fetchadd_int(&u->refs, -1) != 1)
		return;
	mtx_destroy(&u->mtx);
	free(u, M_AICWF);
}

/* called with u->mtx held; idempotent through r->busy */
static void
aicwf_req_finish(struct aicwf_usbif *u, struct aicwf_req *r, int status, uint32_t actual)
{
	aicwf_usbif_done_t done = r->done;
	void *arg = r->arg;

	(void)u;
	if (!r->busy)
		return;
	r->busy = 0;
	r->started = 0;
	r->done = NULL;
	r->arg = NULL;
	r->buf = NULL;
	if (done != NULL)
		done(arg, status, actual);
}

static void
aicwf_xfer_cb(struct usb_xfer *xfer, usb_error_t error)
{
	struct aicwf_usbif *u = usbd_xfer_softc(xfer);
	struct aicwf_req *r = usbd_xfer_get_priv(xfer);
	struct usb_page_cache *pc;
	usb_frlength_t max;
	int actlen;

	switch (USB_GET_STATE(xfer)) {
	case USB_ST_TRANSFERRED:
		usbd_xfer_status(xfer, &actlen, NULL, NULL, NULL);
		if (actlen < 0)
			actlen = 0;
		if (r->busy && (r->ep & UE_DIR_IN) && r->buf != NULL) {
			if ((uint32_t)actlen > r->len)
				actlen = (int)r->len;
			pc = usbd_xfer_get_frame(xfer, 0);
			usbd_copy_out(pc, 0, r->buf, actlen);
		}
		aicwf_req_finish(u, r, 0, (uint32_t)actlen);
		break;
	case USB_ST_SETUP:
		if (!r->busy || r->started || u->gone)
			break;
		r->started = 1;
		max = usbd_xfer_max_len(xfer);
		if (r->len > max) {
			aicwf_req_finish(u, r, AICWF_USBIF_EINVAL, 0);
			break;
		}
		pc = usbd_xfer_get_frame(xfer, 0);
		if (!(r->ep & UE_DIR_IN) && r->len > 0)
			usbd_copy_in(pc, 0, r->buf, r->len);
		usbd_xfer_set_timeout(xfer, (int)r->timeout);
		if (r->zlp)
			usbd_xfer_set_flag(xfer, USB_FORCE_SHORT_XFER);
		else
			usbd_xfer_clr_flag(xfer, USB_FORCE_SHORT_XFER);
		usbd_xfer_set_frame_len(xfer, 0, r->len);
		usbd_transfer_submit(xfer);
		break;
	default:
		if (error == USB_ERR_STALLED)
			usbd_xfer_set_stall(xfer);
		aicwf_req_finish(u, r, aicwf_status(error, u->gone), 0);
		break;
	}
}

int
aicwf_usbif_submit(struct aicwf_usbif *u, uint8_t ep, void *buf, uint32_t len,
    uint32_t timeout_ms, int zlp, aicwf_usbif_done_t done, void *arg, uint32_t *token)
{
	struct aicwf_req *r = NULL;
	int i, matched = 0;

	if (u == NULL || (len > 0 && buf == NULL))
		return (AICWF_USBIF_EINVAL);
	mtx_lock(&u->mtx);
	if (u->gone) {
		mtx_unlock(&u->mtx);
		return (AICWF_USBIF_ENODEV);
	}
	for (i = 0; i < u->nreq; i++) {
		if (u->req[i].ep != ep)
			continue;
		matched = 1;
		if (!u->req[i].busy) {
			r = &u->req[i];
			break;
		}
	}
	if (r == NULL) {
		mtx_unlock(&u->mtx);
		return (matched ? AICWF_USBIF_EBUSY : AICWF_USBIF_EINVAL);
	}
	if (len > usbd_xfer_max_len(r->xfer)) {
		mtx_unlock(&u->mtx);
		printf("aicwf: ep 0x%02x request of %u bytes exceeds %u\n", ep, len,
		    (unsigned)usbd_xfer_max_len(r->xfer));
		return (AICWF_USBIF_EINVAL);
	}
	r->busy = 1;
	r->started = 0;
	r->gen++;
	r->buf = buf;
	r->len = len;
	r->timeout = timeout_ms;
	r->zlp = zlp ? 1 : 0;
	r->done = done;
	r->arg = arg;
	if (token != NULL)
		*token = ((uint32_t)r->gen << 8) | (uint32_t)(r - u->req);
	usbd_transfer_start(r->xfer);
	mtx_unlock(&u->mtx);
	return (0);
}

/* u->mtx held */
static void
aicwf_req_cancel_locked(struct aicwf_usbif *u, struct aicwf_req *r, int status)
{
	if (!r->busy)
		return;
	usbd_transfer_stop(r->xfer);
	/* a transfer that was handed to the controller completes through the callback */
	if (r->busy && (!r->started || !usbd_transfer_pending(r->xfer)))
		aicwf_req_finish(u, r, status, 0);
}

int
aicwf_usbif_cancel(struct aicwf_usbif *u, uint32_t token)
{
	struct aicwf_req *r;
	uint32_t index = token & 0xff;

	if (u == NULL)
		return (AICWF_USBIF_EINVAL);
	mtx_lock(&u->mtx);
	if (index >= (uint32_t)u->nreq) {
		mtx_unlock(&u->mtx);
		return (AICWF_USBIF_EINVAL);
	}
	r = &u->req[index];
	if (r->busy && r->gen == (uint16_t)(token >> 8))
		aicwf_req_cancel_locked(u, r, u->gone ? AICWF_USBIF_ENODEV :
		    AICWF_USBIF_ESHUTDOWN);
	mtx_unlock(&u->mtx);
	return (0);
}

static int
aicwf_ctrl_enter(struct aicwf_usbif *u)
{
	mtx_lock(&u->mtx);
	if (u->gone) {
		mtx_unlock(&u->mtx);
		return (AICWF_USBIF_ENODEV);
	}
	u->ctrl_busy++;
	mtx_unlock(&u->mtx);
	return (0);
}

static void
aicwf_ctrl_leave(struct aicwf_usbif *u)
{
	mtx_lock(&u->mtx);
	if (--u->ctrl_busy == 0 && u->gone)
		wakeup(&u->ctrl_busy);
	mtx_unlock(&u->mtx);
}

int
aicwf_usbif_control(struct aicwf_usbif *u, const uint8_t setup[8], void *data,
    uint32_t timeout_ms)
{
	struct usb_device_request req;
	uint16_t actlen = 0;
	usb_error_t err;
	int rc;

	if (u == NULL || setup == NULL)
		return (AICWF_USBIF_EINVAL);
	if ((rc = aicwf_ctrl_enter(u)) != 0)
		return (rc);
	memcpy(&req, setup, sizeof(req));
	if (timeout_ms == 0 || timeout_ms > AICWF_CTRL_TIMEOUT_MAX)
		timeout_ms = AICWF_CTRL_TIMEOUT_MAX;
	err = usbd_do_request_flags(u->udev, NULL, &req, data, USB_SHORT_XFER_OK, &actlen,
	    timeout_ms);
	aicwf_ctrl_leave(u);
	if (err != USB_ERR_NORMAL_COMPLETION)
		return (aicwf_status(err, u->gone));
	return ((int)actlen);
}

int
aicwf_usbif_clear_stall(struct aicwf_usbif *u, uint8_t ep)
{
	int i;

	if (u == NULL)
		return (AICWF_USBIF_EINVAL);
	mtx_lock(&u->mtx);
	if (u->gone) {
		mtx_unlock(&u->mtx);
		return (AICWF_USBIF_ENODEV);
	}
	/* the next transfer on this pipe first clears the halt and the data toggle */
	for (i = 0; i < u->nreq; i++) {
		if (u->req[i].ep == ep && !u->req[i].busy) {
			usbd_xfer_set_stall(u->req[i].xfer);
			break;
		}
	}
	mtx_unlock(&u->mtx);
	return (0);
}

int
aicwf_usbif_reenumerate(struct aicwf_usbif *u)
{
	int rc;

	if (u == NULL)
		return (AICWF_USBIF_EINVAL);
	if ((rc = aicwf_ctrl_enter(u)) != 0)
		return (rc);
	usbd_start_re_enumerate(u->udev);
	aicwf_ctrl_leave(u);
	return (0);
}

int
aicwf_usbif_string(struct aicwf_usbif *u, uint8_t index, char *buf, size_t len)
{
	usb_error_t err;
	int rc;

	if (u == NULL || buf == NULL || len == 0)
		return (AICWF_USBIF_EINVAL);
	buf[0] = '\0';
	if (index == 0)
		return (AICWF_USBIF_EINVAL);
	if ((rc = aicwf_ctrl_enter(u)) != 0)
		return (rc);
	err = usbd_req_get_string_any(u->udev, NULL, buf, (uint16_t)MIN(len, 0xffff), index);
	aicwf_ctrl_leave(u);
	return (err == USB_ERR_NORMAL_COMPLETION ? 0 : aicwf_status(err, 0));
}

int
aicwf_usbif_connected(struct aicwf_usbif *u)
{
	return (u != NULL && !u->gone);
}

int
aicwf_usbif_register(const struct aicwf_usbif_class *cls)
{
	struct aicwf_usbif *u;

	if (cls == NULL || cls->attach == NULL || cls->detach == NULL)
		return (AICWF_USBIF_EINVAL);
	mtx_lock(&aicwf_reg_mtx);
	if (aicwf_class != NULL) {
		mtx_unlock(&aicwf_reg_mtx);
		return (AICWF_USBIF_EBUSY);
	}
	aicwf_class = cls;
	/* callbacks only queue work, so they run under the registry lock (keeps order) */
	TAILQ_FOREACH(u, &aicwf_live, link) {
		if (!u->announced && !u->gone) {
			u->announced = 1;
			aicwf_usbif_ref(u);
			cls->attach(u, &u->info);
		}
	}
	mtx_unlock(&aicwf_reg_mtx);
	return (0);
}

/*
 * Re-scan every USB bus; with power_cycle, first switch the power of every root-hub port
 * that has no device attached off and on again (what a replug does). Ports with a device
 * (a hub, an Ethernet dongle) are never touched. Returns the number of ports cycled.
 */
int
aicwf_usb_rescan(int power_cycle)
{
	devclass_t dc = devclass_find("usbus");
	int unit, max, cycled = 0;

	if (dc == NULL)
		return (AICWF_USBIF_ENODEV);
	max = devclass_get_maxunit(dc);
	for (unit = 0; unit < max; unit++) {
		device_t dev = devclass_get_device(dc, unit);
		struct usb_device *root;
		struct usb_bus *bus;
		struct usb_hub *hub;
		uint8_t port, do_unlock;

		if (dev == NULL || !device_is_attached(dev))
			continue;
		bus = device_get_ivars(dev);
		if (bus == NULL || bus->devices == NULL)
			continue;
		root = bus->devices[USB_ROOT_HUB_ADDR];
		if (root == NULL || root->hub == NULL)
			continue;
		if (power_cycle) {
			hub = root->hub;
			do_unlock = usbd_enum_lock(root);
			for (port = 0; port < hub->nports; port++) {
				if (usb_bus_port_get_device(bus, hub->ports + port) != NULL)
					continue;
				(void)usbd_req_clear_port_feature(root, NULL, port + 1,
				    UHF_PORT_POWER);
				usb_pause_mtx(NULL, hz / 2);
				(void)usbd_req_set_port_feature(root, NULL, port + 1,
				    UHF_PORT_POWER);
				printf("aicwf: %s root port %u power cycled (no device)\n",
				    device_get_nameunit(dev), port + 1);
				cycled++;
			}
			if (do_unlock)
				usbd_enum_unlock(root);
		}
		usb_needs_explore(bus, 1);
	}
	return (cycled);
}

/* ---- newbus ---- */

static int
aicwf_iface_wanted(const struct usb_attach_arg *uaa)
{
	if (uaa->info.idVendor != AICWF_VID && uaa->info.idVendor != AICWF_VID_V2)
		return (0);
	if (uaa->info.bInterfaceClass == UICLASS_VENDOR &&
	    uaa->info.bInterfaceSubClass == 0xff && uaa->info.bInterfaceProtocol == 0xff)
		return (1);
	/* Bluetooth HCI (btusb class), RT-Smart aic8800_btusb.c */
	return (uaa->info.bInterfaceClass == UICLASS_WIRELESS &&
	    uaa->info.bInterfaceSubClass == 0x01 && uaa->info.bInterfaceProtocol == 0x01);
}

static int
aicwf_match(device_t dev)
{
	struct usb_attach_arg *uaa = device_get_ivars(dev);

	if (uaa->usb_mode != USB_MODE_HOST || uaa->info.bConfigIndex != 0)
		return (ENXIO);
	if (!aicwf_iface_wanted(uaa))
		return (ENXIO);
	return (BUS_PROBE_SPECIFIC);
}

static uint8_t
aicwf_speed(struct usb_device *udev)
{
	switch (usbd_get_speed(udev)) {
	case USB_SPEED_LOW:
		return (AICWF_USBIF_SPEED_LOW);
	case USB_SPEED_FULL:
		return (AICWF_USBIF_SPEED_FULL);
	case USB_SPEED_SUPER:
		return (AICWF_USBIF_SPEED_SUPER);
	default:
		return (AICWF_USBIF_SPEED_HIGH);
	}
}

static int
aicwf_attach(device_t dev)
{
	struct usb_attach_arg *uaa = device_get_ivars(dev);
	struct aicwf_softc *sc = device_get_softc(dev);
	struct usb_device_descriptor *dd = usbd_get_device_descriptor(uaa->device);
	struct usb_config_descriptor *cd = usbd_get_config_descriptor(uaa->device);
	struct usb_interface *iface = usbd_get_iface(uaa->device, uaa->info.bIfaceIndex);
	struct usb_config *cfg;
	struct usb_descriptor *d = NULL;
	struct aicwf_usbif *u;
	usb_error_t uerr;
	uint8_t iface_index = uaa->info.bIfaceIndex;
	int in_iface = 0, i, k, n = 0;

	device_set_usb_desc(dev);
	sc->dev = dev;
	if (iface == NULL || iface->idesc == NULL || cd == NULL) {
		device_printf(dev, "no interface descriptor\n");
		return (ENXIO);
	}
	u = malloc(sizeof(*u), M_AICWF, M_WAITOK | M_ZERO);
	mtx_init(&u->mtx, "aicwf usbif", NULL, MTX_DEF);	/* outlives the device */
	u->refs = 1;
	u->udev = uaa->device;
	u->info.vid = uaa->info.idVendor;
	u->info.pid = uaa->info.idProduct;
	u->info.bcd_device = uaa->info.bcdDevice;
	u->info.speed = aicwf_speed(uaa->device);
	u->info.device_key = (uintptr_t)uaa->device;
	memcpy(u->info.device_desc, dd, MIN(sizeof(u->info.device_desc), sizeof(*dd)));
	memcpy(u->info.config_desc, cd, MIN(sizeof(u->info.config_desc), sizeof(*cd)));
	memcpy(u->info.iface_desc, iface->idesc,
	    MIN(sizeof(u->info.iface_desc), sizeof(*iface->idesc)));
	u->info.iface_number = iface->idesc->bInterfaceNumber;
	u->info.iface_index = iface_index;

	/* endpoints of alternate setting 0 of this interface, in descriptor order */
	while ((d = usb_desc_foreach(cd, d)) != NULL) {
		if (d->bDescriptorType == UDESC_INTERFACE &&
		    d->bLength >= sizeof(struct usb_interface_descriptor)) {
			struct usb_interface_descriptor *id = (void *)d;

			in_iface = id->bInterfaceNumber == u->info.iface_number &&
			    id->bAlternateSetting == 0;
		} else if (in_iface && d->bDescriptorType == UDESC_ENDPOINT &&
		    d->bLength >= sizeof(struct usb_endpoint_descriptor) &&
		    u->info.num_ep < AICWF_USBIF_MAX_EP) {
			struct usb_endpoint_descriptor *ed = (void *)d;
			struct aicwf_usbif_ep *e = &u->info.ep[u->info.num_ep++];

			e->addr = ed->bEndpointAddress;
			e->attributes = ed->bmAttributes;
			e->interval = ed->bInterval;
			e->max_packet = UGETW(ed->wMaxPacketSize);
		}
	}
	device_printf(dev, "%04x:%04x rev %x.%02x iface %u class %02x/%02x/%02x, %u endpoints, "
	    "%s speed\n", u->info.vid, u->info.pid, u->info.bcd_device >> 8,
	    u->info.bcd_device & 0xff, u->info.iface_number, iface->idesc->bInterfaceClass,
	    iface->idesc->bInterfaceSubClass, iface->idesc->bInterfaceProtocol, u->info.num_ep,
	    u->info.speed == AICWF_USBIF_SPEED_HIGH ? "high" :
	    u->info.speed == AICWF_USBIF_SPEED_SUPER ? "super" :
	    u->info.speed == AICWF_USBIF_SPEED_FULL ? "full" : "low");

	cfg = malloc(sizeof(*cfg) * AICWF_MAX_XFER, M_AICWF, M_WAITOK | M_ZERO);
	for (i = 0; i < u->info.num_ep; i++) {
		struct aicwf_usbif_ep *e = &u->info.ep[i];
		uint8_t type = e->attributes & UE_XFERTYPE;
		int is_in = (e->addr & UE_DIR_IN) != 0, count;
		usb_frlength_t size;

		device_printf(dev, "  ep 0x%02x %s %s max packet %u interval %u\n", e->addr,
		    type == UE_BULK ? "bulk" : type == UE_INTERRUPT ? "intr" :
		    type == UE_ISOCHRONOUS ? "isoc" : "ctrl", is_in ? "IN" : "OUT",
		    e->max_packet & 0x7ff, e->interval);
		if (type == UE_BULK) {
			count = is_in ? AICWF_XFER_BULK_IN : AICWF_XFER_BULK_OUT;
			size = is_in ? AICWF_BUF_BULK_IN : AICWF_BUF_BULK_OUT;
		} else if (type == UE_INTERRUPT) {
			count = AICWF_XFER_INTR;
			size = AICWF_BUF_INTR;
		} else {
			continue;	/* isochronous (SCO) is not supported */
		}
		for (k = 0; k < count && n < AICWF_MAX_XFER; k++, n++) {
			cfg[n].type = type;
			cfg[n].endpoint = e->addr & UE_ADDR;
			cfg[n].direction = is_in ? UE_DIR_IN : UE_DIR_OUT;
			cfg[n].ep_index = 0;
			cfg[n].bufsize = size;
			cfg[n].flags.pipe_bof = 1;
			cfg[n].flags.short_xfer_ok = is_in ? 1 : 0;
			cfg[n].callback = aicwf_xfer_cb;
			u->req[n].ep = e->addr;
		}
	}
	if (n > 0) {
		uerr = usbd_transfer_setup(uaa->device, &iface_index, u->xfer, cfg, n, u, &u->mtx);
		if (uerr != USB_ERR_NORMAL_COMPLETION) {
			device_printf(dev, "transfer setup failed: %s\n", usbd_errstr(uerr));
			free(cfg, M_AICWF);
			mtx_destroy(&u->mtx);
			free(u, M_AICWF);
			return (ENXIO);
		}
	}
	free(cfg, M_AICWF);
	for (i = 0; i < n; i++) {
		u->req[i].usbif = u;
		u->req[i].xfer = u->xfer[i];
		usbd_xfer_set_priv(u->xfer[i], &u->req[i]);
	}
	u->nreq = n;
	sc->usbif = u;

	mtx_lock(&aicwf_reg_mtx);
	TAILQ_INSERT_TAIL(&aicwf_live, u, link);
	if (aicwf_class != NULL) {
		u->announced = 1;
		aicwf_usbif_ref(u);
		aicwf_class->attach(u, &u->info);
	} else {
		device_printf(dev, "driver not started yet, interface held\n");
	}
	mtx_unlock(&aicwf_reg_mtx);
	return (0);
}

static int
aicwf_detach(device_t dev)
{
	struct aicwf_softc *sc = device_get_softc(dev);
	struct aicwf_usbif *u = sc->usbif;
	int i;

	if (u == NULL)
		return (0);
	mtx_lock(&aicwf_reg_mtx);
	TAILQ_REMOVE(&aicwf_live, u, link);
	mtx_lock(&u->mtx);
	u->gone = 1;
	for (i = 0; i < u->nreq; i++)
		aicwf_req_cancel_locked(u, &u->req[i], AICWF_USBIF_ENODEV);
	mtx_unlock(&u->mtx);
	if (u->announced && aicwf_class != NULL)
		aicwf_class->detach(u);
	mtx_unlock(&aicwf_reg_mtx);

	/* control requests in progress use udev: wait for them (bounded by their timeout) */
	mtx_lock(&u->mtx);
	while (u->ctrl_busy > 0)
		mtx_sleep(&u->ctrl_busy, &u->mtx, 0, "aicwfd", hz);
	mtx_unlock(&u->mtx);

	usbd_transfer_unsetup(u->xfer, AICWF_MAX_XFER);
	mtx_lock(&u->mtx);
	for (i = 0; i < u->nreq; i++) {
		aicwf_req_finish(u, &u->req[i], AICWF_USBIF_ENODEV, 0);
		u->req[i].xfer = NULL;
	}
	u->nreq = 0;
	u->udev = NULL;
	mtx_unlock(&u->mtx);
	sc->usbif = NULL;
	device_printf(dev, "detached\n");
	aicwf_usbif_release(u);
	return (0);
}

static device_method_t aicwf_methods[] = {
	DEVMETHOD(device_probe, aicwf_match),
	DEVMETHOD(device_attach, aicwf_attach),
	DEVMETHOD(device_detach, aicwf_detach),
	DEVMETHOD_END
};

static driver_t aicwf_driver = {
	.name = "aicwf",
	.methods = aicwf_methods,
	.size = sizeof(struct aicwf_softc),
};

DRIVER_MODULE(aicwf, uhub, aicwf_driver, NULL, NULL);
MODULE_DEPEND(aicwf, usb, 1, 1, 1);
MODULE_DEPEND(aicwf, ether, 1, 1, 1);
MODULE_VERSION(aicwf, 1);

/* ---- Ethernet-style ifnet ---- */

#define AICWF_IF_TXQ_LEN        256
#define AICWF_IF_FRAME_MAX      2048

struct aicwf_ifnet {
	if_t ifp;
	struct mtx mtx;
	struct mbufq txq;
	struct task tx_task;
	struct taskqueue *tq;
	aicwf_ifnet_tx_t tx;
	void *arg;
	int dying;
	uint8_t frame[AICWF_IF_FRAME_MAX] __aligned(4);
};

static void
aicwf_if_tx_task(void *context, int pending)
{
	struct aicwf_ifnet *ifn = context;
	struct mbuf *m;
	int len;

	(void)pending;
	for (;;) {
		mtx_lock(&ifn->mtx);
		m = ifn->dying ? NULL : mbufq_dequeue(&ifn->txq);
		mtx_unlock(&ifn->mtx);
		if (m == NULL)
			break;
		len = m->m_pkthdr.len;
		if (len <= 0 || len > AICWF_IF_FRAME_MAX) {
			if_inc_counter(ifn->ifp, IFCOUNTER_OERRORS, 1);
			m_freem(m);
			continue;
		}
		m_copydata(m, 0, len, (caddr_t)ifn->frame);
		m_freem(m);
		if (ifn->tx(ifn->arg, ifn->frame, (uint32_t)len) == 0) {
			if_inc_counter(ifn->ifp, IFCOUNTER_OPACKETS, 1);
			if_inc_counter(ifn->ifp, IFCOUNTER_OBYTES, len);
		} else {
			if_inc_counter(ifn->ifp, IFCOUNTER_OERRORS, 1);
		}
	}
}

static int
aicwf_if_transmit(if_t ifp, struct mbuf *m)
{
	struct aicwf_ifnet *ifn = if_getsoftc(ifp);
	int error;

	mtx_lock(&ifn->mtx);
	if (ifn->dying || !(if_getdrvflags(ifp) & IFF_DRV_RUNNING))
		error = ENETDOWN;
	else
		error = mbufq_enqueue(&ifn->txq, m);
	mtx_unlock(&ifn->mtx);
	if (error != 0) {
		if_inc_counter(ifp, IFCOUNTER_OQDROPS, 1);
		m_freem(m);
		return (error);
	}
	taskqueue_enqueue(ifn->tq, &ifn->tx_task);
	return (0);
}

static void
aicwf_if_qflush(if_t ifp)
{
	struct aicwf_ifnet *ifn = if_getsoftc(ifp);

	mtx_lock(&ifn->mtx);
	mbufq_drain(&ifn->txq);
	mtx_unlock(&ifn->mtx);
}

static void
aicwf_if_init(void *arg)
{
	struct aicwf_ifnet *ifn = arg;

	if_setdrvflagbits(ifn->ifp, IFF_DRV_RUNNING, 0);
}

static int
aicwf_if_ioctl(if_t ifp, u_long cmd, caddr_t data)
{
	switch (cmd) {
	case SIOCSIFFLAGS:
		if (if_getflags(ifp) & IFF_UP)
			if_setdrvflagbits(ifp, IFF_DRV_RUNNING, 0);
		else
			if_setdrvflagbits(ifp, 0, IFF_DRV_RUNNING);
		return (0);
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		return (0);	/* the firmware passes multicast */
	default:
		return (ether_ioctl(ifp, cmd, data));
	}
}

struct aicwf_ifnet *
aicwf_ifnet_attach(const char *name, const uint8_t mac[6], aicwf_ifnet_tx_t tx, void *arg)
{
	struct aicwf_ifnet *ifn;
	char prefix[IFNAMSIZ];
	size_t plen;
	int unit = 0;
	if_t ifp;

	if (name == NULL || mac == NULL || tx == NULL)
		return (NULL);
	plen = strlen(name);
	while (plen > 0 && name[plen - 1] >= '0' && name[plen - 1] <= '9')
		plen--;
	if (plen == 0 || plen >= sizeof(prefix))
		return (NULL);
	memcpy(prefix, name, plen);
	prefix[plen] = '\0';
	if (name[plen] != '\0')
		unit = (int)strtoul(name + plen, NULL, 10);

	ifn = malloc(sizeof(*ifn), M_AICWF, M_WAITOK | M_ZERO);
	mtx_init(&ifn->mtx, "aicwf ifnet", NULL, MTX_DEF);
	mbufq_init(&ifn->txq, AICWF_IF_TXQ_LEN);
	TASK_INIT(&ifn->tx_task, 0, aicwf_if_tx_task, ifn);
	ifn->tq = taskqueue_create("aicwf_tx", M_WAITOK, taskqueue_thread_enqueue, &ifn->tq);
	if (ifn->tq == NULL || taskqueue_start_threads(&ifn->tq, 1, PI_NET, "%s tx", name) != 0) {
		if (ifn->tq != NULL)
			taskqueue_free(ifn->tq);
		mtx_destroy(&ifn->mtx);
		free(ifn, M_AICWF);
		return (NULL);
	}
	ifn->tx = tx;
	ifn->arg = arg;
	ifp = if_alloc(IFT_ETHER);
	ifn->ifp = ifp;
	if_setsoftc(ifp, ifn);
	if_initname(ifp, prefix, unit);
	if_setflags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST);
	if_setmtu(ifp, ETHERMTU);
	if_setinitfn(ifp, aicwf_if_init);
	if_setioctlfn(ifp, aicwf_if_ioctl);
	if_settransmitfn(ifp, aicwf_if_transmit);
	if_setqflushfn(ifp, aicwf_if_qflush);
	ether_ifattach(ifp, mac);
	if_link_state_change(ifp, LINK_STATE_DOWN);
	printf("aicwf: %s attached, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", if_name(ifp),
	    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return (ifn);
}

int
aicwf_ifnet_input(struct aicwf_ifnet *ifn, const void *frame, uint32_t len)
{
	struct mbuf *m;

	if (ifn == NULL || frame == NULL || len < ETHER_HDR_LEN || len > AICWF_IF_FRAME_MAX)
		return (EINVAL);
	if (ifn->dying)
		return (ENXIO);
	m = m_devget(__DECONST(char *, frame), (int)len, ETHER_ALIGN, ifn->ifp, NULL);
	if (m == NULL) {
		if_inc_counter(ifn->ifp, IFCOUNTER_IQDROPS, 1);
		return (ENOBUFS);
	}
	if_inc_counter(ifn->ifp, IFCOUNTER_IPACKETS, 1);
	if_input(ifn->ifp, m);
	return (0);
}

void
aicwf_ifnet_link(struct aicwf_ifnet *ifn, int up)
{
	if (ifn == NULL || ifn->dying)
		return;
	if_link_state_change(ifn->ifp, up ? LINK_STATE_UP : LINK_STATE_DOWN);
}

void
aicwf_ifnet_detach(struct aicwf_ifnet *ifn)
{
	if (ifn == NULL)
		return;
	mtx_lock(&ifn->mtx);
	ifn->dying = 1;
	mtx_unlock(&ifn->mtx);
	if_link_state_change(ifn->ifp, LINK_STATE_DOWN);
	ether_ifdetach(ifn->ifp);
	taskqueue_drain(ifn->tq, &ifn->tx_task);
	taskqueue_free(ifn->tq);
	mtx_lock(&ifn->mtx);
	mbufq_drain(&ifn->txq);
	mtx_unlock(&ifn->mtx);
	if_free(ifn->ifp);
	mtx_destroy(&ifn->mtx);
	free(ifn, M_AICWF);
}
