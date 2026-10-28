/*
 * RT-Thread kernel services for the RT-Smart AIC8800 driver on RTEMS, built on POSIX
 * threads (see rtthread.h for the model). Timeouts are RT-Thread ticks = milliseconds.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE     /* pthread_setname_np(), getentropy() in newlib */
#include <rtthread.h>
#include <rthw.h>
#include <ipc/completion.h>
#include <ipc/workqueue.h>
#include <tick.h>

#include <limits.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/random.h>   /* getentropy(); newlib declares it in unistd.h */
#endif

#define DBG_TAG "aicwf.rtt"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#ifdef __APPLE__
/* host syntax/behaviour checks only: no monotonic condition variables there */
#define SHIM_COND_CLOCK CLOCK_REALTIME
#else
#define SHIM_COND_CLOCK CLOCK_MONOTONIC
#endif

/* RT-Thread priorities (0 = highest) mapped onto POSIX SCHED_FIFO priorities below the
 * libbsd kernel threads */
#define SHIM_PRIO_BASE      150
#ifdef __APPLE__
#define SHIM_STACK_MIN      (512 * 1024)    /* host sanitizer runs */
#else
#define SHIM_STACK_MIN      (16 * 1024)
#endif

/* ---------------------------------------------------------------- basics */

void rt_shim_assert(const char *expr, const char *func, int line)
{
    printf("aicwf: assertion \"%s\" failed in %s:%d\n", expr, func, line);
    for (;;)
    {
        sleep(1);
    }
}

void rt_shim_log(int level, const char *tag, const char *fmt, ...)
{
    static const char letters[] = "EWID";
    char line[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("[%c/%s] %s\n", letters[level < 0 || level > 3 ? 3 : level], tag, line);
}

static pthread_mutex_t g_irq_lock;
static pthread_once_t g_irq_once = PTHREAD_ONCE_INIT;

static void shim_irq_init(void)
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_irq_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

rt_base_t rt_hw_interrupt_disable(void)
{
    pthread_once(&g_irq_once, shim_irq_init);
    pthread_mutex_lock(&g_irq_lock);
    return 1;
}

void rt_hw_interrupt_enable(rt_base_t level)
{
    (void)level;
    pthread_mutex_unlock(&g_irq_lock);
}

static uint64_t shim_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t cpu_ticks_us(void)
{
    return shim_now_ns() / 1000ULL;
}

rt_tick_t rt_tick_get(void)
{
    return (rt_tick_t)(shim_now_ns() / 1000000ULL);
}

rt_tick_t rt_tick_from_millisecond(rt_int32_t ms)
{
    if (ms < 0)
    {
        return (rt_tick_t)RT_WAITING_FOREVER;
    }
    return (rt_tick_t)ms;
}

void *rt_malloc(rt_size_t size)
{
    return malloc(size ? size : 1);
}

void *rt_calloc(rt_size_t count, rt_size_t size)
{
    return calloc(count ? count : 1, size ? size : 1);
}

void *rt_realloc(void *ptr, rt_size_t size)
{
    return realloc(ptr, size);
}

void rt_free(void *ptr)
{
    free(ptr);
}

void *rt_malloc_align(rt_size_t size, rt_size_t align)
{
    void *ptr = RT_NULL;

    if (align < sizeof(void *))
    {
        align = sizeof(void *);
    }
    size = RT_ALIGN(size ? size : 1, align);
    if (posix_memalign(&ptr, align, size) != 0)
    {
        return RT_NULL;
    }
    return ptr;
}

void rt_free_align(void *ptr)
{
    free(ptr);
}

void rt_set_errno(rt_err_t error)
{
    errno = error < 0 ? -error : error;
}

rt_err_t rt_get_errno(void)
{
    return errno;
}

static void shim_object_init(struct rt_object *object, const char *name, rt_uint8_t type)
{
    memset(object->name, 0, sizeof(object->name));
    if (name)
    {
        strncpy(object->name, name, sizeof(object->name) - 1);
    }
    object->type = type | RT_Object_Class_Static;
    object->flag = 0;
    rt_list_init(&object->list);
}

static void shim_object_detach(struct rt_object *object)
{
    object->type = RT_Object_Class_Null;
}

rt_uint8_t rt_object_get_type(rt_object_t object)
{
    return object ? (rt_uint8_t)(object->type & ~RT_Object_Class_Static) : 0;
}

/* ---------------------------------------------------------------- wait primitive */

static void shim_deadline(struct timespec *ts, rt_int32_t ms);

static void shim_wait_init(struct rt_shim_wait *wait)
{
    pthread_mutex_init(&wait->lock, RT_NULL);
    rt_shim_cond_init(&wait->cond);
    wait->ready = 1;
}

static void shim_wait_destroy(struct rt_shim_wait *wait)
{
    if (!wait->ready)
    {
        return;
    }
    wait->ready = 0;
    pthread_cond_destroy(&wait->cond);
    pthread_mutex_destroy(&wait->lock);
}

void rt_shim_cond_init(pthread_cond_t *cond)
{
    pthread_condattr_t attr;

    pthread_condattr_init(&attr);
#ifndef __APPLE__
    pthread_condattr_setclock(&attr, SHIM_COND_CLOCK);
#endif
    pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
}

void rt_shim_deadline(struct timespec *ts, rt_int32_t ms)
{
    shim_deadline(ts, ms);
}

static void shim_deadline(struct timespec *ts, rt_int32_t ms)
{
    clock_gettime(SHIM_COND_CLOCK, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

/* wait->lock held; returns 0 when signalled, ETIMEDOUT at the deadline */
static int shim_wait_until(struct rt_shim_wait *wait, const struct timespec *deadline)
{
    if (!deadline)
    {
        return pthread_cond_wait(&wait->cond, &wait->lock);
    }
    return pthread_cond_timedwait(&wait->cond, &wait->lock, deadline);
}

/* ---------------------------------------------------------------- mutex */

rt_err_t rt_mutex_init(rt_mutex_t mutex, const char *name, rt_uint8_t flag)
{
    pthread_mutexattr_t attr;

    (void)flag;
    if (!mutex)
    {
        return -RT_EINVAL;
    }
    shim_object_init(&mutex->parent.parent, name, RT_Object_Class_Mutex);
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mutex->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    return RT_EOK;
}

rt_err_t rt_mutex_detach(rt_mutex_t mutex)
{
    if (!mutex || rt_object_get_type(&mutex->parent.parent) != RT_Object_Class_Mutex)
    {
        return -RT_EINVAL;
    }
    pthread_mutex_destroy(&mutex->lock);
    shim_object_detach(&mutex->parent.parent);
    return RT_EOK;
}

rt_mutex_t rt_mutex_create(const char *name, rt_uint8_t flag)
{
    rt_mutex_t mutex = rt_calloc(1, sizeof(*mutex));

    if (mutex)
    {
        rt_mutex_init(mutex, name, flag);
    }
    return mutex;
}

rt_err_t rt_mutex_delete(rt_mutex_t mutex)
{
    rt_err_t result = rt_mutex_detach(mutex);

    if (result == RT_EOK)
    {
        rt_free(mutex);
    }
    return result;
}

rt_err_t rt_mutex_take(rt_mutex_t mutex, rt_int32_t time)
{
    struct timespec deadline;
    int rc;

    if (!mutex || rt_object_get_type(&mutex->parent.parent) != RT_Object_Class_Mutex)
    {
        return -RT_EINVAL;
    }
    if (time == RT_WAITING_FOREVER || time < 0)
    {
        rc = pthread_mutex_lock(&mutex->lock);
    }
    else if (time == 0)
    {
        rc = pthread_mutex_trylock(&mutex->lock);
    }
    else
    {
#ifdef __APPLE__
        rc = pthread_mutex_lock(&mutex->lock);
        (void)deadline;
#else
        /* pthread_mutex_timedlock() measures against CLOCK_REALTIME */
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += time / 1000;
        deadline.tv_nsec += (long)(time % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L)
        {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        rc = pthread_mutex_timedlock(&mutex->lock, &deadline);
#endif
    }
    if (rc == 0)
    {
        return RT_EOK;
    }
    return (rc == ETIMEDOUT || rc == EBUSY) ? -RT_ETIMEOUT : -RT_ERROR;
}

rt_err_t rt_mutex_trytake(rt_mutex_t mutex)
{
    return rt_mutex_take(mutex, RT_WAITING_NO);
}

rt_err_t rt_mutex_release(rt_mutex_t mutex)
{
    if (!mutex || rt_object_get_type(&mutex->parent.parent) != RT_Object_Class_Mutex)
    {
        return -RT_EINVAL;
    }
    return pthread_mutex_unlock(&mutex->lock) == 0 ? RT_EOK : -RT_ERROR;
}

/* ---------------------------------------------------------------- semaphore */

rt_err_t rt_sem_init(rt_sem_t sem, const char *name, rt_uint32_t value, rt_uint8_t flag)
{
    (void)flag;
    if (!sem)
    {
        return -RT_EINVAL;
    }
    shim_object_init(&sem->parent.parent, name, RT_Object_Class_Semaphore);
    shim_wait_init(&sem->wait);
    sem->value = value;
    return RT_EOK;
}

rt_err_t rt_sem_detach(rt_sem_t sem)
{
    if (!sem || rt_object_get_type(&sem->parent.parent) != RT_Object_Class_Semaphore)
    {
        return -RT_EINVAL;
    }
    shim_wait_destroy(&sem->wait);
    shim_object_detach(&sem->parent.parent);
    return RT_EOK;
}

rt_sem_t rt_sem_create(const char *name, rt_uint32_t value, rt_uint8_t flag)
{
    rt_sem_t sem = rt_calloc(1, sizeof(*sem));

    if (sem)
    {
        rt_sem_init(sem, name, value, flag);
    }
    return sem;
}

rt_err_t rt_sem_delete(rt_sem_t sem)
{
    rt_err_t result = rt_sem_detach(sem);

    if (result == RT_EOK)
    {
        rt_free(sem);
    }
    return result;
}

rt_err_t rt_sem_take(rt_sem_t sem, rt_int32_t time)
{
    struct timespec deadline;
    rt_err_t result = RT_EOK;

    if (!sem || !sem->wait.ready)
    {
        return -RT_EINVAL;
    }
    if (time > 0)
    {
        shim_deadline(&deadline, time);
    }
    pthread_mutex_lock(&sem->wait.lock);
    while (sem->value == 0)
    {
        if (time == 0)
        {
            result = -RT_ETIMEOUT;
            break;
        }
        if (shim_wait_until(&sem->wait, time > 0 ? &deadline : RT_NULL) == ETIMEDOUT &&
            sem->value == 0)
        {
            result = -RT_ETIMEOUT;
            break;
        }
    }
    if (result == RT_EOK)
    {
        sem->value--;
    }
    pthread_mutex_unlock(&sem->wait.lock);
    return result;
}

rt_err_t rt_sem_trytake(rt_sem_t sem)
{
    return rt_sem_take(sem, RT_WAITING_NO);
}

rt_err_t rt_sem_release(rt_sem_t sem)
{
    if (!sem || !sem->wait.ready)
    {
        return -RT_EINVAL;
    }
    pthread_mutex_lock(&sem->wait.lock);
    if (sem->value < 0xffffU)
    {
        sem->value++;
    }
    pthread_cond_signal(&sem->wait.cond);
    pthread_mutex_unlock(&sem->wait.lock);
    return RT_EOK;
}

rt_err_t rt_sem_control(rt_sem_t sem, int cmd, void *arg)
{
    if (!sem || !sem->wait.ready)
    {
        return -RT_EINVAL;
    }
    if (cmd != RT_IPC_CMD_RESET)
    {
        return -RT_ERROR;
    }
    pthread_mutex_lock(&sem->wait.lock);
    sem->value = (rt_uint32_t)(rt_ubase_t)arg;
    pthread_cond_broadcast(&sem->wait.cond);
    pthread_mutex_unlock(&sem->wait.lock);
    return RT_EOK;
}

/* ---------------------------------------------------------------- event */

rt_err_t rt_event_init(rt_event_t event, const char *name, rt_uint8_t flag)
{
    (void)flag;
    if (!event)
    {
        return -RT_EINVAL;
    }
    shim_object_init(&event->parent.parent, name, RT_Object_Class_Event);
    shim_wait_init(&event->wait);
    event->set = 0;
    return RT_EOK;
}

rt_err_t rt_event_detach(rt_event_t event)
{
    if (!event || rt_object_get_type(&event->parent.parent) != RT_Object_Class_Event)
    {
        return -RT_EINVAL;
    }
    shim_wait_destroy(&event->wait);
    shim_object_detach(&event->parent.parent);
    return RT_EOK;
}

rt_err_t rt_event_send(rt_event_t event, rt_uint32_t set)
{
    if (!event || !event->wait.ready || !set)
    {
        return -RT_EINVAL;
    }
    pthread_mutex_lock(&event->wait.lock);
    event->set |= set;
    pthread_cond_broadcast(&event->wait.cond);
    pthread_mutex_unlock(&event->wait.lock);
    return RT_EOK;
}

rt_err_t rt_event_recv(rt_event_t event, rt_uint32_t set, rt_uint8_t opt,
                       rt_int32_t timeout, rt_uint32_t *recved)
{
    struct timespec deadline;
    rt_err_t result = RT_EOK;

    if (!event || !event->wait.ready || !set)
    {
        return -RT_EINVAL;
    }
    if (timeout > 0)
    {
        shim_deadline(&deadline, timeout);
    }
    pthread_mutex_lock(&event->wait.lock);
    for (;;)
    {
        rt_bool_t hit = (opt & RT_EVENT_FLAG_AND) ? (event->set & set) == set
                                                  : (event->set & set) != 0;

        if (hit)
        {
            if (recved)
            {
                *recved = event->set & set;
            }
            if (opt & RT_EVENT_FLAG_CLEAR)
            {
                event->set &= ~set;
            }
            break;
        }
        if (timeout == 0)
        {
            result = -RT_ETIMEOUT;
            break;
        }
        if (shim_wait_until(&event->wait, timeout > 0 ? &deadline : RT_NULL) == ETIMEDOUT)
        {
            hit = (opt & RT_EVENT_FLAG_AND) ? (event->set & set) == set
                                            : (event->set & set) != 0;
            if (!hit)
            {
                result = -RT_ETIMEOUT;
                break;
            }
        }
    }
    pthread_mutex_unlock(&event->wait.lock);
    return result;
}

rt_err_t rt_event_control(rt_event_t event, int cmd, void *arg)
{
    (void)arg;
    if (!event || !event->wait.ready)
    {
        return -RT_EINVAL;
    }
    if (cmd != RT_IPC_CMD_RESET)
    {
        return -RT_ERROR;
    }
    pthread_mutex_lock(&event->wait.lock);
    event->set = 0;
    pthread_cond_broadcast(&event->wait.cond);
    pthread_mutex_unlock(&event->wait.lock);
    return RT_EOK;
}

/* ---------------------------------------------------------------- message queue */

rt_mq_t rt_mq_create(const char *name, rt_size_t msg_size, rt_size_t max_msgs, rt_uint8_t flag)
{
    rt_mq_t mq;

    (void)flag;
    if (!msg_size || !max_msgs)
    {
        return RT_NULL;
    }
    mq = rt_calloc(1, sizeof(*mq));
    if (!mq)
    {
        return RT_NULL;
    }
    mq->pool = rt_calloc(max_msgs, msg_size);
    if (!mq->pool)
    {
        rt_free(mq);
        return RT_NULL;
    }
    shim_object_init(&mq->parent.parent, name, RT_Object_Class_MessageQueue);
    shim_wait_init(&mq->wait);
    mq->msg_size = msg_size;
    mq->max_msgs = max_msgs;
    return mq;
}

rt_err_t rt_mq_delete(rt_mq_t mq)
{
    if (!mq)
    {
        return -RT_EINVAL;
    }
    shim_wait_destroy(&mq->wait);
    shim_object_detach(&mq->parent.parent);
    rt_free(mq->pool);
    rt_free(mq);
    return RT_EOK;
}

static rt_err_t shim_mq_put(rt_mq_t mq, const void *buffer, rt_size_t size, rt_bool_t urgent)
{
    rt_size_t slot;

    if (!mq || !buffer || size > mq->msg_size)
    {
        return -RT_EINVAL;
    }
    pthread_mutex_lock(&mq->wait.lock);
    if (mq->entry >= mq->max_msgs)
    {
        pthread_mutex_unlock(&mq->wait.lock);
        return -RT_EFULL;
    }
    if (urgent)
    {
        mq->head = (mq->head + mq->max_msgs - 1) % mq->max_msgs;
        slot = mq->head;
    }
    else
    {
        slot = (mq->head + mq->entry) % mq->max_msgs;
    }
    memset(mq->pool + slot * mq->msg_size, 0, mq->msg_size);
    memcpy(mq->pool + slot * mq->msg_size, buffer, size);
    mq->entry++;
    pthread_cond_broadcast(&mq->wait.cond);
    pthread_mutex_unlock(&mq->wait.lock);
    return RT_EOK;
}

rt_err_t rt_mq_send(rt_mq_t mq, const void *buffer, rt_size_t size)
{
    return shim_mq_put(mq, buffer, size, RT_FALSE);
}

rt_err_t rt_mq_urgent(rt_mq_t mq, const void *buffer, rt_size_t size)
{
    return shim_mq_put(mq, buffer, size, RT_TRUE);
}

rt_err_t rt_mq_recv(rt_mq_t mq, void *buffer, rt_size_t size, rt_int32_t timeout)
{
    struct timespec deadline;
    rt_err_t result = RT_EOK;

    if (!mq || !buffer)
    {
        return -RT_EINVAL;
    }
    if (timeout > 0)
    {
        shim_deadline(&deadline, timeout);
    }
    pthread_mutex_lock(&mq->wait.lock);
    while (mq->entry == 0)
    {
        if (timeout == 0)
        {
            result = -RT_ETIMEOUT;
            break;
        }
        if (shim_wait_until(&mq->wait, timeout > 0 ? &deadline : RT_NULL) == ETIMEDOUT &&
            mq->entry == 0)
        {
            result = -RT_ETIMEOUT;
            break;
        }
    }
    if (result == RT_EOK)
    {
        memcpy(buffer, mq->pool + mq->head * mq->msg_size,
               size < mq->msg_size ? size : mq->msg_size);
        mq->head = (mq->head + 1) % mq->max_msgs;
        mq->entry--;
    }
    pthread_mutex_unlock(&mq->wait.lock);
    return result;
}

rt_err_t rt_mq_control(rt_mq_t mq, int cmd, void *arg)
{
    (void)arg;
    if (!mq || cmd != RT_IPC_CMD_RESET)
    {
        return -RT_ERROR;
    }
    pthread_mutex_lock(&mq->wait.lock);
    mq->entry = 0;
    mq->head = 0;
    pthread_mutex_unlock(&mq->wait.lock);
    return RT_EOK;
}

/* ---------------------------------------------------------------- memory pool
 * RT-Thread layout: every block is preceded by one pointer, which holds the next free
 * block while free and the owning pool while allocated (the USB TX queue checks it). */

rt_mp_t rt_mp_create(const char *name, rt_size_t block_count, rt_size_t block_size)
{
    rt_size_t stride, index;
    rt_uint8_t *block;
    rt_mp_t mp;

    if (!block_count || !block_size)
    {
        return RT_NULL;
    }
    block_size = RT_ALIGN(block_size, RT_ALIGN_SIZE);
    stride = block_size + sizeof(rt_uint8_t *);
    mp = rt_calloc(1, sizeof(*mp));
    if (!mp)
    {
        return RT_NULL;
    }
    mp->start_address = rt_malloc_align(stride * block_count, RT_ALIGN_SIZE);
    if (!mp->start_address)
    {
        rt_free(mp);
        return RT_NULL;
    }
    shim_object_init(&mp->parent, name, RT_Object_Class_MemPool);
    shim_wait_init(&mp->wait);
    mp->size = stride * block_count;
    mp->block_size = block_size;
    mp->block_total_count = block_count;
    mp->block_free_count = block_count;
    block = mp->start_address;
    for (index = 0; index < block_count; index++)
    {
        *(rt_uint8_t **)(block + index * stride) =
            index + 1 < block_count ? block + (index + 1) * stride : RT_NULL;
    }
    mp->block_list = block;
    return mp;
}

rt_err_t rt_mp_delete(rt_mp_t mp)
{
    if (!mp)
    {
        return -RT_EINVAL;
    }
    shim_wait_destroy(&mp->wait);
    shim_object_detach(&mp->parent);
    rt_free_align(mp->start_address);
    rt_free(mp);
    return RT_EOK;
}

void *rt_mp_alloc(rt_mp_t mp, rt_int32_t time)
{
    struct timespec deadline;
    rt_uint8_t *block = RT_NULL;

    if (!mp)
    {
        return RT_NULL;
    }
    if (time > 0)
    {
        shim_deadline(&deadline, time);
    }
    pthread_mutex_lock(&mp->wait.lock);
    while (!mp->block_list)
    {
        if (time == 0)
        {
            break;
        }
        if (shim_wait_until(&mp->wait, time > 0 ? &deadline : RT_NULL) == ETIMEDOUT &&
            !mp->block_list)
        {
            break;
        }
    }
    if (mp->block_list)
    {
        block = mp->block_list;
        mp->block_list = *(rt_uint8_t **)block;
        *(rt_uint8_t **)block = (rt_uint8_t *)mp;
        mp->block_free_count--;
    }
    pthread_mutex_unlock(&mp->wait.lock);
    return block ? block + sizeof(rt_uint8_t *) : RT_NULL;
}

void rt_mp_free(void *ptr)
{
    rt_uint8_t *block;
    rt_mp_t mp;

    if (!ptr)
    {
        return;
    }
    block = (rt_uint8_t *)ptr - sizeof(rt_uint8_t *);
    mp = *(rt_mp_t *)block;
    pthread_mutex_lock(&mp->wait.lock);
    *(rt_uint8_t **)block = mp->block_list;
    mp->block_list = block;
    mp->block_free_count++;
    pthread_cond_signal(&mp->wait.cond);
    pthread_mutex_unlock(&mp->wait.lock);
}

/* ---------------------------------------------------------------- threads */

static pthread_key_t g_thread_key;
static pthread_once_t g_thread_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_thread_lock = PTHREAD_MUTEX_INITIALIZER;

static void shim_thread_key_init(void)
{
    pthread_key_create(&g_thread_key, RT_NULL);
}

static void *shim_thread_main(void *parameter)
{
    rt_thread_t thread = parameter;
    rt_bool_t release;

    pthread_setspecific(g_thread_key, thread);
    thread->entry(thread->parameter);
    pthread_mutex_lock(&g_thread_lock);
    thread->exited = 1;
    release = thread->deleted;
    pthread_mutex_unlock(&g_thread_lock);
    if (release)
    {
        rt_free(thread);
    }
    return RT_NULL;
}

rt_thread_t rt_thread_create(const char *name, void (*entry)(void *parameter), void *parameter,
                             rt_uint32_t stack_size, rt_uint8_t priority, rt_uint32_t tick)
{
    rt_thread_t thread;

    (void)tick;
    pthread_once(&g_thread_once, shim_thread_key_init);
    if (!entry)
    {
        return RT_NULL;
    }
    thread = rt_calloc(1, sizeof(*thread));
    if (!thread)
    {
        return RT_NULL;
    }
    shim_object_init(&thread->parent, name, RT_Object_Class_Thread);
    thread->entry = entry;
    thread->parameter = parameter;
    thread->stack_size = stack_size;
    thread->current_priority = priority;
    return thread;
}

rt_err_t rt_thread_startup(rt_thread_t thread)
{
    pthread_attr_t attr;
    size_t stack;
    int rc;

    if (!thread || thread->started)
    {
        return -RT_EINVAL;
    }
    stack = thread->stack_size < SHIM_STACK_MIN ? SHIM_STACK_MIN : thread->stack_size;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#ifndef __APPLE__
    {
        struct sched_param param;
        int prio = SHIM_PRIO_BASE - (int)thread->current_priority;

        if (prio < sched_get_priority_min(SCHED_FIFO))
        {
            prio = sched_get_priority_min(SCHED_FIFO);
        }
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        param.sched_priority = prio;
        pthread_attr_setschedparam(&attr, &param);
    }
#endif
#if defined(__rtems__) && defined(AICWF_THREAD_CPU)
    {
        /* RT-Smart runs this driver on one core: keep its threads on one CPU */
        cpu_set_t cpus;

        CPU_ZERO(&cpus);
        CPU_SET(AICWF_THREAD_CPU, &cpus);
        pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
    }
#endif
    thread->started = 1;
    rc = pthread_create(&thread->tid, &attr, shim_thread_main, thread);
    if (rc == EPERM || rc == EINVAL)
    {
        /* scheduling or affinity attributes refused: default attributes */
        pthread_attr_destroy(&attr);
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, stack);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        rc = pthread_create(&thread->tid, &attr, shim_thread_main, thread);
    }
    pthread_attr_destroy(&attr);
    if (rc != 0)
    {
        thread->started = 0;
        LOG_E("thread %s: pthread_create failed (%d)", thread->parent.name, rc);
        return -RT_ENOMEM;
    }
#ifdef __rtems__
    pthread_setname_np(thread->tid, thread->parent.name);
#endif
    return RT_EOK;
}

rt_err_t rt_thread_delete(rt_thread_t thread)
{
    rt_bool_t release = RT_FALSE;

    if (!thread)
    {
        return -RT_EINVAL;
    }
    pthread_mutex_lock(&g_thread_lock);
    if (!thread->started || thread->exited)
    {
        release = RT_TRUE;
    }
    else
    {
        /* a running POSIX thread cannot be killed: it frees itself if it ever returns */
        thread->deleted = 1;
        LOG_W("thread %s still running at delete; left to finish", thread->parent.name);
    }
    pthread_mutex_unlock(&g_thread_lock);
    if (release)
    {
        rt_free(thread);
    }
    return RT_EOK;
}

rt_thread_t rt_thread_self(void)
{
    pthread_once(&g_thread_once, shim_thread_key_init);
    return pthread_getspecific(g_thread_key);
}

rt_err_t rt_thread_delay(rt_tick_t tick)
{
    struct timespec ts;

    ts.tv_sec = tick / 1000;
    ts.tv_nsec = (long)(tick % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
    {
    }
    return RT_EOK;
}

rt_err_t rt_thread_mdelay(rt_int32_t ms)
{
    return rt_thread_delay(ms > 0 ? (rt_tick_t)ms : 0);
}

rt_err_t rt_thread_yield(void)
{
    sched_yield();
    return RT_EOK;
}

/* ---------------------------------------------------------------- timers */

static struct rt_shim_wait g_timer_wait;
static rt_list_t g_timer_list = RT_LIST_OBJECT_INIT(g_timer_list);
static struct rt_timer *g_timer_running;
static rt_thread_t g_timer_thread;
static pthread_once_t g_timer_once = PTHREAD_ONCE_INIT;

static void shim_timer_thread(void *parameter);

static void shim_timer_init_once(void)
{
    shim_wait_init(&g_timer_wait);
    g_timer_thread = rt_thread_create("aic-tmr", shim_timer_thread, RT_NULL, 8192, 4, 0);
    if (!g_timer_thread || rt_thread_startup(g_timer_thread) != RT_EOK)
    {
        LOG_E("timer thread failed to start");
    }
}

/* g_timer_wait.lock held */
static void shim_timer_insert(struct rt_timer *timer)
{
    rt_list_t *node;

    rt_list_for_each(node, &g_timer_list)
    {
        struct rt_timer *other = rt_list_entry(node, struct rt_timer, row);

        if ((rt_int32_t)(other->timeout_tick - timer->timeout_tick) > 0)
        {
            break;
        }
    }
    rt_list_insert_before(node, &timer->row);
    timer->parent.flag |= RT_TIMER_FLAG_ACTIVATED;
}

static void shim_timer_thread(void *parameter)
{
    (void)parameter;
    pthread_mutex_lock(&g_timer_wait.lock);
    for (;;)
    {
        struct rt_timer *timer;
        struct timespec deadline;
        rt_int32_t wait_ms;
        rt_uint32_t generation;

        if (rt_list_isempty(&g_timer_list))
        {
            pthread_cond_wait(&g_timer_wait.cond, &g_timer_wait.lock);
            continue;
        }
        timer = rt_list_entry(g_timer_list.next, struct rt_timer, row);
        wait_ms = (rt_int32_t)(timer->timeout_tick - rt_tick_get());
        if (wait_ms > 0)
        {
            shim_deadline(&deadline, wait_ms);
            pthread_cond_timedwait(&g_timer_wait.cond, &g_timer_wait.lock, &deadline);
            continue;
        }
        rt_list_remove(&timer->row);
        timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;
        generation = ++timer->generation;
        g_timer_running = timer;
        pthread_mutex_unlock(&g_timer_wait.lock);
        timer->timeout_func(timer->parameter);
        pthread_mutex_lock(&g_timer_wait.lock);
        g_timer_running = RT_NULL;
        /* periodic timers continue unless stopped or restarted by the callback */
        if ((timer->parent.flag & RT_TIMER_FLAG_PERIODIC) &&
            !(timer->parent.flag & RT_TIMER_FLAG_ACTIVATED) &&
            timer->generation == generation &&
            rt_object_get_type(&timer->parent) == RT_Object_Class_Timer &&
            !(timer->parent.flag & 0x80))
        {
            timer->timeout_tick = rt_tick_get() + timer->init_tick;
            shim_timer_insert(timer);
        }
        timer->parent.flag &= ~0x80;
        pthread_cond_broadcast(&g_timer_wait.cond);
    }
}

void rt_timer_init(rt_timer_t timer, const char *name, void (*timeout)(void *parameter),
                   void *parameter, rt_tick_t time, rt_uint8_t flag)
{
    pthread_once(&g_timer_once, shim_timer_init_once);
    shim_object_init(&timer->parent, name, RT_Object_Class_Timer);
    timer->parent.flag = flag & ~RT_TIMER_FLAG_ACTIVATED;
    rt_list_init(&timer->row);
    timer->timeout_func = timeout;
    timer->parameter = parameter;
    timer->init_tick = time;
    timer->timeout_tick = 0;
    timer->generation = 0;
}

rt_timer_t rt_timer_create(const char *name, void (*timeout)(void *parameter),
                           void *parameter, rt_tick_t time, rt_uint8_t flag)
{
    rt_timer_t timer = rt_calloc(1, sizeof(*timer));

    if (timer)
    {
        rt_timer_init(timer, name, timeout, parameter, time, flag);
    }
    return timer;
}

rt_err_t rt_timer_start(rt_timer_t timer)
{
    if (!timer || rt_object_get_type(&timer->parent) != RT_Object_Class_Timer)
    {
        return -RT_EINVAL;
    }
    pthread_mutex_lock(&g_timer_wait.lock);
    if (timer->parent.flag & RT_TIMER_FLAG_ACTIVATED)
    {
        rt_list_remove(&timer->row);
    }
    timer->generation++;
    timer->parent.flag &= ~0x80;
    timer->timeout_tick = rt_tick_get() + timer->init_tick;
    shim_timer_insert(timer);
    pthread_cond_broadcast(&g_timer_wait.cond);
    pthread_mutex_unlock(&g_timer_wait.lock);
    return RT_EOK;
}

rt_err_t rt_timer_stop(rt_timer_t timer)
{
    rt_err_t result = RT_EOK;

    if (!timer || rt_object_get_type(&timer->parent) != RT_Object_Class_Timer)
    {
        return -RT_EINVAL;
    }
    pthread_mutex_lock(&g_timer_wait.lock);
    if (timer->parent.flag & RT_TIMER_FLAG_ACTIVATED)
    {
        rt_list_remove(&timer->row);
        timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;
    }
    else
    {
        result = -RT_ERROR;
    }
    if (g_timer_running == timer)
    {
        /* do not re-arm a periodic timer whose callback is running now */
        timer->parent.flag |= 0x80;
    }
    pthread_mutex_unlock(&g_timer_wait.lock);
    return result;
}

static void shim_timer_quiesce(rt_timer_t timer)
{
    pthread_mutex_lock(&g_timer_wait.lock);
    if (timer->parent.flag & RT_TIMER_FLAG_ACTIVATED)
    {
        rt_list_remove(&timer->row);
        timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;
    }
    timer->parent.flag |= 0x80;
    /* the storage may be freed after this returns: wait for a running callback */
    while (g_timer_running == timer && rt_thread_self() != g_timer_thread)
    {
        pthread_cond_wait(&g_timer_wait.cond, &g_timer_wait.lock);
    }
    shim_object_detach(&timer->parent);
    pthread_mutex_unlock(&g_timer_wait.lock);
}

rt_err_t rt_timer_detach(rt_timer_t timer)
{
    if (!timer || rt_object_get_type(&timer->parent) != RT_Object_Class_Timer)
    {
        return -RT_EINVAL;
    }
    shim_timer_quiesce(timer);
    return RT_EOK;
}

rt_err_t rt_timer_delete(rt_timer_t timer)
{
    if (!timer || rt_object_get_type(&timer->parent) != RT_Object_Class_Timer)
    {
        return -RT_EINVAL;
    }
    shim_timer_quiesce(timer);
    rt_free(timer);
    return RT_EOK;
}

rt_err_t rt_timer_control(rt_timer_t timer, int cmd, void *arg)
{
    if (!timer || !arg)
    {
        return -RT_EINVAL;
    }
    pthread_mutex_lock(&g_timer_wait.lock);
    if (cmd == RT_TIMER_CTRL_SET_TIME)
    {
        timer->init_tick = *(rt_tick_t *)arg;
    }
    else if (cmd == RT_TIMER_CTRL_GET_TIME)
    {
        *(rt_tick_t *)arg = timer->init_tick;
    }
    pthread_mutex_unlock(&g_timer_wait.lock);
    return RT_EOK;
}

/* ---------------------------------------------------------------- completion */

void rt_completion_init(struct rt_completion *completion)
{
    /* completions often live on the stack: always build the wait object afresh */
    shim_wait_init(&completion->wait);
    completion->flag = 0;
    completion->initialized = 1;
}

rt_err_t rt_completion_wait(struct rt_completion *completion, rt_int32_t timeout)
{
    struct timespec deadline;
    rt_err_t result = RT_EOK;

    if (!completion->initialized)
    {
        return -RT_EINVAL;
    }
    if (timeout > 0)
    {
        shim_deadline(&deadline, timeout);
    }
    pthread_mutex_lock(&completion->wait.lock);
    while (!completion->flag)
    {
        if (timeout == 0)
        {
            result = -RT_ETIMEOUT;
            break;
        }
        if (shim_wait_until(&completion->wait, timeout > 0 ? &deadline : RT_NULL) ==
                ETIMEDOUT && !completion->flag)
        {
            result = -RT_ETIMEOUT;
            break;
        }
    }
    if (result == RT_EOK)
    {
        completion->flag = 0;
    }
    pthread_mutex_unlock(&completion->wait.lock);
    return result;
}

void rt_completion_done(struct rt_completion *completion)
{
    if (!completion->initialized)
    {
        return;
    }
    pthread_mutex_lock(&completion->wait.lock);
    completion->flag = 1;
    pthread_cond_broadcast(&completion->wait.cond);
    pthread_mutex_unlock(&completion->wait.lock);
}

/* ---------------------------------------------------------------- work queues */

static struct rt_workqueue *g_system_workqueue;
static pthread_once_t g_system_workqueue_once = PTHREAD_ONCE_INIT;

static void shim_workqueue_thread(void *parameter)
{
    struct rt_workqueue *queue = parameter;

    pthread_mutex_lock(&queue->wait.lock);
    while (!queue->quit)
    {
        struct rt_work *work;
        rt_list_t *node, *next;
        rt_int32_t wait_ms = -1;
        rt_tick_t now = rt_tick_get();

        /* move due delayed work to the run list, find the next deadline */
        rt_list_for_each_safe(node, next, &queue->delayed_list)
        {
            work = rt_list_entry(node, struct rt_work, list);
            if ((rt_int32_t)(work->timeout_tick - now) <= 0)
            {
                rt_list_remove(&work->list);
                rt_list_insert_before(&queue->work_list, &work->list);
            }
            else if (wait_ms < 0 || (rt_int32_t)(work->timeout_tick - now) < wait_ms)
            {
                wait_ms = (rt_int32_t)(work->timeout_tick - now);
            }
        }
        if (rt_list_isempty(&queue->work_list))
        {
            if (wait_ms < 0)
            {
                pthread_cond_wait(&queue->wait.cond, &queue->wait.lock);
            }
            else
            {
                struct timespec deadline;

                shim_deadline(&deadline, wait_ms);
                pthread_cond_timedwait(&queue->wait.cond, &queue->wait.lock, &deadline);
            }
            continue;
        }
        work = rt_list_entry(queue->work_list.next, struct rt_work, list);
        rt_list_remove(&work->list);
        work->flags &= ~(RT_WORK_STATE_PENDING | RT_WORK_STATE_SUBMITTING);
        queue->work_current = work;
        pthread_mutex_unlock(&queue->wait.lock);
        work->work_func(work, work->work_data);
        pthread_mutex_lock(&queue->wait.lock);
        queue->work_current = RT_NULL;
        pthread_cond_broadcast(&queue->wait.cond);
    }
    pthread_mutex_unlock(&queue->wait.lock);
}

void rt_work_init(struct rt_work *work, void (*work_func)(struct rt_work *work, void *work_data),
                  void *work_data)
{
    rt_list_init(&work->list);
    work->work_func = work_func;
    work->work_data = work_data;
    work->flags = 0;
    work->type = 0;
    work->timeout_tick = 0;
    work->workqueue = RT_NULL;
}

void rt_delayed_work_init(struct rt_delayed_work *work,
                          void (*work_func)(struct rt_work *work, void *work_data),
                          void *work_data)
{
    rt_work_init(&work->work, work_func, work_data);
    work->work.type = RT_WORK_TYPE_DELAYED;
}

struct rt_workqueue *rt_workqueue_create(const char *name, rt_uint16_t stack_size,
                                         rt_uint8_t priority)
{
    struct rt_workqueue *queue = rt_calloc(1, sizeof(*queue));

    if (!queue)
    {
        return RT_NULL;
    }
    rt_list_init(&queue->work_list);
    rt_list_init(&queue->delayed_list);
    shim_wait_init(&queue->wait);
    queue->work_thread = rt_thread_create(name, shim_workqueue_thread, queue,
                                          stack_size < 8192 ? 8192 : stack_size,
                                          priority, 10);
    if (!queue->work_thread || rt_thread_startup(queue->work_thread) != RT_EOK)
    {
        if (queue->work_thread)
        {
            rt_thread_delete(queue->work_thread);
        }
        shim_wait_destroy(&queue->wait);
        rt_free(queue);
        return RT_NULL;
    }
    return queue;
}

rt_err_t rt_workqueue_destroy(struct rt_workqueue *queue)
{
    if (!queue)
    {
        return -RT_EINVAL;
    }
    /* the worker thread exits; the queue object is kept (it may still be referenced) */
    pthread_mutex_lock(&queue->wait.lock);
    queue->quit = 1;
    pthread_cond_broadcast(&queue->wait.cond);
    pthread_mutex_unlock(&queue->wait.lock);
    return RT_EOK;
}

rt_err_t rt_workqueue_submit_work(struct rt_workqueue *queue, struct rt_work *work,
                                  rt_tick_t ticks)
{
    if (!queue || !work || !work->work_func)
    {
        return -RT_EINVAL;
    }
    if ((rt_int32_t)ticks < 0)
    {
        return -RT_EINVAL;
    }
    pthread_mutex_lock(&queue->wait.lock);
    if (work->flags & (RT_WORK_STATE_PENDING | RT_WORK_STATE_SUBMITTING))
    {
        rt_list_remove(&work->list);
    }
    work->flags = 0;
    work->workqueue = queue;
    if (ticks == 0)
    {
        rt_list_insert_before(&queue->work_list, &work->list);
        work->flags |= RT_WORK_STATE_PENDING;
    }
    else
    {
        work->timeout_tick = rt_tick_get() + ticks;
        rt_list_insert_before(&queue->delayed_list, &work->list);
        work->flags |= RT_WORK_STATE_SUBMITTING;
    }
    pthread_cond_broadcast(&queue->wait.cond);
    pthread_mutex_unlock(&queue->wait.lock);
    return RT_EOK;
}

rt_err_t rt_workqueue_dowork(struct rt_workqueue *queue, struct rt_work *work)
{
    return rt_workqueue_submit_work(queue, work, 0);
}

rt_err_t rt_workqueue_urgent_work(struct rt_workqueue *queue, struct rt_work *work)
{
    if (!queue || !work)
    {
        return -RT_EINVAL;
    }
    pthread_mutex_lock(&queue->wait.lock);
    if (work->flags & (RT_WORK_STATE_PENDING | RT_WORK_STATE_SUBMITTING))
    {
        rt_list_remove(&work->list);
    }
    work->flags = RT_WORK_STATE_PENDING;
    work->workqueue = queue;
    rt_list_insert_after(&queue->work_list, &work->list);
    pthread_cond_broadcast(&queue->wait.cond);
    pthread_mutex_unlock(&queue->wait.lock);
    return RT_EOK;
}

rt_err_t rt_workqueue_cancel_work(struct rt_workqueue *queue, struct rt_work *work)
{
    rt_err_t result = RT_EOK;

    if (!queue || !work)
    {
        return -RT_EINVAL;
    }
    pthread_mutex_lock(&queue->wait.lock);
    if (work->flags & (RT_WORK_STATE_PENDING | RT_WORK_STATE_SUBMITTING))
    {
        rt_list_remove(&work->list);
        work->flags &= ~(RT_WORK_STATE_PENDING | RT_WORK_STATE_SUBMITTING);
    }
    else if (queue->work_current == work)
    {
        result = -RT_EBUSY;
    }
    pthread_mutex_unlock(&queue->wait.lock);
    return result;
}

rt_err_t rt_workqueue_cancel_work_sync(struct rt_workqueue *queue, struct rt_work *work)
{
    if (!queue || !work)
    {
        return -RT_EINVAL;
    }
    pthread_mutex_lock(&queue->wait.lock);
    if (work->flags & (RT_WORK_STATE_PENDING | RT_WORK_STATE_SUBMITTING))
    {
        rt_list_remove(&work->list);
        work->flags &= ~(RT_WORK_STATE_PENDING | RT_WORK_STATE_SUBMITTING);
    }
    while (queue->work_current == work && rt_thread_self() != queue->work_thread)
    {
        pthread_cond_wait(&queue->wait.cond, &queue->wait.lock);
    }
    pthread_mutex_unlock(&queue->wait.lock);
    return RT_EOK;
}

rt_err_t rt_workqueue_cancel_all_work(struct rt_workqueue *queue)
{
    rt_list_t *node, *next;

    if (!queue)
    {
        return -RT_EINVAL;
    }
    pthread_mutex_lock(&queue->wait.lock);
    rt_list_for_each_safe(node, next, &queue->work_list)
    {
        struct rt_work *work = rt_list_entry(node, struct rt_work, list);

        rt_list_remove(node);
        work->flags &= ~(RT_WORK_STATE_PENDING | RT_WORK_STATE_SUBMITTING);
    }
    rt_list_for_each_safe(node, next, &queue->delayed_list)
    {
        struct rt_work *work = rt_list_entry(node, struct rt_work, list);

        rt_list_remove(node);
        work->flags &= ~(RT_WORK_STATE_PENDING | RT_WORK_STATE_SUBMITTING);
    }
    pthread_mutex_unlock(&queue->wait.lock);
    return RT_EOK;
}

static void shim_system_workqueue_init(void)
{
    g_system_workqueue = rt_workqueue_create("sys_work", RT_SYSTEM_WORKQUEUE_STACKSIZE,
                                             RT_SYSTEM_WORKQUEUE_PRIORITY);
    if (!g_system_workqueue)
    {
        LOG_E("system workqueue failed to start");
    }
}

static struct rt_workqueue *shim_system_workqueue(void)
{
    pthread_once(&g_system_workqueue_once, shim_system_workqueue_init);
    return g_system_workqueue;
}

rt_err_t rt_work_submit(struct rt_work *work, rt_tick_t ticks)
{
    return rt_workqueue_submit_work(shim_system_workqueue(), work, ticks);
}

rt_err_t rt_work_urgent(struct rt_work *work)
{
    return rt_workqueue_urgent_work(shim_system_workqueue(), work);
}

rt_err_t rt_work_cancel(struct rt_work *work)
{
    return rt_workqueue_cancel_work(shim_system_workqueue(), work);
}

rt_err_t rt_work_cancel_sync(struct rt_work *work)
{
    return rt_workqueue_cancel_work_sync(shim_system_workqueue(), work);
}

/* ---------------------------------------------------------------- devices */

static rt_ssize_t shim_hwrng_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    rt_uint8_t *out = buffer;
    rt_size_t done = 0;

    (void)dev;
    (void)pos;
    while (done < size)
    {
        rt_size_t chunk = size - done > 256 ? 256 : size - done;

        if (getentropy(out + done, chunk) != 0)
        {
            break;
        }
        done += chunk;
    }
    return (rt_ssize_t)done;
}

static struct rt_device g_hwrng = {
    .parent = { .name = "hwrng", .type = RT_Object_Class_Device | RT_Object_Class_Static },
    .type = RT_Device_Class_Miscellaneous,
    .read = shim_hwrng_read,
};

rt_device_t rt_device_find(const char *name)
{
    if (name && strcmp(name, "hwrng") == 0)
    {
        return &g_hwrng;
    }
    return RT_NULL;
}

rt_err_t rt_device_open(rt_device_t dev, rt_uint16_t oflag)
{
    if (!dev)
    {
        return -RT_EINVAL;
    }
    dev->open_flag = oflag | RT_DEVICE_OFLAG_OPEN;
    dev->ref_count++;
    return RT_EOK;
}

rt_err_t rt_device_close(rt_device_t dev)
{
    if (!dev)
    {
        return -RT_EINVAL;
    }
    if (dev->ref_count)
    {
        dev->ref_count--;
    }
    return RT_EOK;
}

rt_ssize_t rt_device_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    if (!dev || !dev->read)
    {
        return 0;
    }
    return dev->read(dev, pos, buffer, size);
}
