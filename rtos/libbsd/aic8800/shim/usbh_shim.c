/*
 * CherryUSB host API for the RT-Smart AIC8800 driver, on the libbsd USB host stack
 * (aicwf_usbif.h, implemented by if_aicwf_usb.c).
 *
 * Threads:
 *  aic-usbh  replays interface attach/detach to the RT-Smart class drivers in arrival
 *            order (connect() may run the whole firmware download: it blocks this thread,
 *            never the libbsd USB explore thread).
 *  aic-usbc  delivers URB completions (the "giveback" context of the DWC2 port).
 * The libbsd completion callback only records the result and queues the URB.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtthread.h>
#include <rthw.h>
#include <usbh_core.h>

#include "aic8800_wifi.h"
#include "aicwf_usbif.h"
#include "aicwf_shim.h"

#define DBG_TAG "aicwf.usb"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define URB_IDLE        0
#define URB_PENDING     1
#define URB_QUEUED      2

#define SHIM_MAX_HPORT  4
#define SHIM_SYNC_GRACE_MS 2000

/* RT-Smart class descriptors (CLASS_INFO_DEFINE objects in aic8800_usb.c / _btusb.c) */
extern const struct usbh_class_info g_aic8801_class_info, g_aic8800_boot_class_info,
    g_aic8800dc_class_info, g_aic8800dw_class_info, g_aic8800d81_class_info,
    g_aic8800d80_boot_class_info, g_aic8800d83_class_info, g_aic8800d84_class_info,
    g_aic8800d85_class_info, g_aic8800d86_class_info, g_aic8800d88_class_info,
    g_aic8800d81_v2_class_info, g_aic8800d83_v2_class_info, g_aic8800d84_v2_class_info,
    g_aic8800d85_v2_class_info, g_aic8800d86_v2_class_info, g_aic8800d88_v2_class_info,
    g_aic8800d41_class_info, g_aic8800d40_boot_class_info, g_aic8800d81x2_class_info,
    g_aic8800d89x2_class_info, g_aic8800d80x2_boot_class_info;
#ifdef AIC8800_WIFI_BLE
extern const struct usbh_class_info g_aic_btusb_8801, g_aic_btusb_88dc, g_aic_btusb_8d81,
    g_aic_btusb_8d41, g_aic_btusb_8d99, g_aic_btusb_88dd, g_aic_btusb_8d91;
#endif

static const struct usbh_class_info *const g_class_table[] = {
    &g_aic8801_class_info, &g_aic8800_boot_class_info, &g_aic8800dc_class_info,
    &g_aic8800dw_class_info, &g_aic8800d81_class_info, &g_aic8800d80_boot_class_info,
    &g_aic8800d83_class_info, &g_aic8800d84_class_info, &g_aic8800d85_class_info,
    &g_aic8800d86_class_info, &g_aic8800d88_class_info, &g_aic8800d81_v2_class_info,
    &g_aic8800d83_v2_class_info, &g_aic8800d84_v2_class_info, &g_aic8800d85_v2_class_info,
    &g_aic8800d86_v2_class_info, &g_aic8800d88_v2_class_info, &g_aic8800d41_class_info,
    &g_aic8800d40_boot_class_info, &g_aic8800d81x2_class_info, &g_aic8800d89x2_class_info,
    &g_aic8800d80x2_boot_class_info,
#ifdef AIC8800_WIFI_BLE
    &g_aic_btusb_8801, &g_aic_btusb_88dc, &g_aic_btusb_8d81, &g_aic_btusb_8d41,
    &g_aic_btusb_8d99, &g_aic_btusb_88dd, &g_aic_btusb_8d91,
#endif
};

struct shim_event
{
    struct shim_event *next;
    rt_uint8_t attach;
    struct aicwf_usbif *usbif;
    struct aicwf_usbif_info info;
};

struct shim_waiter
{
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int done;
};

static struct rt_shim_wait g_event_wait;
static struct shim_event *g_event_head, *g_event_tail;
static struct rt_shim_wait g_done_wait;
static struct usbh_urb *g_done_head, *g_done_tail;
static pthread_mutex_t g_hport_lock = PTHREAD_MUTEX_INITIALIZER;
static struct usbh_hubport *g_hports[SHIM_MAX_HPORT];
static rt_bool_t g_started;
static volatile unsigned g_attach_count;

/* ---- helpers ---- */

static void shim_wait_setup(struct rt_shim_wait *wait)
{
    pthread_mutex_init(&wait->lock, RT_NULL);
    rt_shim_cond_init(&wait->cond);
    wait->ready = 1;
}

static int shim_status_to_usb(int status)
{
    /* aicwf_usbif.h results are the negated CherryUSB USB_ERR_* numbers */
    return status;
}

static struct aicwf_usbif *shim_usbif_for_ep(struct usbh_hubport *hport,
                                             const struct usb_endpoint_descriptor *ep)
{
    int i, k;

    if (!hport || !ep)
    {
        return RT_NULL;
    }
    for (i = 0; i < CONFIG_USBHOST_MAX_INTERFACES; i++)
    {
        struct usbh_interface *intf = &hport->config.intf[i];

        if (!intf->shim_usbif)
        {
            continue;
        }
        for (k = 0; k < intf->altsetting[0].intf_desc.bNumEndpoints &&
                    k < CONFIG_USBHOST_MAX_ENDPOINTS; k++)
        {
            if (intf->altsetting[0].ep[k].ep_desc.bEndpointAddress == ep->bEndpointAddress)
            {
                return intf->shim_usbif;
            }
        }
    }
    return RT_NULL;
}

static struct aicwf_usbif *shim_any_usbif(struct usbh_hubport *hport)
{
    int i;

    if (!hport)
    {
        return RT_NULL;
    }
    for (i = 0; i < CONFIG_USBHOST_MAX_INTERFACES; i++)
    {
        if (hport->config.intf[i].shim_usbif)
        {
            return hport->config.intf[i].shim_usbif;
        }
    }
    return RT_NULL;
}

/* ---- completions ---- */

/* libbsd USB callback context, interface lock held: record and queue only */
static struct usbh_urb *g_delivering;     /* URB whose callback runs now */
static rt_thread_t g_completion_thread;

/* libbsd USB callback context, interface lock held: record and queue only */
static void shim_urb_done(void *arg, int status, uint32_t actual)
{
    struct usbh_urb *urb = arg;
    struct shim_waiter *waiter;

    pthread_mutex_lock(&g_done_wait.lock);
    urb->actual_length = actual;
    urb->errorcode = shim_status_to_usb(status);
    waiter = urb->shim_waiter;
#ifdef AICWF_SHIM_TRACE
    printf("shim done urb %p ep %02x status %d actual %u waiter %p state %u\n", (void *)urb,
           urb->ep ? urb->ep->bEndpointAddress : 0, status, actual, (void *)waiter,
           (unsigned)urb->shim_state);
#endif
    if (waiter)
    {
        /* synchronous submit: its caller finishes the URB */
        pthread_mutex_lock(&waiter->lock);
        waiter->done = 1;
        pthread_cond_broadcast(&waiter->cond);
        pthread_mutex_unlock(&waiter->lock);
    }
    else
    {
        urb->shim_state = URB_QUEUED;
        urb->hcpriv = RT_NULL;
        if (g_done_tail)
        {
            g_done_tail->hcpriv = urb;
        }
        else
        {
            g_done_head = urb;
        }
        g_done_tail = urb;
    }
    pthread_cond_broadcast(&g_done_wait.cond);
    pthread_mutex_unlock(&g_done_wait.lock);
}

/* g_done_wait.lock held; urb is queued */
static void shim_unqueue_locked(struct usbh_urb *urb)
{
    struct usbh_urb **link = &g_done_head, *prev = RT_NULL;

    while (*link && *link != urb)
    {
        prev = *link;
        link = (struct usbh_urb **)&(*link)->hcpriv;
    }
    if (*link == urb)
    {
        *link = urb->hcpriv;
        if (g_done_tail == urb)
        {
            g_done_tail = prev;
        }
    }
    urb->hcpriv = RT_NULL;
}

/* g_done_wait.lock held on entry and exit; runs the callback unlocked */
static void shim_deliver_locked(struct usbh_urb *urb)
{
    usbh_complete_callback_t complete = urb->complete;
    void *arg = urb->arg;
    int nbytes = urb->errorcode < 0 ? urb->errorcode : (int)urb->actual_length;
    struct usbh_urb *previous = g_delivering;

    /* the URB may be resubmitted from inside its own completion */
    urb->shim_state = URB_IDLE;
    g_delivering = urb;
    pthread_mutex_unlock(&g_done_wait.lock);
#ifdef AICWF_SHIM_TRACE
    printf("shim deliver urb %p nbytes %d\n", (void *)urb, nbytes);
#endif
    if (complete)
    {
        complete(arg, nbytes);
    }
    pthread_mutex_lock(&g_done_wait.lock);
    g_delivering = previous;
    pthread_cond_broadcast(&g_done_wait.cond);
}

static void shim_completion_thread(void *parameter)
{
    (void)parameter;
    pthread_mutex_lock(&g_done_wait.lock);
    for (;;)
    {
        struct usbh_urb *urb;

        while (!g_done_head)
        {
            pthread_cond_wait(&g_done_wait.cond, &g_done_wait.lock);
        }
        urb = g_done_head;
        shim_unqueue_locked(urb);
        shim_deliver_locked(urb);
    }
}

int usbh_submit_urb(struct usbh_urb *urb)
{
    struct aicwf_usbif *usbif;
    struct shim_waiter waiter;
    uint32_t timeout;
    int result;

    if (!urb || !urb->hport || !urb->ep)
    {
        return -USB_ERR_INVAL;
    }
    if (!urb->hport->connected)
    {
        return -USB_ERR_NOTCONN;
    }
    usbif = shim_usbif_for_ep(urb->hport, urb->ep);
    if (!usbif)
    {
        return -USB_ERR_NODEV;
    }
    timeout = urb->timeout;
    if (timeout)
    {
        pthread_mutex_init(&waiter.lock, RT_NULL);
        rt_shim_cond_init(&waiter.cond);
        waiter.done = 0;
    }
    pthread_mutex_lock(&g_done_wait.lock);
    if (urb->shim_state != URB_IDLE)
    {
        pthread_mutex_unlock(&g_done_wait.lock);
        if (timeout)
        {
            pthread_cond_destroy(&waiter.cond);
            pthread_mutex_destroy(&waiter.lock);
        }
        return -USB_ERR_BUSY;
    }
    urb->shim_usbif = usbif;
    urb->actual_length = 0;
    urb->errorcode = -USB_ERR_BUSY;
    urb->shim_waiter = timeout ? &waiter : RT_NULL;
    urb->shim_state = URB_PENDING;
    pthread_mutex_unlock(&g_done_wait.lock);

    result = aicwf_usbif_submit(usbif, urb->ep->bEndpointAddress, urb->transfer_buffer,
                                urb->transfer_buffer_length, timeout,
                                (urb->transfer_flags & URB_ZERO_PACKET) != 0,
                                shim_urb_done, urb, &urb->shim_token);
    if (result != 0)
    {
        pthread_mutex_lock(&g_done_wait.lock);
        urb->shim_state = URB_IDLE;
        urb->shim_waiter = RT_NULL;
        urb->errorcode = result;
        pthread_cond_broadcast(&g_done_wait.cond);
        pthread_mutex_unlock(&g_done_wait.lock);
        if (timeout)
        {
            pthread_cond_destroy(&waiter.cond);
            pthread_mutex_destroy(&waiter.lock);
        }
        return result;
    }
    if (!timeout)
    {
        return 0;
    }

    /* synchronous: wait for the result (the URB timeout is also the USB timeout). The
     * libbsd side always reports a cancelled transfer, so the wait after cancelling
     * ends; the waiter must stay valid until it does. */
    {
        struct timespec deadline;
        rt_bool_t cancelled = RT_FALSE;

        rt_shim_deadline(&deadline, (rt_int32_t)(timeout + SHIM_SYNC_GRACE_MS));
        pthread_mutex_lock(&waiter.lock);
        while (!waiter.done)
        {
            if (pthread_cond_timedwait(&waiter.cond, &waiter.lock, &deadline) == ETIMEDOUT &&
                !waiter.done)
            {
                if (cancelled)
                {
                    LOG_W("ep 0x%02x: still waiting for a cancelled transfer",
                          urb->ep->bEndpointAddress);
                }
                else
                {
                    pthread_mutex_unlock(&waiter.lock);
                    aicwf_usbif_cancel(usbif, urb->shim_token);
                    pthread_mutex_lock(&waiter.lock);
                    cancelled = RT_TRUE;
                }
                rt_shim_deadline(&deadline, SHIM_SYNC_GRACE_MS);
            }
        }
        pthread_mutex_unlock(&waiter.lock);
        pthread_mutex_lock(&g_done_wait.lock);
        result = cancelled && urb->errorcode == -USB_ERR_SHUTDOWN ? -USB_ERR_TIMEOUT
                                                                 : urb->errorcode;
        urb->shim_waiter = RT_NULL;
        urb->shim_state = URB_IDLE;
        urb->timeout = 0;
        pthread_cond_broadcast(&g_done_wait.cond);
        pthread_mutex_unlock(&g_done_wait.lock);
    }
    pthread_cond_destroy(&waiter.cond);
    pthread_mutex_destroy(&waiter.lock);
    if (result == 0 && (urb->transfer_flags & URB_SHORT_NOT_OK) &&
        urb->actual_length < urb->transfer_buffer_length)
    {
        result = -USB_ERR_IO;
    }
    return result;
}

/*
 * Synchronous, as in the K230 port: when this returns the transfer is finished and its
 * completion callback has run (or the URB was idle); stack-allocated URBs may go away.
 */
int usbh_kill_urb(struct usbh_urb *urb)
{
    rt_bool_t self = rt_thread_self() == g_completion_thread && g_completion_thread;
    rt_bool_t cancelled = RT_FALSE;
    int waits = 0;

    if (!urb)
    {
        return -USB_ERR_INVAL;
    }
    pthread_mutex_lock(&g_done_wait.lock);
    for (;;)
    {
        if (urb->shim_state == URB_PENDING && !cancelled && urb->shim_usbif)
        {
            struct aicwf_usbif *usbif = urb->shim_usbif;
            uint32_t token = urb->shim_token;

            pthread_mutex_unlock(&g_done_wait.lock);
            aicwf_usbif_cancel(usbif, token);
            pthread_mutex_lock(&g_done_wait.lock);
            cancelled = RT_TRUE;
            continue;
        }
        if (urb->shim_state == URB_QUEUED && self)
        {
            /* called from a completion callback: deliver this one inline */
            shim_unqueue_locked(urb);
            shim_deliver_locked(urb);
            continue;
        }
        if (urb->shim_state == URB_IDLE && (g_delivering != urb || self))
        {
            break;
        }
        {
            struct timespec deadline;

            rt_shim_deadline(&deadline, 100);
            pthread_cond_timedwait(&g_done_wait.cond, &g_done_wait.lock, &deadline);
            if (++waits % 50 == 0)
            {
                LOG_W("kill ep 0x%02x: waiting for the transfer to finish",
                      urb->ep ? urb->ep->bEndpointAddress : 0);
            }
            if (urb->shim_state == URB_PENDING && waits % 50 == 0)
            {
                cancelled = RT_FALSE;   /* resubmitted meanwhile: cancel again */
            }
        }
    }
    pthread_mutex_unlock(&g_done_wait.lock);
    return 0;
}

int usbh_control_transfer(struct usbh_hubport *hport, struct usb_setup_packet *setup,
                          uint8_t *buffer)
{
    struct aicwf_usbif *usbif = shim_any_usbif(hport);
    uint8_t raw[8];
    int result;

    if (!setup)
    {
        return -USB_ERR_INVAL;
    }
    if (!usbif || !hport->connected)
    {
        return -USB_ERR_NOTCONN;
    }
    raw[0] = setup->bmRequestType;
    raw[1] = setup->bRequest;
    raw[2] = (uint8_t)setup->wValue;
    raw[3] = (uint8_t)(setup->wValue >> 8);
    raw[4] = (uint8_t)setup->wIndex;
    raw[5] = (uint8_t)(setup->wIndex >> 8);
    raw[6] = (uint8_t)setup->wLength;
    raw[7] = (uint8_t)(setup->wLength >> 8);
    result = aicwf_usbif_control(usbif, raw, buffer, 5000);
    return result < 0 ? result : result + 8;
}

int usbh_get_string_desc(struct usbh_hubport *hport, uint8_t index, uint8_t *output,
                         uint32_t output_len)
{
    struct aicwf_usbif *usbif = shim_any_usbif(hport);

    if (!usbif)
    {
        return -USB_ERR_NOTCONN;
    }
    return aicwf_usbif_string(usbif, index, (char *)output, output_len);
}

int usbh_hubport_reenumerate(struct usbh_hubport *hport)
{
    struct aicwf_usbif *usbif = shim_any_usbif(hport);

    return usbif ? aicwf_usbif_reenumerate(usbif) : -USB_ERR_NOTCONN;
}

int usbh_dwc2_reset_endpoint(struct usbh_hubport *hport, uint8_t endpoint)
{
    struct usb_endpoint_descriptor ep;
    struct aicwf_usbif *usbif;

    memset(&ep, 0, sizeof(ep));
    ep.bEndpointAddress = endpoint;
    usbif = shim_usbif_for_ep(hport, &ep);
    return usbif ? aicwf_usbif_clear_stall(usbif, endpoint) : -USB_ERR_NOTCONN;
}

/* ---- attach / detach ---- */

static struct usbh_hubport *shim_hport_get(const struct aicwf_usbif_info *info)
{
    struct usbh_hubport *hport = RT_NULL;
    int i, free_slot = -1;

    pthread_mutex_lock(&g_hport_lock);
    for (i = 0; i < SHIM_MAX_HPORT; i++)
    {
        if (g_hports[i] && g_hports[i]->shim_key == info->device_key && g_hports[i]->connected)
        {
            hport = g_hports[i];
            break;
        }
        if (!g_hports[i] && free_slot < 0)
        {
            free_slot = i;
        }
    }
    if (!hport && free_slot >= 0)
    {
        hport = rt_calloc(1, sizeof(*hport));
        if (hport)
        {
            hport->setup = rt_malloc_align(sizeof(struct usb_setup_packet), 8);
            if (!hport->setup)
            {
                rt_free(hport);
                hport = RT_NULL;
            }
        }
        if (hport)
        {
            hport->shim_key = info->device_key;
            hport->speed = info->speed;
            hport->port = (uint8_t)(free_slot + 1);
            memcpy(&hport->device_desc, info->device_desc, sizeof(hport->device_desc));
            memcpy(&hport->config.config_desc, info->config_desc,
                   sizeof(hport->config.config_desc));
            hport->connected = true;
            g_hports[free_slot] = hport;
        }
    }
    if (hport)
    {
        hport->shim_refs++;
    }
    pthread_mutex_unlock(&g_hport_lock);
    return hport;
}

static void shim_hport_put(struct usbh_hubport *hport)
{
    int i;
    rt_bool_t release = RT_FALSE;

    pthread_mutex_lock(&g_hport_lock);
    if (--hport->shim_refs == 0)
    {
        for (i = 0; i < SHIM_MAX_HPORT; i++)
        {
            if (g_hports[i] == hport)
            {
                g_hports[i] = RT_NULL;
            }
        }
        release = RT_TRUE;
    }
    pthread_mutex_unlock(&g_hport_lock);
    if (release)
    {
        rt_free_align(hport->setup);
        rt_free(hport);
    }
}

static const struct usbh_class_info *shim_match(const struct aicwf_usbif_info *info)
{
    const struct usb_interface_descriptor *idesc = (const void *)info->iface_desc;
    rt_size_t i;

    for (i = 0; i < sizeof(g_class_table) / sizeof(g_class_table[0]); i++)
    {
        const struct usbh_class_info *c = g_class_table[i];

        if ((c->match_flags & USB_CLASS_MATCH_VENDOR) && c->vid != info->vid)
            continue;
        if ((c->match_flags & USB_CLASS_MATCH_PRODUCT) && c->pid != info->pid)
            continue;
        if ((c->match_flags & USB_CLASS_MATCH_INTF_CLASS) && c->class != idesc->bInterfaceClass)
            continue;
        if ((c->match_flags & USB_CLASS_MATCH_INTF_SUBCLASS) &&
            c->subclass != idesc->bInterfaceSubClass)
            continue;
        if ((c->match_flags & USB_CLASS_MATCH_INTF_PROTOCOL) &&
            c->protocol != idesc->bInterfaceProtocol)
            continue;
        return c;
    }
    return RT_NULL;
}

static struct usbh_interface *shim_find_intf(struct usbh_hubport **hport_out,
                                            struct aicwf_usbif *usbif)
{
    int i, k;

    pthread_mutex_lock(&g_hport_lock);
    for (i = 0; i < SHIM_MAX_HPORT; i++)
    {
        struct usbh_hubport *hport = g_hports[i];

        if (!hport)
            continue;
        for (k = 0; k < CONFIG_USBHOST_MAX_INTERFACES; k++)
        {
            if (hport->config.intf[k].shim_usbif == usbif)
            {
                *hport_out = hport;
                pthread_mutex_unlock(&g_hport_lock);
                return &hport->config.intf[k];
            }
        }
    }
    pthread_mutex_unlock(&g_hport_lock);
    return RT_NULL;
}

static void shim_handle_attach(struct aicwf_usbif *usbif, const struct aicwf_usbif_info *info)
{
    const struct usb_interface_descriptor *idesc = (const void *)info->iface_desc;
    const struct usbh_class_info *cls;
    struct usbh_interface *intf;
    struct usbh_hubport *hport;
    uint8_t number = info->iface_number;
    int k, result;

    if (!aicwf_usbif_connected(usbif))
    {
        aicwf_usbif_release(usbif);
        return;
    }
    cls = shim_match(info);
    if (!cls)
    {
        LOG_I("%04x:%04x interface %u (%02x/%02x/%02x): no RT-Smart class driver",
              info->vid, info->pid, number, idesc->bInterfaceClass,
              idesc->bInterfaceSubClass, idesc->bInterfaceProtocol);
        aicwf_usbif_release(usbif);
        return;
    }
    if (number >= CONFIG_USBHOST_MAX_INTERFACES)
    {
        LOG_E("interface number %u out of range", number);
        aicwf_usbif_release(usbif);
        return;
    }
    hport = shim_hport_get(info);
    if (!hport)
    {
        LOG_E("no hub port slot for %04x:%04x", info->vid, info->pid);
        aicwf_usbif_release(usbif);
        return;
    }
    intf = &hport->config.intf[number];
    memset(intf, 0, sizeof(*intf));
    memcpy(&intf->altsetting[0].intf_desc, info->iface_desc,
           sizeof(intf->altsetting[0].intf_desc));
    intf->altsetting[0].intf_desc.bNumEndpoints = info->num_ep;
    for (k = 0; k < info->num_ep && k < CONFIG_USBHOST_MAX_ENDPOINTS; k++)
    {
        struct usb_endpoint_descriptor *ep = &intf->altsetting[0].ep[k].ep_desc;

        ep->bLength = USB_SIZEOF_ENDPOINT_DESC;
        ep->bDescriptorType = USB_DESCRIPTOR_TYPE_ENDPOINT;
        ep->bEndpointAddress = info->ep[k].addr;
        ep->bmAttributes = info->ep[k].attributes;
        ep->wMaxPacketSize = info->ep[k].max_packet;
        ep->bInterval = info->ep[k].interval;
    }
    intf->altsetting_num = 1;
    intf->class_driver = (struct usbh_class_driver *)cls->class_driver;
    intf->shim_class = cls;
    intf->shim_usbif = usbif;
    if (hport->config.config_desc.bNumInterfaces <= number)
    {
        hport->config.config_desc.bNumInterfaces = number + 1;
    }
    LOG_I("%04x:%04x interface %u -> %s", info->vid, info->pid, number,
          cls->class_driver->driver_name);
    result = cls->class_driver->connect(hport, number);
    if (result == 0)
    {
        intf->shim_connected = 1;
        /* bring the station up here, serialised with the detach events (the RT-Smart
         * build did this from the attach work through AIC8800_WIFI_AUTO_START) */
        aicwf_wlan_attach_done();
    }
    else
    {
        LOG_W("%s connect on interface %u returned %d", cls->class_driver->driver_name,
              number, result);
    }
    /* a detach that arrived during connect() is handled by the next event */
}

static void shim_handle_detach(struct aicwf_usbif *usbif)
{
    struct usbh_hubport *hport = RT_NULL;
    struct usbh_interface *intf = shim_find_intf(&hport, usbif);
    uint8_t number;

    if (!intf)
    {
        aicwf_usbif_release(usbif);
        return;
    }
    hport->connected = false;
    number = (uint8_t)(intf - hport->config.intf);
    if (intf->shim_connected && intf->class_driver && intf->class_driver->disconnect)
    {
        intf->class_driver->disconnect(hport, number);
    }
    pthread_mutex_lock(&g_hport_lock);
    intf->shim_connected = 0;
    intf->shim_usbif = RT_NULL;
    intf->class_driver = RT_NULL;
    pthread_mutex_unlock(&g_hport_lock);
    aicwf_usbif_release(usbif);
    shim_hport_put(hport);
}

static void shim_event_thread(void *parameter)
{
    (void)parameter;
    for (;;)
    {
        struct shim_event *event;

        pthread_mutex_lock(&g_event_wait.lock);
        while (!g_event_head)
        {
            pthread_cond_wait(&g_event_wait.cond, &g_event_wait.lock);
        }
        event = g_event_head;
        g_event_head = event->next;
        if (!g_event_head)
        {
            g_event_tail = RT_NULL;
        }
        pthread_mutex_unlock(&g_event_wait.lock);
        if (event->attach)
        {
            shim_handle_attach(event->usbif, &event->info);
        }
        else
        {
            shim_handle_detach(event->usbif);
        }
        rt_free(event);
    }
}

static void shim_queue_event(struct shim_event *event)
{
    pthread_mutex_lock(&g_event_wait.lock);
    event->next = RT_NULL;
    if (g_event_tail)
    {
        g_event_tail->next = event;
    }
    else
    {
        g_event_head = event;
    }
    g_event_tail = event;
    pthread_cond_signal(&g_event_wait.cond);
    pthread_mutex_unlock(&g_event_wait.lock);
}

/* explore-thread callbacks: queue only */
static void shim_class_attach(struct aicwf_usbif *usbif, const struct aicwf_usbif_info *info)
{
    struct shim_event *event = rt_calloc(1, sizeof(*event));

    g_attach_count++;

    if (!event)
    {
        aicwf_usbif_release(usbif);
        return;
    }
    event->attach = 1;
    event->usbif = usbif;
    event->info = *info;
    shim_queue_event(event);
}

static void shim_class_detach(struct aicwf_usbif *usbif)
{
    struct usbh_hubport *hport = RT_NULL;
    struct shim_event *event;

    /* make the driver see the disconnect at once, even while connect() runs */
    if (shim_find_intf(&hport, usbif) && hport)
    {
        hport->connected = false;
    }
    event = rt_calloc(1, sizeof(*event));
    if (!event)
    {
        LOG_E("out of memory queueing a detach");
        return;
    }
    event->attach = 0;
    event->usbif = usbif;
    shim_queue_event(event);
}

static const struct aicwf_usbif_class g_shim_class = {
    .attach = shim_class_attach,
    .detach = shim_class_detach,
};

unsigned aicwf_usbh_attach_count(void)
{
    return g_attach_count;
}

/* "wifi fwdiag": the interfaces the class drivers hold */
void aicwf_usbh_dump(void)
{
    int i, k;

    pthread_mutex_lock(&g_hport_lock);
    printf("usb: %u interface attach(es) since start\n", g_attach_count);
    for (i = 0; i < SHIM_MAX_HPORT; i++)
    {
        struct usbh_hubport *hport = g_hports[i];

        if (!hport)
            continue;
        printf("usb: %04x:%04x speed %u %s\n", hport->device_desc.idVendor,
               hport->device_desc.idProduct, hport->speed,
               hport->connected ? "connected" : "gone");
        for (k = 0; k < CONFIG_USBHOST_MAX_INTERFACES; k++)
        {
            struct usbh_interface *intf = &hport->config.intf[k];

            if (!intf->shim_usbif)
                continue;
            printf("usb:   iface %d %02x/%02x/%02x %u ep -> %s%s\n", k,
                   intf->altsetting[0].intf_desc.bInterfaceClass,
                   intf->altsetting[0].intf_desc.bInterfaceSubClass,
                   intf->altsetting[0].intf_desc.bInterfaceProtocol,
                   intf->altsetting[0].intf_desc.bNumEndpoints,
                   intf->shim_class ? intf->shim_class->class_driver->driver_name : "-",
                   intf->shim_connected ? " (connected)" : "");
        }
    }
    pthread_mutex_unlock(&g_hport_lock);
}

/* "wifi reprobe": re-enumerate the first attached AIC device (re-runs the loader) */
int aicwf_usbh_reprobe(void)
{
    struct aicwf_usbif *usbif = RT_NULL;
    int i;

    pthread_mutex_lock(&g_hport_lock);
    for (i = 0; i < SHIM_MAX_HPORT && !usbif; i++)
    {
        if (g_hports[i] && g_hports[i]->connected)
            usbif = shim_any_usbif(g_hports[i]);
    }
    pthread_mutex_unlock(&g_hport_lock);
    return usbif ? aicwf_usbif_reenumerate(usbif) : AICWF_USBIF_ENODEV;
}

void aicwf_usbh_start(void)
{
    rt_thread_t thread;
    rt_err_t result;

    if (g_started)
    {
        return;
    }
    g_started = RT_TRUE;
    shim_wait_setup(&g_event_wait);
    shim_wait_setup(&g_done_wait);
    thread = rt_thread_create("aic-usbc", shim_completion_thread, RT_NULL, 16384, 10, 0);
    g_completion_thread = thread;
    if (!thread || rt_thread_startup(thread) != RT_EOK)
    {
        LOG_E("completion thread failed to start");
        return;
    }
#ifdef AIC8800_WIFI_BLE
    result = aic8800_btusb_driver_init();
    if (result != RT_EOK)
    {
        LOG_W("Bluetooth HCI transport not available (%d)", result);
    }
#endif
    result = aic8800_usb_driver_init();
    if (result != RT_EOK)
    {
        LOG_E("AIC8800 USB driver initialisation failed (%d)", result);
        return;
    }
    thread = rt_thread_create("aic-usbh", shim_event_thread, RT_NULL, 65536,
                              AIC8800_WIFI_ATTACH_THREAD_PRIORITY, 0);
    if (!thread || rt_thread_startup(thread) != RT_EOK)
    {
        LOG_E("hub event thread failed to start");
        return;
    }
    if (aicwf_usbif_register(&g_shim_class) != 0)
    {
        LOG_E("USB attachment registration failed");
    }
}
