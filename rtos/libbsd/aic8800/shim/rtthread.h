/*
 * Minimal RT-Thread kernel API for the RT-Smart AIC8800 driver on RTEMS (POSIX threads).
 *
 * Only what the imported driver and WLAN offload sources use. Object layouts are this
 * shim's own; the fields those sources touch directly (object type and name, mempool
 * geometry, work flags, device reference count and user data) keep RT-Thread's names and
 * meaning. Every blocking primitive is a mutex + condition variable on CLOCK_MONOTONIC.
 * "Interrupt disable" is one global recursive lock: nothing in this port runs in an
 * interrupt handler, and the lock also serialises the two Cortex-A9 cores, which the
 * single-core RT-Smart code assumes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AICWF_RTTHREAD_H
#define AICWF_RTTHREAD_H

#include <rtconfig.h>

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int8_t rt_int8_t;
typedef int16_t rt_int16_t;
typedef int32_t rt_int32_t;
typedef int64_t rt_int64_t;
typedef uint8_t rt_uint8_t;
typedef uint16_t rt_uint16_t;
typedef uint32_t rt_uint32_t;
typedef uint64_t rt_uint64_t;
typedef int rt_bool_t;
typedef long rt_base_t;
typedef unsigned long rt_ubase_t;
typedef int rt_err_t;
typedef rt_uint32_t rt_time_t;
typedef rt_uint32_t rt_tick_t;
typedef rt_base_t rt_flag_t;
typedef size_t rt_size_t;
typedef ptrdiff_t rt_ssize_t;
typedef rt_base_t rt_off_t;
typedef rt_ubase_t rt_spinlock_t;

#define RT_TRUE                 1
#define RT_FALSE                0
#define RT_NULL                 ((void *)0)

#define rt_inline               static __inline
#define RT_UNUSED(x)            ((void)(x))
#define RT_WEAK                 __attribute__((weak))
#define rt_used                 __attribute__((used))
#define ALIGN(n)                __attribute__((aligned(n)))
#define rt_align(n)             __attribute__((aligned(n)))
#define RT_SECTION(x)           __attribute__((section(x)))

/* automatic initialisation is replaced by explicit calls in usbh_shim.c */
#define RT_SHIM_INIT_KEEP(fn)   static int (*const rt_init_##fn)(void) __attribute__((used)) = fn
#define INIT_BOARD_EXPORT(fn)       RT_SHIM_INIT_KEEP(fn)
#define INIT_PREV_EXPORT(fn)        RT_SHIM_INIT_KEEP(fn)
#define INIT_DEVICE_EXPORT(fn)      RT_SHIM_INIT_KEEP(fn)
#define INIT_COMPONENT_EXPORT(fn)   RT_SHIM_INIT_KEEP(fn)
#define INIT_ENV_EXPORT(fn)         RT_SHIM_INIT_KEEP(fn)
#define INIT_APP_EXPORT(fn)         RT_SHIM_INIT_KEEP(fn)

#define RT_EOK                  0
#define RT_ERROR                1
#define RT_ETIMEOUT             2
#define RT_EFULL                3
#define RT_EEMPTY               4
#define RT_ENOMEM               5
#define RT_ENOSYS               6
#define RT_EBUSY                7
#define RT_EIO                  8
#define RT_EINTR                9
#define RT_EINVAL               10
#define RT_ETRAP                11
#define RT_ENOENT               12
#define RT_ENOSPC               13
#define RT_EPERM                14

#define RT_WAITING_FOREVER      (-1)
#define RT_WAITING_NO           0

#define RT_IPC_FLAG_FIFO        0x00
#define RT_IPC_FLAG_PRIO        0x01
#define RT_IPC_CMD_UNKNOWN      0x00
#define RT_IPC_CMD_RESET        0x01

#define RT_EVENT_FLAG_AND       0x01
#define RT_EVENT_FLAG_OR        0x02
#define RT_EVENT_FLAG_CLEAR     0x04

#define RT_TIMER_FLAG_DEACTIVATED   0x0
#define RT_TIMER_FLAG_ACTIVATED     0x1
#define RT_TIMER_FLAG_ONE_SHOT      0x0
#define RT_TIMER_FLAG_PERIODIC      0x2
#define RT_TIMER_FLAG_HARD_TIMER    0x0
#define RT_TIMER_FLAG_SOFT_TIMER    0x4
#define RT_TIMER_CTRL_SET_TIME      0x0
#define RT_TIMER_CTRL_GET_TIME      0x1

#define RT_ALIGN(size, align)       (((size) + (align) - 1) & ~((align) - 1))
#define RT_ALIGN_DOWN(size, align)  ((size) & ~((align) - 1))
#define RT_ARRAY_SIZE(array)        (sizeof(array) / sizeof((array)[0]))

#define RT_ASSERT(expr)                                                         \
    do {                                                                        \
        if (!(expr))                                                            \
            rt_shim_assert(#expr, __func__, __LINE__);                          \
    } while (0)

/* ---- intrusive list (rtservice.h) ---- */
struct rt_list_node
{
    struct rt_list_node *next;
    struct rt_list_node *prev;
};
typedef struct rt_list_node rt_list_t;

struct rt_slist_node
{
    struct rt_slist_node *next;
};
typedef struct rt_slist_node rt_slist_t;

#define RT_LIST_OBJECT_INIT(object) { &(object), &(object) }

rt_inline void rt_list_init(rt_list_t *l)
{
    l->next = l->prev = l;
}

rt_inline void rt_list_insert_after(rt_list_t *l, rt_list_t *n)
{
    l->next->prev = n;
    n->next = l->next;
    l->next = n;
    n->prev = l;
}

rt_inline void rt_list_insert_before(rt_list_t *l, rt_list_t *n)
{
    l->prev->next = n;
    n->prev = l->prev;
    l->prev = n;
    n->next = l;
}

rt_inline void rt_list_remove(rt_list_t *n)
{
    n->next->prev = n->prev;
    n->prev->next = n->next;
    n->next = n->prev = n;
}

rt_inline int rt_list_isempty(const rt_list_t *l)
{
    return l->next == l;
}

rt_inline unsigned int rt_list_len(const rt_list_t *l)
{
    unsigned int len = 0;
    const rt_list_t *p = l;

    while (p->next != l)
    {
        p = p->next;
        len++;
    }
    return len;
}

#define rt_container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - (unsigned long)(&((type *)0)->member)))
#define rt_list_entry(node, type, member) rt_container_of(node, type, member)
#define rt_list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)
#define rt_list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)
#define rt_list_for_each_entry(pos, head, member)                       \
    for (pos = rt_list_entry((head)->next, __typeof__(*pos), member);   \
         &pos->member != (head);                                        \
         pos = rt_list_entry(pos->member.next, __typeof__(*pos), member))
#define rt_list_first_entry(ptr, type, member) rt_list_entry((ptr)->next, type, member)

/* ---- objects ---- */
enum rt_object_class_type
{
    RT_Object_Class_Null          = 0x00,
    RT_Object_Class_Thread        = 0x01,
    RT_Object_Class_Semaphore     = 0x02,
    RT_Object_Class_Mutex         = 0x03,
    RT_Object_Class_Event         = 0x04,
    RT_Object_Class_MailBox       = 0x05,
    RT_Object_Class_MessageQueue  = 0x06,
    RT_Object_Class_MemHeap       = 0x07,
    RT_Object_Class_MemPool       = 0x08,
    RT_Object_Class_Device        = 0x09,
    RT_Object_Class_Timer         = 0x0a,
    RT_Object_Class_Module        = 0x0b,
    RT_Object_Class_Memory        = 0x0c,
    RT_Object_Class_Unknown       = 0x0e,
    RT_Object_Class_Static        = 0x80
};

struct rt_object
{
    char name[RT_NAME_MAX];
    rt_uint8_t type;
    rt_uint8_t flag;
    rt_list_t list;
};
typedef struct rt_object *rt_object_t;

struct rt_ipc_object
{
    struct rt_object parent;
};

/* wait primitive shared by every blocking object */
struct rt_shim_wait
{
    pthread_mutex_t lock;
    pthread_cond_t cond;
    rt_uint8_t ready;
};

struct rt_mutex
{
    struct rt_ipc_object parent;
    pthread_mutex_t lock;
};
typedef struct rt_mutex *rt_mutex_t;

struct rt_semaphore
{
    struct rt_ipc_object parent;
    struct rt_shim_wait wait;
    rt_uint32_t value;
};
typedef struct rt_semaphore *rt_sem_t;

struct rt_event
{
    struct rt_ipc_object parent;
    struct rt_shim_wait wait;
    rt_uint32_t set;
};
typedef struct rt_event *rt_event_t;

struct rt_messagequeue
{
    struct rt_ipc_object parent;
    struct rt_shim_wait wait;
    rt_uint8_t *pool;
    rt_size_t msg_size;
    rt_size_t max_msgs;
    rt_size_t entry;
    rt_size_t head;
};
typedef struct rt_messagequeue *rt_mq_t;

struct rt_mempool
{
    struct rt_object parent;
    void *start_address;            /* RT-Thread layout: blocks of block_size + one header pointer */
    rt_size_t size;
    rt_size_t block_size;
    rt_uint8_t *block_list;
    rt_size_t block_total_count;
    rt_size_t block_free_count;
    struct rt_shim_wait wait;
};
typedef struct rt_mempool *rt_mp_t;

struct rt_timer
{
    struct rt_object parent;
    rt_list_t row;
    void (*timeout_func)(void *parameter);
    void *parameter;
    rt_tick_t init_tick;
    rt_tick_t timeout_tick;
    rt_uint32_t generation;
};
typedef struct rt_timer *rt_timer_t;

struct rt_thread
{
    struct rt_object parent;
    pthread_t tid;
    void (*entry)(void *parameter);
    void *parameter;
    rt_uint32_t stack_size;
    rt_uint8_t current_priority;
    rt_uint8_t started;
    rt_uint8_t exited;
    rt_uint8_t deleted;
    void *user_data;
};
typedef struct rt_thread *rt_thread_t;

/* ---- devices (only what the driver and offload framework touch) ---- */
#define RT_DEVICE_OFLAG_CLOSE   0x000
#define RT_DEVICE_OFLAG_RDONLY  0x001
#define RT_DEVICE_OFLAG_WRONLY  0x002
#define RT_DEVICE_OFLAG_RDWR    0x003
#define RT_DEVICE_OFLAG_OPEN    0x008
#define RT_DEVICE_FLAG_RDWR     0x003

enum rt_device_class_type
{
    RT_Device_Class_Char = 0,
    RT_Device_Class_Block,
    RT_Device_Class_NetIf,
    RT_Device_Class_MTD,
    RT_Device_Class_CAN,
    RT_Device_Class_RTC,
    RT_Device_Class_Sound,
    RT_Device_Class_Graphic,
    RT_Device_Class_I2CBUS,
    RT_Device_Class_USBDevice,
    RT_Device_Class_USBHost,
    RT_Device_Class_USBOTG,
    RT_Device_Class_SPIBUS,
    RT_Device_Class_SPIDevice,
    RT_Device_Class_SDIO,
    RT_Device_Class_PM,
    RT_Device_Class_Pipe,
    RT_Device_Class_Portal,
    RT_Device_Class_Timer,
    RT_Device_Class_Miscellaneous,
    RT_Device_Class_Sensor,
    RT_Device_Class_Touch,
    RT_Device_Class_PHY,
    RT_Device_Class_Unknown
};

struct rt_device;
typedef struct rt_device *rt_device_t;

struct rt_device
{
    struct rt_object parent;
    enum rt_device_class_type type;
    rt_uint16_t flag;
    rt_uint16_t open_flag;
    rt_uint8_t ref_count;
    rt_uint8_t device_id;
    rt_ssize_t (*read)(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size);
    void *user_data;
};

/* ---- kernel services ---- */
void rt_shim_assert(const char *expr, const char *func, int line);
/* condition variables of the shim run on CLOCK_MONOTONIC: init and deadline helpers */
void rt_shim_cond_init(pthread_cond_t *cond);
void rt_shim_deadline(struct timespec *ts, rt_int32_t ms);

rt_base_t rt_hw_interrupt_disable(void);
void rt_hw_interrupt_enable(rt_base_t level);

rt_tick_t rt_tick_get(void);
rt_tick_t rt_tick_from_millisecond(rt_int32_t ms);

void *rt_malloc(rt_size_t size);
void *rt_calloc(rt_size_t count, rt_size_t size);
void *rt_realloc(void *ptr, rt_size_t size);
void rt_free(void *ptr);
void *rt_malloc_align(rt_size_t size, rt_size_t align);
void rt_free_align(void *ptr);

#define rt_memset   memset
#define rt_memcpy   memcpy
#define rt_memmove  memmove
#define rt_memcmp   memcmp
#define rt_strlen   strlen
#define rt_strncpy  strncpy
#define rt_strcmp   strcmp
#define rt_strncmp  strncmp
#define rt_strstr   strstr
#define rt_snprintf snprintf
#define rt_sprintf  sprintf
#define rt_vsnprintf vsnprintf
#define rt_kprintf  printf

void rt_set_errno(rt_err_t error);
rt_err_t rt_get_errno(void);

rt_uint8_t rt_object_get_type(rt_object_t object);

rt_err_t rt_mutex_init(rt_mutex_t mutex, const char *name, rt_uint8_t flag);
rt_err_t rt_mutex_detach(rt_mutex_t mutex);
rt_mutex_t rt_mutex_create(const char *name, rt_uint8_t flag);
rt_err_t rt_mutex_delete(rt_mutex_t mutex);
rt_err_t rt_mutex_take(rt_mutex_t mutex, rt_int32_t time);
rt_err_t rt_mutex_trytake(rt_mutex_t mutex);
rt_err_t rt_mutex_release(rt_mutex_t mutex);

rt_err_t rt_sem_init(rt_sem_t sem, const char *name, rt_uint32_t value, rt_uint8_t flag);
rt_err_t rt_sem_detach(rt_sem_t sem);
rt_sem_t rt_sem_create(const char *name, rt_uint32_t value, rt_uint8_t flag);
rt_err_t rt_sem_delete(rt_sem_t sem);
rt_err_t rt_sem_take(rt_sem_t sem, rt_int32_t time);
rt_err_t rt_sem_trytake(rt_sem_t sem);
rt_err_t rt_sem_release(rt_sem_t sem);
rt_err_t rt_sem_control(rt_sem_t sem, int cmd, void *arg);

rt_err_t rt_event_init(rt_event_t event, const char *name, rt_uint8_t flag);
rt_err_t rt_event_detach(rt_event_t event);
rt_err_t rt_event_send(rt_event_t event, rt_uint32_t set);
rt_err_t rt_event_recv(rt_event_t event, rt_uint32_t set, rt_uint8_t opt,
                       rt_int32_t timeout, rt_uint32_t *recved);
rt_err_t rt_event_control(rt_event_t event, int cmd, void *arg);

rt_mq_t rt_mq_create(const char *name, rt_size_t msg_size, rt_size_t max_msgs, rt_uint8_t flag);
rt_err_t rt_mq_delete(rt_mq_t mq);
rt_err_t rt_mq_send(rt_mq_t mq, const void *buffer, rt_size_t size);
rt_err_t rt_mq_urgent(rt_mq_t mq, const void *buffer, rt_size_t size);
rt_err_t rt_mq_recv(rt_mq_t mq, void *buffer, rt_size_t size, rt_int32_t timeout);
rt_err_t rt_mq_control(rt_mq_t mq, int cmd, void *arg);

rt_mp_t rt_mp_create(const char *name, rt_size_t block_count, rt_size_t block_size);
rt_err_t rt_mp_delete(rt_mp_t mp);
void *rt_mp_alloc(rt_mp_t mp, rt_int32_t time);
void rt_mp_free(void *block);

void rt_timer_init(rt_timer_t timer, const char *name, void (*timeout)(void *parameter),
                   void *parameter, rt_tick_t time, rt_uint8_t flag);
rt_err_t rt_timer_detach(rt_timer_t timer);
rt_timer_t rt_timer_create(const char *name, void (*timeout)(void *parameter),
                           void *parameter, rt_tick_t time, rt_uint8_t flag);
rt_err_t rt_timer_delete(rt_timer_t timer);
rt_err_t rt_timer_start(rt_timer_t timer);
rt_err_t rt_timer_stop(rt_timer_t timer);
rt_err_t rt_timer_control(rt_timer_t timer, int cmd, void *arg);

rt_thread_t rt_thread_create(const char *name, void (*entry)(void *parameter), void *parameter,
                             rt_uint32_t stack_size, rt_uint8_t priority, rt_uint32_t tick);
rt_err_t rt_thread_startup(rt_thread_t thread);
rt_err_t rt_thread_delete(rt_thread_t thread);
rt_thread_t rt_thread_self(void);
rt_err_t rt_thread_delay(rt_tick_t tick);
rt_err_t rt_thread_mdelay(rt_int32_t ms);
rt_err_t rt_thread_yield(void);

rt_device_t rt_device_find(const char *name);
rt_err_t rt_device_open(rt_device_t dev, rt_uint16_t oflag);
rt_err_t rt_device_close(rt_device_t dev);
rt_ssize_t rt_device_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size);

#ifdef __cplusplus
}
#endif

#endif /* AICWF_RTTHREAD_H */
