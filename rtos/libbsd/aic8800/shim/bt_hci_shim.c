/*
 * RT-Thread Bluetooth HCI device API for the RT-Smart AIC8800 USB Bluetooth transport,
 * without the RT-Thread device framework: one controller, an H:4 receive ring, and the
 * aicwf_bt_*() calls of aicwf_bt.h for the host stack.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtthread.h>
#include <drivers/bt_hci.h>

#include "aicwf_bt.h"
#include "aicwf_shim.h"

#define DBG_TAG "aicwf.bt"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static struct rt_bt_hci_device *g_hci;

rt_err_t rt_bt_hci_register(struct rt_bt_hci_device *hci, const char *name,
                            const struct rt_bt_hci_ops *ops, rt_uint8_t *rx_buffer,
                            rt_uint16_t rx_buffer_size, void *user_data)
{
    if (!hci || !ops || !rx_buffer || rx_buffer_size < 64 || g_hci)
    {
        return -RT_EINVAL;
    }
    memset(&hci->parent, 0, sizeof(hci->parent));
    strncpy(hci->parent.parent.name, name ? name : "hci0", sizeof(hci->parent.parent.name) - 1);
    hci->parent.parent.type = RT_Object_Class_Device | RT_Object_Class_Static;
    hci->parent.user_data = user_data;
    hci->ops = ops;
    hci->rx_buffer = rx_buffer;
    hci->rx_size = rx_buffer_size;
    hci->rx_head = 0;
    hci->rx_count = 0;
    pthread_mutex_init(&hci->rx_wait.lock, RT_NULL);
    rt_shim_cond_init(&hci->rx_wait.cond);
    hci->rx_wait.ready = 1;
    rt_mutex_init(&hci->tx_lock, "hci-tx", RT_IPC_FLAG_PRIO);
    memset(&hci->stats, 0, sizeof(hci->stats));
    hci->opened = 0;
    hci->link_up = 0;
    g_hci = hci;
    return RT_EOK;
}

rt_err_t rt_bt_hci_register_auto(struct rt_bt_hci_device *hci, const struct rt_bt_hci_ops *ops,
                                 rt_uint8_t *rx_buffer, rt_uint16_t rx_buffer_size,
                                 void *user_data)
{
    return rt_bt_hci_register(hci, "hci0", ops, rx_buffer, rx_buffer_size, user_data);
}

rt_err_t rt_bt_hci_send(struct rt_bt_hci_device *hci, rt_uint8_t packet_type,
                        const rt_uint8_t *data, rt_size_t length)
{
    rt_err_t result;

    if (!hci || !hci->ops || !hci->ops->send || !data || !length)
    {
        return -RT_EINVAL;
    }
    rt_mutex_take(&hci->tx_lock, RT_WAITING_FOREVER);
    result = hci->ops->send(hci, packet_type, data, length);
    rt_mutex_release(&hci->tx_lock);
    if (result == RT_EOK)
    {
        hci->stats.tx_packets++;
        hci->stats.tx_bytes += (rt_uint32_t)length;
    }
    else
    {
        hci->stats.tx_errors++;
    }
    return result;
}

rt_err_t rt_bt_hci_receive(struct rt_bt_hci_device *hci, rt_uint8_t packet_type,
                           const rt_uint8_t *data, rt_size_t length)
{
    rt_size_t i, tail;

    if (!hci || !hci->rx_wait.ready || (!data && length))
    {
        return -RT_EINVAL;
    }
    pthread_mutex_lock(&hci->rx_wait.lock);
    if (hci->rx_count + 1 + length > hci->rx_size)
    {
        hci->stats.rx_dropped++;
        pthread_mutex_unlock(&hci->rx_wait.lock);
        return -RT_EFULL;
    }
    tail = (hci->rx_head + hci->rx_count) % hci->rx_size;
    hci->rx_buffer[tail] = packet_type;
    for (i = 0; i < length; i++)
    {
        hci->rx_buffer[(tail + 1 + i) % hci->rx_size] = data[i];
    }
    hci->rx_count += 1 + length;
    hci->stats.rx_packets++;
    hci->stats.rx_bytes += (rt_uint32_t)length;
    pthread_cond_broadcast(&hci->rx_wait.cond);
    pthread_mutex_unlock(&hci->rx_wait.lock);
    return RT_EOK;
}

void rt_bt_hci_flush(struct rt_bt_hci_device *hci)
{
    if (!hci || !hci->rx_wait.ready)
    {
        return;
    }
    pthread_mutex_lock(&hci->rx_wait.lock);
    hci->rx_head = 0;
    hci->rx_count = 0;
    pthread_mutex_unlock(&hci->rx_wait.lock);
}

rt_size_t rt_bt_hci_rx_length(struct rt_bt_hci_device *hci)
{
    rt_size_t count;

    if (!hci || !hci->rx_wait.ready)
    {
        return 0;
    }
    pthread_mutex_lock(&hci->rx_wait.lock);
    count = hci->rx_count;
    pthread_mutex_unlock(&hci->rx_wait.lock);
    return count;
}

void rt_bt_hci_get_stats(struct rt_bt_hci_device *hci, struct rt_bt_hci_stats *stats)
{
    if (hci && stats)
    {
        *stats = hci->stats;
    }
}

/* host reads fail (and wake up) until it reopens; link_up < 0 = leave unchanged */
static void hci_host_down(struct rt_bt_hci_device *hci, int link_up)
{
    if (!hci || !hci->rx_wait.ready)
    {
        return;
    }
    pthread_mutex_lock(&hci->rx_wait.lock);
    if (link_up >= 0)
    {
        hci->link_up = (rt_uint8_t)link_up;
    }
    if (link_up != 1)
    {
        hci->opened = 0;
    }
    pthread_cond_broadcast(&hci->rx_wait.cond);
    pthread_mutex_unlock(&hci->rx_wait.lock);
}

void rt_bt_hci_link(struct rt_bt_hci_device *hci, rt_bool_t up)
{
    hci_host_down(hci, up ? 1 : 0);
}

void rt_bt_hci_reset_notify(struct rt_bt_hci_device *hci)
{
    if (hci && hci->opened)
    {
        LOG_W("controller reset under the host: reopen required");
        hci_host_down(hci, -1);
    }
}

/* ---- aicwf_bt.h ---- */

int aicwf_bt_open(void)
{
    rt_err_t result;

    if (!g_hci || !g_hci->ops->open)
    {
        return -RT_EIO;
    }
    /* always ask the driver (its open is idempotent): after a USB replug or a controller
     * reset it is closed or restarted while a stale host flag would say open */
    rt_bt_hci_flush(g_hci);
    result = g_hci->ops->open(g_hci);
    pthread_mutex_lock(&g_hci->rx_wait.lock);
    g_hci->opened = result == RT_EOK;
    pthread_mutex_unlock(&g_hci->rx_wait.lock);
    return result;
}

int aicwf_bt_close(void)
{
    if (!g_hci)
    {
        return 0;
    }
    pthread_mutex_lock(&g_hci->rx_wait.lock);
    g_hci->opened = 0;
    pthread_cond_broadcast(&g_hci->rx_wait.cond);
    pthread_mutex_unlock(&g_hci->rx_wait.lock);
    /* also after a reset notification or a disconnect: the driver's close is idempotent */
    return g_hci->ops->close ? g_hci->ops->close(g_hci) : 0;
}

int aicwf_bt_send(uint8_t packet_type, const uint8_t *data, size_t length)
{
    if (!g_hci || !g_hci->opened)
    {
        return -RT_EIO;
    }
    return rt_bt_hci_send(g_hci, packet_type, data, length);
}

int aicwf_bt_read(uint8_t *buffer, size_t length, int timeout_ms)
{
    struct timespec deadline;
    size_t n = 0;

    if (!g_hci || !buffer || !length)
    {
        return -RT_EINVAL;
    }
    rt_shim_deadline(&deadline, timeout_ms > 0 ? timeout_ms : 0);
    pthread_mutex_lock(&g_hci->rx_wait.lock);
    if (!g_hci->opened)
    {
        pthread_mutex_unlock(&g_hci->rx_wait.lock);
        return -RT_EIO; /* closed, unplugged or reset: the host must reopen */
    }
    while (g_hci->rx_count == 0 && timeout_ms > 0 && g_hci->opened)
    {
        if (pthread_cond_timedwait(&g_hci->rx_wait.cond, &g_hci->rx_wait.lock, &deadline) ==
            ETIMEDOUT)
        {
            break;
        }
    }
    while (n < length && g_hci->rx_count > 0)
    {
        buffer[n++] = g_hci->rx_buffer[g_hci->rx_head];
        g_hci->rx_head = (g_hci->rx_head + 1) % g_hci->rx_size;
        g_hci->rx_count--;
    }
    pthread_mutex_unlock(&g_hci->rx_wait.lock);
    return (int)n;
}

int aicwf_bt_present(void)
{
    rt_uint16_t vid = 0, pid = 0;

    if (g_hci && g_hci->link_up)
    {
        return AICWF_BT_READY;
    }
    if (!aicwf_usbh_first_id(&vid, &pid))
    {
        return AICWF_BT_NO_DEVICE;
    }
    /* the DC variant's runtime identity has no Bluetooth function */
    return pid == 0x88dc ? AICWF_BT_UNSUPPORTED : AICWF_BT_STARTING;
}

int aicwf_bt_stats(struct aicwf_bt_stats *st)
{
    struct rt_bt_hci_stats s;
    rt_uint16_t vid = 0, pid = 0;

    if (!st)
    {
        return -RT_EINVAL;
    }
    memset(st, 0, sizeof(*st));
    if (aicwf_usbh_first_id(&vid, &pid))
    {
        st->vid = vid;
        st->pid = pid;
    }
    if (!g_hci)
    {
        return -RT_EIO;
    }
    pthread_mutex_lock(&g_hci->rx_wait.lock); /* rx counters are updated under this lock */
    s = g_hci->stats;
    pthread_mutex_unlock(&g_hci->rx_wait.lock);
    st->rx_packets = s.rx_packets;
    st->rx_bytes = s.rx_bytes;
    st->rx_dropped = s.rx_dropped;
    st->tx_packets = s.tx_packets;
    st->tx_bytes = s.tx_bytes;
    st->tx_errors = s.tx_errors;
    return 0;
}
