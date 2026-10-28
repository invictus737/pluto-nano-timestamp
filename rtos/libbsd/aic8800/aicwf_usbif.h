/*
 * aicwf: interface between the libbsd kernel part of the AIC8800 port (if_aicwf_usb.c:
 * USB interface attachment, bulk/interrupt pipes, the wlan0 ifnet) and the RT-Smart
 * driver that runs on top of it as ordinary RTEMS threads (shim/usbh_shim.c,
 * shim/wlan_dev_shim.c). Plain C types only: this header is compiled on both sides.
 *
 * Threading contract
 *  - attach/detach callbacks are called from the USB explore thread and must only queue
 *    work; they never wait for the driver.
 *  - transfer completion callbacks are called from the USB callback thread with the
 *    interface lock held: they must only record the result and wake a thread (never call
 *    back into aicwf_usbif_*).
 *  - after detach every call on the interface fails with AICWF_USBIF_ENODEV and no
 *    callback follows except completions of transfers cancelled by the detach. The
 *    interface object stays valid until the last aicwf_usbif_release().
 *  - the ifnet transmit callback runs on a kernel taskqueue thread and may block briefly.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef AICWF_USBIF_H
#define AICWF_USBIF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* transfer results: 0 or one of these (negated CherryUSB USB_ERR_* numbers) */
#define AICWF_USBIF_ENOMEM      (-1)
#define AICWF_USBIF_EINVAL      (-2)
#define AICWF_USBIF_ENODEV      (-3)
#define AICWF_USBIF_ENOTCONN    (-4)
#define AICWF_USBIF_ENOTSUPP    (-5)
#define AICWF_USBIF_EBUSY       (-6)
#define AICWF_USBIF_ESTALL      (-8)
#define AICWF_USBIF_EIO         (-12)
#define AICWF_USBIF_ESHUTDOWN   (-13)
#define AICWF_USBIF_ETIMEOUT    (-14)

/* CherryUSB speed numbering */
#define AICWF_USBIF_SPEED_LOW   1
#define AICWF_USBIF_SPEED_FULL  2
#define AICWF_USBIF_SPEED_HIGH  3
#define AICWF_USBIF_SPEED_SUPER 5

#define AICWF_USBIF_MAX_EP      16

struct aicwf_usbif_ep {
	uint8_t addr;           /* bEndpointAddress */
	uint8_t attributes;     /* bmAttributes */
	uint8_t interval;       /* bInterval */
	uint8_t reserved;
	uint16_t max_packet;    /* wMaxPacketSize */
};

struct aicwf_usbif_info {
	uint16_t vid;
	uint16_t pid;
	uint16_t bcd_device;
	uint8_t speed;
	uint8_t reserved;
	uintptr_t device_key;           /* same value for every interface of one device */
	uint8_t device_desc[18];        /* raw device descriptor */
	uint8_t config_desc[9];         /* raw configuration descriptor header */
	uint8_t iface_number;           /* bInterfaceNumber */
	uint8_t iface_index;            /* index in the configuration */
	uint8_t iface_desc[9];          /* raw interface descriptor (alternate setting 0) */
	uint8_t num_ep;
	struct aicwf_usbif_ep ep[AICWF_USBIF_MAX_EP];
};

struct aicwf_usbif;

typedef void (*aicwf_usbif_done_t)(void *arg, int status, uint32_t actual);

struct aicwf_usbif_class {
	/* a matching interface appeared (iface: its descriptors); the class owns one reference */
	void (*attach)(struct aicwf_usbif *usbif, const struct aicwf_usbif_info *info);
	/* the device is gone; pending transfers complete with AICWF_USBIF_ENODEV */
	void (*detach)(struct aicwf_usbif *usbif);
};

/* Register the (single) client class. Interfaces attached earlier are replayed. */
int aicwf_usbif_register(const struct aicwf_usbif_class *cls);

/*
 * Queue one transfer on endpoint ep (address incl. direction bit). IN data is copied to
 * buf on completion, OUT data is read from buf when the transfer starts: buf must stay
 * valid until the completion callback. timeout_ms 0 = none.
 * zlp: terminate an OUT transfer whose length is a multiple of the packet size with a
 * zero-length packet. *token identifies the transfer for aicwf_usbif_cancel().
 */
int aicwf_usbif_submit(struct aicwf_usbif *usbif, uint8_t ep, void *buf, uint32_t len,
    uint32_t timeout_ms, int zlp, aicwf_usbif_done_t done, void *arg, uint32_t *token);
/* Cancel one transfer; its completion reports AICWF_USBIF_ESHUTDOWN. */
int aicwf_usbif_cancel(struct aicwf_usbif *usbif, uint32_t token);
/* Synchronous control transfer; returns the number of data bytes or an error. */
int aicwf_usbif_control(struct aicwf_usbif *usbif, const uint8_t setup[8], void *data,
    uint32_t timeout_ms);
/* Clear a halted endpoint (host side and device side). */
int aicwf_usbif_clear_stall(struct aicwf_usbif *usbif, uint8_t ep);
/* Ask the hub driver to reset and re-enumerate the device. */
int aicwf_usbif_reenumerate(struct aicwf_usbif *usbif);
/* ASCII string descriptor. */
int aicwf_usbif_string(struct aicwf_usbif *usbif, uint8_t index, char *buf, size_t len);
int aicwf_usbif_connected(struct aicwf_usbif *usbif);
void aicwf_usbif_release(struct aicwf_usbif *usbif);
/*
 * Re-explore every USB bus; with power_cycle, first power-cycle the empty root-hub ports
 * (recovers a device the controller did not see at boot). Returns ports cycled or < 0.
 */
int aicwf_usb_rescan(int power_cycle);

/* ---- Ethernet-style network interface (the station) ---- */
struct aicwf_ifnet;
typedef int (*aicwf_ifnet_tx_t)(void *arg, const void *frame, uint32_t len);

struct aicwf_ifnet *aicwf_ifnet_attach(const char *name, const uint8_t mac[6],
    aicwf_ifnet_tx_t tx, void *arg);
/* Hand one received Ethernet frame (destination MAC first) to the stack. */
int aicwf_ifnet_input(struct aicwf_ifnet *ifn, const void *frame, uint32_t len);
void aicwf_ifnet_link(struct aicwf_ifnet *ifn, int up);
void aicwf_ifnet_detach(struct aicwf_ifnet *ifn);

#ifdef __cplusplus
}
#endif

#endif /* AICWF_USBIF_H */
