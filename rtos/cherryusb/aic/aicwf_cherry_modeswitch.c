/*
 * AICSemi AIC8800 storage-mode switch on CherryUSB (USB_STACK=cherry).
 *
 * The sticks start as a "driver CD" mass-storage device. Same commands as the libbsd version
 * (rtos/libbsd/aic_modeswitch.c) and the Linux firmware's usb_modeswitch.d entries, sent as
 * raw bulk-only transport CBWs on the interface's bulk pipes (CherryUSB's MSC class is not
 * used: its connect() asks GET_MAX_LUN first, which leaves the DC stick unusable):
 *   a69c:5721..572f  StandardEject: ALLOW MEDIUM REMOVAL, START STOP UNIT (eject)
 *   1111:1111        AIC8800D80 "Pandora" clone: INQUIRY, TEST UNIT READY, READ CAPACITY,
 *                    READ(10) of block 0, then the vendor CDBs FD..F3 (5 bytes in) and FD..F2,
 *                    then a port re-enumeration. GET_MAX_LUN is sent only as a retry when the
 *                    vendor command is not answered without it.
 * The 1111:1111 placeholder ID is never ejected (it may be a stranger's storage).
 * The device then drops off the bus and comes back as the Wi-Fi/BT device.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

#include "usbh_core.h"
#include "usb_msc.h"

#include "nexus_cherryusb.h"

#define AIC_VID     0xa69c
#define PANDORA_VID 0x1111
#define PANDORA_PID 0x1111

#define BOT_TIMEOUT_MS 2000

int nexus_cherryusb_aic_dc_switch = 1;

/* usbh_hubport_reenumerate(): aicwf_cherry.c */
int usbh_hubport_reenumerate(struct usbh_hubport *hport);

static const uint8_t cdb_allow_removal[6] = { 0x1e, 0, 0, 0, 0, 0 };
static const uint8_t cdb_stop_eject[6] = { 0x1b, 0, 0, 0, 0x02, 0 };
static const uint8_t cdb_stop_eject_immed[6] = { 0x1b, 0x01, 0, 0, 0x02, 0 };
static const uint8_t cdb_test_unit_ready[6] = { 0, 0, 0, 0, 0, 0 };
static const uint8_t cdb_inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
static const uint8_t cdb_read_capacity[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t cdb_pandora_f3[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xf3 };
static const uint8_t cdb_pandora_f2[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xf2 };

/* DMA buffers: non-cacheable, one switch at a time (hub thread) */
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_cbw[USB_ALIGN_UP(USB_SIZEOF_MSC_CBW, CONFIG_USB_ALIGN_SIZE)];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_csw[USB_ALIGN_UP(USB_SIZEOF_MSC_CSW, CONFIG_USB_ALIGN_SIZE)];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_data[512];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX struct usb_setup_packet g_setup;

struct bot {
    struct usbh_hubport *hport;
    struct usb_endpoint_descriptor *in, *out;
    uint8_t ifno;
    uint8_t tog_in, tog_out;
    uint32_t tag;
};

static int bot_xfer(struct bot *b, struct usb_endpoint_descriptor *ep, uint8_t *buf, uint32_t len,
                    uint32_t *actual)
{
    struct usbh_urb urb;
    uint8_t *tog = (ep->bEndpointAddress & 0x80) ? &b->tog_in : &b->tog_out;
    int rc;

    memset(&urb, 0, sizeof(urb));
    usbh_bulk_urb_fill(&urb, b->hport, ep, buf, len, BOT_TIMEOUT_MS, NULL, NULL);
    urb.data_toggle = *tog;
    rc = usbh_submit_urb(&urb);
    if (rc == 0 || rc == -USB_ERR_STALL) {
        *tog = rc == 0 ? urb.data_toggle : 0;
    }
    if (actual) {
        *actual = urb.actual_length;
    }
    return rc;
}

static int bot_clear_halt(struct bot *b, struct usb_endpoint_descriptor *ep)
{
    g_setup.bmRequestType = USB_REQUEST_DIR_OUT | USB_REQUEST_STANDARD | USB_REQUEST_RECIPIENT_ENDPOINT;
    g_setup.bRequest = USB_REQUEST_CLEAR_FEATURE;
    g_setup.wValue = USB_FEATURE_ENDPOINT_HALT;
    g_setup.wIndex = ep->bEndpointAddress;
    g_setup.wLength = 0;
    if (ep->bEndpointAddress & 0x80) {
        b->tog_in = 0;
    } else {
        b->tog_out = 0;
    }
    return usbh_control_transfer(b->hport, &g_setup, NULL);
}

/* 0: CSW status good; > 0: CSW status; 1000: device gone (what an eject should do); < 0 error */
static int bot_command(struct bot *b, const uint8_t *cdb, uint8_t cdb_len, uint8_t *data_in,
                       uint32_t data_len)
{
    struct CBW *cbw = (struct CBW *)g_cbw;
    struct CSW *csw = (struct CSW *)g_csw;
    uint32_t actual = 0;
    int rc;

    memset(g_cbw, 0, sizeof(g_cbw));
    cbw->dSignature = MSC_CBW_Signature;
    cbw->dTag = ++b->tag;
    cbw->dDataLength = data_in ? data_len : 0;
    cbw->bmFlags = data_in ? 0x80 : 0x00;
    cbw->bLUN = 0;
    cbw->bCBLength = cdb_len;
    memcpy(cbw->CB, cdb, cdb_len);
    rc = bot_xfer(b, b->out, g_cbw, USB_SIZEOF_MSC_CBW, &actual);
    if (rc < 0) {
        return b->hport->connected ? rc : 1000;
    }
    if (data_in && data_len) {
        rc = bot_xfer(b, b->in, g_data, data_len, &actual);
        if (rc == -USB_ERR_STALL) {
            (void)bot_clear_halt(b, b->in);
        } else if (rc < 0) {
            return b->hport->connected ? rc : 1000;
        } else if (data_in != g_data) {
            memcpy(data_in, g_data, actual);
        }
    }
    memset(g_csw, 0, sizeof(g_csw));
    rc = bot_xfer(b, b->in, g_csw, USB_SIZEOF_MSC_CSW, &actual);
    if (rc == -USB_ERR_STALL) {
        (void)bot_clear_halt(b, b->in);
        rc = bot_xfer(b, b->in, g_csw, USB_SIZEOF_MSC_CSW, &actual);
    }
    if (rc < 0) {
        /* an eject that worked takes the device away before the CSW */
        return b->hport->connected ? rc : 1000;
    }
    if (actual != USB_SIZEOF_MSC_CSW || csw->dSignature != MSC_CSW_Signature || csw->dTag != b->tag) {
        return -USB_ERR_IO;
    }
    return csw->bStatus;
}

static int bot_get_max_lun(struct bot *b)
{
    g_setup.bmRequestType = USB_REQUEST_DIR_IN | USB_REQUEST_CLASS | USB_REQUEST_RECIPIENT_INTERFACE;
    g_setup.bRequest = MSC_REQUEST_GET_MAX_LUN;
    g_setup.wValue = 0;
    g_setup.wIndex = b->ifno;
    g_setup.wLength = 1;
    return usbh_control_transfer(b->hport, &g_setup, g_data);
}

static int aicms_eject(struct bot *b)
{
    int rc;

    (void)bot_command(b, cdb_allow_removal, sizeof(cdb_allow_removal), NULL, 0);
    rc = bot_command(b, cdb_stop_eject, sizeof(cdb_stop_eject), NULL, 0);
    if (rc != 0 && rc != 1000) {
        rc = bot_command(b, cdb_stop_eject_immed, sizeof(cdb_stop_eject_immed), NULL, 0);
    }
    return rc;
}

static int aicms_vendor_once(struct bot *b, uint8_t id[5])
{
    int rc, i;

    (void)bot_command(b, cdb_inquiry, sizeof(cdb_inquiry), g_data, 36);
    for (i = 0; i < 8; ++i) {
        if (bot_command(b, cdb_test_unit_ready, sizeof(cdb_test_unit_ready), NULL, 0) == 0) {
            break;
        }
        usb_osal_msleep(50);
    }
    if (bot_command(b, cdb_read_capacity, sizeof(cdb_read_capacity), g_data, 8) == 0) {
        uint32_t bs = ((uint32_t)g_data[4] << 24) | ((uint32_t)g_data[5] << 16) |
                      ((uint32_t)g_data[6] << 8) | g_data[7];
        uint8_t read10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0 };

        if (bs != 0 && bs <= sizeof(g_data)) {
            (void)bot_command(b, read10, sizeof(read10), g_data, bs);
        }
    }
    rc = bot_command(b, cdb_pandora_f3, sizeof(cdb_pandora_f3), g_data, 5);
    if (rc == 0) {
        memcpy(id, g_data, 5);
        rc = bot_command(b, cdb_pandora_f2, sizeof(cdb_pandora_f2), NULL, 0);
    }
    return rc;
}

static int aicms_vendor(struct bot *b, uint16_t vid, uint16_t pid)
{
    uint8_t id[5] = { 0 };
    int rc = aicms_vendor_once(b, id);

    if (rc != 0 && rc != 1000 && b->hport->connected) {
        /* some fake-storage firmware ignores bulk-only commands before the MSC class init */
        (void)bot_get_max_lun(b);
        rc = aicms_vendor_once(b, id);
    }
    if (rc == 0) {
        printf("aic8800: %04x:%04x vendor switch accepted (id %02x%02x%02x%02x%02x)\n",
               vid, pid, id[0], id[1], id[2], id[3], id[4]);
        /* the device needs a port re-enumeration; the worker waits until connect() returned */
        (void)usbh_hubport_reenumerate(b->hport);
    }
    return rc;
}

int aicwf_cherry_is_storage_mode(uint16_t vid, uint16_t pid)
{
    if (vid == PANDORA_VID && pid == PANDORA_PID) {
        return 1;
    }
    return vid == AIC_VID && pid >= 0x5721 && pid <= 0x572f;
}

/* class connect() for the mass-storage interface of a storage-mode AIC stick (hub thread) */
int aicwf_cherry_modeswitch_connect(struct usbh_hubport *hport, uint8_t intf)
{
    struct usbh_interface_altsetting *alt = &hport->config.intf[intf].altsetting[0];
    uint16_t vid = hport->device_desc.idVendor, pid = hport->device_desc.idProduct;
    struct bot b;
    int rc, i;

    memset(&b, 0, sizeof(b));
    b.hport = hport;
    b.ifno = alt->intf_desc.bInterfaceNumber;
    for (i = 0; i < alt->intf_desc.bNumEndpoints && i < CONFIG_USBHOST_MAX_ENDPOINTS; i++) {
        struct usb_endpoint_descriptor *ep = &alt->ep[i].ep_desc;

        if (USB_GET_ENDPOINT_TYPE(ep->bmAttributes) != USB_ENDPOINT_TYPE_BULK) {
            continue;
        }
        if (ep->bEndpointAddress & 0x80) {
            b.in = ep;
        } else {
            b.out = ep;
        }
    }
    if (b.in == NULL || b.out == NULL) {
        printf("aic8800: %04x:%04x storage interface without bulk pipes\n", vid, pid);
        return -USB_ERR_NODEV;
    }
    if (vid == AIC_VID && pid == 0x5721 && !nexus_cherryusb_aic_dc_switch) {
        printf("aic8800: %04x:%04x driver CD left alone (usb_aic_dc_switch=0)\n", vid, pid);
        return 0;
    }
    if (vid != PANDORA_VID) {
        rc = aicms_eject(&b);
        if (rc == 0 || rc == 1000) {
            printf("aic8800: %04x:%04x StandardEject accepted%s\n", vid, pid,
                   rc == 1000 ? " (device left the bus)" : "");
            return 0;
        }
    }
    rc = aicms_vendor(&b, vid, pid);
    printf("aic8800: %04x:%04x storage mode, switch command %s (%d)\n", vid, pid,
           (rc == 0 || rc == 1000) ? "sent" : "failed", rc);
    return 0;
}
