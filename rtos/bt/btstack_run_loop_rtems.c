/*
 * BTstack run loop for RTEMS (see btstack_run_loop_rtems.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "btstack_run_loop_rtems.h"

#include "btstack_linked_list.h"
#include "btstack_run_loop.h"
#include "btstack_util.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __APPLE__
#define RL_CLOCK CLOCK_REALTIME /* host checks: no monotonic condition variables */
#else
#define RL_CLOCK CLOCK_MONOTONIC
#endif

static pthread_mutex_t rl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rl_cond;
static bool rl_poll_pending;
static bool rl_exit_requested;
static struct timespec rl_start;

static uint32_t rl_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  int64_t ms = (int64_t)(ts.tv_sec - rl_start.tv_sec) * 1000 + (ts.tv_nsec - rl_start.tv_nsec) / 1000000;
  return (uint32_t)ms;
}

static void rl_init(void)
{
  pthread_condattr_t ca;
  btstack_run_loop_base_init();
  pthread_condattr_init(&ca);
#ifndef __APPLE__
  pthread_condattr_setclock(&ca, RL_CLOCK);
#endif
  pthread_cond_init(&rl_cond, &ca);
  pthread_condattr_destroy(&ca);
  clock_gettime(CLOCK_MONOTONIC, &rl_start);
  rl_start.tv_nsec = 0;
  rl_poll_pending = false;
  rl_exit_requested = false;
}

static void rl_set_timer(btstack_timer_source_t *ts, uint32_t timeout_ms)
{
  ts->timeout = rl_now_ms() + timeout_ms;
}

/* the callback list is shared with other threads: pop under the lock, run without it */
static void rl_run_callbacks(void)
{
  for (;;) {
    pthread_mutex_lock(&rl_lock);
    btstack_context_callback_registration_t *cb =
        (btstack_context_callback_registration_t *)btstack_linked_list_pop(&btstack_run_loop_base_callbacks);
    pthread_mutex_unlock(&rl_lock);
    if (cb == NULL) {
      return;
    }
    cb->callback(cb->context);
  }
}

static void rl_execute(void)
{
  rl_exit_requested = false;
  for (;;) {
    rl_run_callbacks();

    pthread_mutex_lock(&rl_lock);
    bool poll = rl_poll_pending;
    rl_poll_pending = false;
    pthread_mutex_unlock(&rl_lock);
    if (poll) {
      btstack_run_loop_base_poll_data_sources();
    }

    btstack_run_loop_base_process_timers(rl_now_ms());

    pthread_mutex_lock(&rl_lock);
    if (rl_exit_requested) {
      pthread_mutex_unlock(&rl_lock);
      return;
    }
    if (!rl_poll_pending && btstack_run_loop_base_callbacks == NULL) {
      int32_t delta = btstack_run_loop_base_get_time_until_timeout(rl_now_ms());
      if (delta < 0) {
        pthread_cond_wait(&rl_cond, &rl_lock);
      } else if (delta > 0) {
        struct timespec dl;
        clock_gettime(RL_CLOCK, &dl);
        dl.tv_sec += delta / 1000;
        dl.tv_nsec += (long)(delta % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) {
          dl.tv_sec++;
          dl.tv_nsec -= 1000000000L;
        }
        (void)pthread_cond_timedwait(&rl_cond, &rl_lock, &dl);
      }
    }
    pthread_mutex_unlock(&rl_lock);
  }
}

static void rl_trigger_exit(void)
{
  pthread_mutex_lock(&rl_lock);
  rl_exit_requested = true;
  pthread_cond_signal(&rl_cond);
  pthread_mutex_unlock(&rl_lock);
}

static void rl_poll_from_irq(void)
{
  pthread_mutex_lock(&rl_lock);
  rl_poll_pending = true;
  pthread_cond_signal(&rl_cond);
  pthread_mutex_unlock(&rl_lock);
}

static void rl_execute_on_main_thread(btstack_context_callback_registration_t *cb)
{
  pthread_mutex_lock(&rl_lock);
  btstack_run_loop_base_add_callback(cb); /* no-op when already queued */
  pthread_cond_signal(&rl_cond);
  pthread_mutex_unlock(&rl_lock);
}

static const btstack_run_loop_t rl_rtems = {
    .init = rl_init,
    .add_data_source = btstack_run_loop_base_add_data_source,
    .remove_data_source = btstack_run_loop_base_remove_data_source,
    .enable_data_source_callbacks = btstack_run_loop_base_enable_data_source_callbacks,
    .disable_data_source_callbacks = btstack_run_loop_base_disable_data_source_callbacks,
    .set_timer = rl_set_timer,
    .add_timer = btstack_run_loop_base_add_timer,
    .remove_timer = btstack_run_loop_base_remove_timer,
    .execute = rl_execute,
    .dump_timer = btstack_run_loop_base_dump_timer,
    .get_time_ms = rl_now_ms,
    .poll_data_sources_from_irq = rl_poll_from_irq,
    .execute_on_main_thread = rl_execute_on_main_thread,
    .trigger_exit = rl_trigger_exit,
};

const btstack_run_loop_t *btstack_run_loop_rtems_get_instance(void)
{
  return &rl_rtems;
}
