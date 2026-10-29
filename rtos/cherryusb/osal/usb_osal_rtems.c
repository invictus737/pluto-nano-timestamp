/*
 * CherryUSB OS abstraction for RTEMS 6 (SMP).
 *
 * Threads are POSIX threads (SCHED_FIFO, POSIX priority 150 - prio as the AIC RT-Thread shim
 * maps priorities, pinned to NEXUS_USB_THREAD_CPU). Semaphores, mutexes and the message queue
 * are RTEMS self-contained objects (<rtems/thread.h>): no object limits, tick timeouts, so a
 * wall-clock step (SNTP) never shortens or extends a USB timeout. Timers run on one timer
 * thread.
 *
 * Critical section: CherryUSB uses it to serialise its schedule lists against the interrupt
 * handler. On RTEMS the EHCI interrupt is handled in the "usbh_irq" thread
 * (port/usb_glue_zynq.c), so the critical section is a recursive mutex shared by the handler and
 * every thread; it may be held across a blocking call. Leaving the outermost level drains the
 * CPU write buffers and the L2C-310 store buffer (Cortex-A9 erratum 769419 / PL310: Normal
 * non-cacheable writes can stay buffered), so the controller sees every QH/qTD write made in it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE /* pthread_attr_setaffinity_np, pthread_setname_np */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

#include <rtems.h>
#include <rtems/thread.h>

#include "usb_config.h"
#include "usb_errno.h"
#include "usb_log.h"
#include "usb_osal.h"

#define OSAL_PRIO_BASE  150
#define OSAL_STACK_MIN  (8 * 1024)

/* ---- time ---- */

static uint32_t osal_ms_to_ticks(uint32_t ms)
{
    uint64_t tps = rtems_clock_get_ticks_per_second();
    uint64_t ticks = ((uint64_t)ms * tps + 999U) / 1000U;

    if (ticks == 0) {
        ticks = 1;
    }
    if (ticks > 0x7fffffffU) {
        ticks = 0x7fffffffU;
    }
    return (uint32_t)ticks;
}

void usb_osal_msleep(uint32_t delay)
{
    if (delay == 0) {
        sched_yield();
        return;
    }
    (void)rtems_task_wake_after(osal_ms_to_ticks(delay));
}

/* ---- threads ---- */

struct osal_thread {
    usb_thread_entry_t entry;
    void *args;
    pthread_t tid;
};

static void *osal_thread_main(void *arg)
{
    struct osal_thread *t = arg;

    t->entry(t->args);
    return NULL;
}

usb_osal_thread_t usb_osal_thread_create(const char *name, uint32_t stack_size, uint32_t prio,
                                         usb_thread_entry_t entry, void *args)
{
    struct osal_thread *t = calloc(1, sizeof(*t));
    struct sched_param param;
    pthread_attr_t attr;
    cpu_set_t cpus;
    int rc, p;

    if (t == NULL) {
        USB_LOG_ERR("thread %s: no memory\r\n", name);
        return NULL;
    }
    t->entry = entry;
    t->args = args;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size < OSAL_STACK_MIN ? OSAL_STACK_MIN : stack_size);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    p = OSAL_PRIO_BASE - (int)prio;
    if (p < sched_get_priority_min(SCHED_FIFO)) {
        p = sched_get_priority_min(SCHED_FIFO);
    }
    if (p > sched_get_priority_max(SCHED_FIFO)) {
        p = sched_get_priority_max(SCHED_FIFO);
    }
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    param.sched_priority = p;
    pthread_attr_setschedparam(&attr, &param);
    CPU_ZERO(&cpus);
    CPU_SET(NEXUS_USB_THREAD_CPU, &cpus);
    pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
    rc = pthread_create(&t->tid, &attr, osal_thread_main, t);
    if (rc == EINVAL || rc == EPERM) {
        /* scheduling or affinity attributes refused (one CPU online): defaults */
        pthread_attr_destroy(&attr);
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, stack_size < OSAL_STACK_MIN ? OSAL_STACK_MIN : stack_size);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        rc = pthread_create(&t->tid, &attr, osal_thread_main, t);
    }
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        USB_LOG_ERR("thread %s: pthread_create failed (%d)\r\n", name, rc);
        free(t);
        return NULL;
    }
    {
        char n[16];

        strncpy(n, name, sizeof(n) - 1);
        n[sizeof(n) - 1] = '\0';
        (void)pthread_setname_np(t->tid, n);
    }
    return t;
}

void usb_osal_thread_delete(usb_osal_thread_t thread)
{
    /* CherryUSB deletes only the calling thread (NULL); the handle leaks with it, once */
    if (thread == NULL) {
        pthread_exit(NULL);
    }
}

void usb_osal_thread_schedule_other(void)
{
    sched_yield();
}

/* ---- semaphores ---- */

struct osal_sem {
    bool binary;
    rtems_binary_semaphore b;
    rtems_counting_semaphore c;
};

static struct osal_sem *osal_sem_new(bool binary, uint32_t count)
{
    struct osal_sem *s = calloc(1, sizeof(*s));

    if (s == NULL) {
        USB_LOG_ERR("semaphore: no memory\r\n");
        return NULL;
    }
    s->binary = binary;
    if (binary) {
        rtems_binary_semaphore_init(&s->b, "usb");
        if (count) {
            rtems_binary_semaphore_post(&s->b);
        }
    } else {
        rtems_counting_semaphore_init(&s->c, "usb", count);
    }
    return s;
}

usb_osal_sem_t usb_osal_sem_create(uint32_t initial_count)
{
    return osal_sem_new(true, initial_count ? 1 : 0);
}

usb_osal_sem_t usb_osal_sem_create_counting(uint32_t max_count)
{
    (void)max_count;
    return osal_sem_new(false, 0);
}

void usb_osal_sem_delete(usb_osal_sem_t sem)
{
    struct osal_sem *s = sem;

    if (s == NULL) {
        return;
    }
    if (s->binary) {
        rtems_binary_semaphore_destroy(&s->b);
    } else {
        rtems_counting_semaphore_destroy(&s->c);
    }
    free(s);
}

int usb_osal_sem_take(usb_osal_sem_t sem, uint32_t timeout)
{
    struct osal_sem *s = sem;
    int rc;

    if (timeout == USB_OSAL_WAITING_FOREVER) {
        if (s->binary) {
            rtems_binary_semaphore_wait(&s->b);
        } else {
            rtems_counting_semaphore_wait(&s->c);
        }
        return 0;
    }
    if (timeout == 0) {
        rc = s->binary ? rtems_binary_semaphore_try_wait(&s->b) :
                         rtems_counting_semaphore_try_wait(&s->c);
    } else if (s->binary) {
        rc = rtems_binary_semaphore_wait_timed_ticks(&s->b, osal_ms_to_ticks(timeout));
    } else {
        rc = rtems_counting_semaphore_wait_timed_ticks(&s->c, osal_ms_to_ticks(timeout));
    }
    return rc == 0 ? 0 : -USB_ERR_TIMEOUT;
}

int usb_osal_sem_give(usb_osal_sem_t sem)
{
    struct osal_sem *s = sem;

    if (s->binary) {
        rtems_binary_semaphore_post(&s->b);
    } else {
        rtems_counting_semaphore_post(&s->c);
    }
    return 0;
}

void usb_osal_sem_reset(usb_osal_sem_t sem)
{
    struct osal_sem *s = sem;

    if (s->binary) {
        while (rtems_binary_semaphore_try_wait(&s->b) == 0) {
        }
    } else {
        while (rtems_counting_semaphore_try_wait(&s->c) == 0) {
        }
    }
}

/* ---- mutexes (recursive: the glue takes a hub port's mutex around its own requests) ---- */

struct osal_mutex {
    rtems_recursive_mutex m;
    rtems_id owner;      /* written by the owner only */
    uint32_t depth;
};

usb_osal_mutex_t usb_osal_mutex_create(void)
{
    struct osal_mutex *m = calloc(1, sizeof(*m));

    if (m == NULL) {
        USB_LOG_ERR("mutex: no memory\r\n");
        return NULL;
    }
    rtems_recursive_mutex_init(&m->m, "usb");
    return m;
}

void usb_osal_mutex_delete(usb_osal_mutex_t mutex)
{
    struct osal_mutex *m = mutex;

    if (m == NULL) {
        return;
    }
    /* usbh_hubport_release() deletes the mutex while holding it */
    if (m->depth != 0 && m->owner == rtems_task_self()) {
        while (m->depth != 0) {
            m->depth--;
            rtems_recursive_mutex_unlock(&m->m);
        }
    }
    rtems_recursive_mutex_destroy(&m->m);
    free(m);
}

int usb_osal_mutex_take(usb_osal_mutex_t mutex)
{
    struct osal_mutex *m = mutex;

    rtems_recursive_mutex_lock(&m->m);
    m->owner = rtems_task_self();
    m->depth++;
    return 0;
}

int usb_osal_mutex_give(usb_osal_mutex_t mutex)
{
    struct osal_mutex *m = mutex;

    if (--m->depth == 0) {
        m->owner = 0;
    }
    rtems_recursive_mutex_unlock(&m->m);
    return 0;
}

/* ---- message queue (pointers) ---- */

struct osal_mq {
    rtems_mutex lock;
    rtems_counting_semaphore items;
    uint32_t size, head, count;
    uintptr_t slot[];
};

usb_osal_mq_t usb_osal_mq_create(uint32_t max_msgs)
{
    struct osal_mq *q = calloc(1, sizeof(*q) + max_msgs * sizeof(uintptr_t));

    if (q == NULL) {
        USB_LOG_ERR("mq: no memory\r\n");
        return NULL;
    }
    rtems_mutex_init(&q->lock, "usbmq");
    rtems_counting_semaphore_init(&q->items, "usbmq", 0);
    q->size = max_msgs;
    return q;
}

void usb_osal_mq_delete(usb_osal_mq_t mq)
{
    struct osal_mq *q = mq;

    if (q == NULL) {
        return;
    }
    rtems_counting_semaphore_destroy(&q->items);
    rtems_mutex_destroy(&q->lock);
    free(q);
}

int usb_osal_mq_send(usb_osal_mq_t mq, uintptr_t addr)
{
    struct osal_mq *q = mq;

    rtems_mutex_lock(&q->lock);
    if (q->count == q->size) {
        rtems_mutex_unlock(&q->lock);
        return -USB_ERR_NOMEM;
    }
    q->slot[(q->head + q->count) % q->size] = addr;
    q->count++;
    rtems_mutex_unlock(&q->lock);
    rtems_counting_semaphore_post(&q->items);
    return 0;
}

int usb_osal_mq_recv(usb_osal_mq_t mq, uintptr_t *addr, uint32_t timeout)
{
    struct osal_mq *q = mq;

    if (timeout == USB_OSAL_WAITING_FOREVER) {
        rtems_counting_semaphore_wait(&q->items);
    } else if (timeout == 0) {
        if (rtems_counting_semaphore_try_wait(&q->items) != 0) {
            return -USB_ERR_TIMEOUT;
        }
    } else if (rtems_counting_semaphore_wait_timed_ticks(&q->items, osal_ms_to_ticks(timeout)) != 0) {
        return -USB_ERR_TIMEOUT;
    }
    rtems_mutex_lock(&q->lock);
    *addr = q->slot[q->head];
    q->head = (q->head + 1) % q->size;
    q->count--;
    rtems_mutex_unlock(&q->lock);
    return 0;
}

/* ---- timers: one thread, deadlines in ticks ---- */

struct osal_timer {
    struct usb_osal_timer pub;
    struct osal_timer *next;
    rtems_interval due;
    bool armed;
};

static rtems_mutex g_tmr_lock = RTEMS_MUTEX_INITIALIZER("usbtmr");
static rtems_binary_semaphore g_tmr_wake = RTEMS_BINARY_SEMAPHORE_INITIALIZER("usbtmr");
static struct osal_timer *g_tmr_list;
static bool g_tmr_started;

static void osal_timer_thread(CONFIG_USB_OSAL_THREAD_SET_ARGV)
{
    (void)argument;
    for (;;) {
        struct osal_timer *t, *fire = NULL;
        usb_timer_handler_t handler = NULL;
        void *handler_arg = NULL;
        rtems_interval now = rtems_clock_get_ticks_since_boot();
        int32_t wait = -1;

        rtems_mutex_lock(&g_tmr_lock);
        for (t = g_tmr_list; t != NULL; t = t->next) {
            int32_t left;

            if (!t->armed) {
                continue;
            }
            left = (int32_t)(t->due - now);
            if (left <= 0) {
                fire = t;
                break;
            }
            if (wait < 0 || left < wait) {
                wait = left;
            }
        }
        if (fire != NULL) {
            handler = fire->pub.handler;
            handler_arg = fire->pub.argument;
            if (fire->pub.is_period) {
                fire->due = now + osal_ms_to_ticks(fire->pub.timeout_ms);
            } else {
                fire->armed = false;
            }
        }
        rtems_mutex_unlock(&g_tmr_lock);
        if (fire != NULL) {
            /* copied under the lock: the timer may be deleted while its handler runs */
            handler(handler_arg);
            continue;
        }
        if (wait < 0) {
            rtems_binary_semaphore_wait(&g_tmr_wake);
        } else {
            (void)rtems_binary_semaphore_wait_timed_ticks(&g_tmr_wake, (uint32_t)wait);
        }
    }
}

struct usb_osal_timer *usb_osal_timer_create(const char *name, uint32_t timeout_ms,
                                             usb_timer_handler_t handler, void *argument,
                                             bool is_period)
{
    struct osal_timer *t = calloc(1, sizeof(*t));
    bool start = false;

    (void)name;
    if (t == NULL) {
        return NULL;
    }
    t->pub.handler = handler;
    t->pub.argument = argument;
    t->pub.is_period = is_period;
    t->pub.timeout_ms = timeout_ms ? timeout_ms : 1;
    t->pub.timer = t;
    rtems_mutex_lock(&g_tmr_lock);
    t->next = g_tmr_list;
    g_tmr_list = t;
    if (!g_tmr_started) {
        g_tmr_started = true;
        start = true;
    }
    rtems_mutex_unlock(&g_tmr_lock);
    if (start && usb_osal_thread_create("usbh_tmr", 16384, 4, osal_timer_thread, NULL) == NULL) {
        USB_LOG_ERR("timer thread failed to start\r\n");
    }
    return &t->pub;
}

void usb_osal_timer_delete(struct usb_osal_timer *timer)
{
    struct osal_timer *t, **link;

    if (timer == NULL) {
        return;
    }
    t = timer->timer;
    rtems_mutex_lock(&g_tmr_lock);
    for (link = &g_tmr_list; *link != NULL; link = &(*link)->next) {
        if (*link == t) {
            *link = t->next;
            break;
        }
    }
    rtems_mutex_unlock(&g_tmr_lock);
    /* a handler running now uses the copies taken by the timer thread */
    free(t);
}

void usb_osal_timer_start(struct usb_osal_timer *timer)
{
    struct osal_timer *t = timer->timer;

    rtems_mutex_lock(&g_tmr_lock);
    t->due = rtems_clock_get_ticks_since_boot() + osal_ms_to_ticks(timer->timeout_ms);
    t->armed = true;
    rtems_mutex_unlock(&g_tmr_lock);
    rtems_binary_semaphore_post(&g_tmr_wake);
}

void usb_osal_timer_stop(struct usb_osal_timer *timer)
{
    struct osal_timer *t = timer->timer;

    rtems_mutex_lock(&g_tmr_lock);
    t->armed = false;
    rtems_mutex_unlock(&g_tmr_lock);
}

/* ---- critical section ---- */

static rtems_recursive_mutex g_crit = RTEMS_RECURSIVE_MUTEX_INITIALIZER("usbcrit");
static uint32_t g_crit_depth; /* written by the owner only */
/* one cacheable line: flushing it ends with an L2C-310 CACHE_SYNC */
static uint32_t g_crit_sync_line[8] __attribute__((aligned(32)));

void usb_osal_dma_barrier(void)
{
    __asm__ volatile("dsb" ::: "memory");
    rtems_cache_flush_multiple_data_lines(g_crit_sync_line, sizeof(g_crit_sync_line));
}

size_t usb_osal_enter_critical_section(void)
{
    rtems_recursive_mutex_lock(&g_crit);
    g_crit_depth++;
    return g_crit_depth;
}

void usb_osal_leave_critical_section(size_t flag)
{
    (void)flag;
    if (--g_crit_depth == 0) {
        usb_osal_dma_barrier();
    }
    rtems_recursive_mutex_unlock(&g_crit);
}

/* ---- memory ---- */

void *usb_osal_malloc(size_t size)
{
    return malloc(size);
}

void usb_osal_free(void *ptr)
{
    free(ptr);
}
