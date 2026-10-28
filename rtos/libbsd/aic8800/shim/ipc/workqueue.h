/*
 * RT-Thread <ipc/workqueue.h> for the AIC8800 port (same API and work flags).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AICWF_WORKQUEUE_H
#define AICWF_WORKQUEUE_H
#include <rtthread.h>
#include <ipc/completion.h>
enum
{
    RT_WORK_STATE_PENDING    = 0x0001,
    RT_WORK_STATE_SUBMITTING = 0x0002,
};
enum
{
    RT_WORK_TYPE_DELAYED     = 0x0001,
};
struct rt_work;
struct rt_workqueue
{
    rt_list_t work_list;
    rt_list_t delayed_list;
    struct rt_work *work_current;
    rt_thread_t work_thread;
    struct rt_shim_wait wait;
    rt_uint8_t quit;
};
struct rt_work
{
    rt_list_t list;
    void (*work_func)(struct rt_work *work, void *work_data);
    void *work_data;
    rt_uint16_t flags;
    rt_uint16_t type;
    rt_tick_t timeout_tick;
    struct rt_workqueue *workqueue;
};
struct rt_delayed_work
{
    struct rt_work work;
};
void rt_work_init(struct rt_work *work, void (*work_func)(struct rt_work *work, void *work_data),
                  void *work_data);
void rt_delayed_work_init(struct rt_delayed_work *work,
                          void (*work_func)(struct rt_work *work, void *work_data),
                          void *work_data);
struct rt_workqueue *rt_workqueue_create(const char *name, rt_uint16_t stack_size,
                                         rt_uint8_t priority);
rt_err_t rt_workqueue_destroy(struct rt_workqueue *queue);
rt_err_t rt_workqueue_dowork(struct rt_workqueue *queue, struct rt_work *work);
rt_err_t rt_workqueue_submit_work(struct rt_workqueue *queue, struct rt_work *work, rt_tick_t ticks);
rt_err_t rt_workqueue_cancel_work(struct rt_workqueue *queue, struct rt_work *work);
rt_err_t rt_workqueue_cancel_work_sync(struct rt_workqueue *queue, struct rt_work *work);
rt_err_t rt_workqueue_cancel_all_work(struct rt_workqueue *queue);
rt_err_t rt_workqueue_urgent_work(struct rt_workqueue *queue, struct rt_work *work);
rt_err_t rt_work_submit(struct rt_work *work, rt_tick_t ticks);
rt_err_t rt_work_urgent(struct rt_work *work);
rt_err_t rt_work_cancel(struct rt_work *work);
rt_err_t rt_work_cancel_sync(struct rt_work *work);
#endif
