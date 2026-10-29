/*
 * Mutexes shared with real-time threads (Pluto RTOS). RTEMS POSIX mutexes default to
 * PTHREAD_PRIO_NONE (a FIFO queue, no inheritance): a radio thread blocked on a mutex held by a
 * base-priority thread would wait behind everything above that holder. NEXUS_PI_MUTEX defines
 * a static mutex and switches it to PTHREAD_PRIO_INHERIT from a global constructor (RTEMS runs
 * them in the first thread, before Init and before any other thread exists).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NEXUS_MUTEX_H
#define NEXUS_MUTEX_H

#include <pthread.h>

static inline void nexus_pi_mutex_init(pthread_mutex_t *m)
{
  pthread_mutexattr_t a;
  if (pthread_mutexattr_init(&a) == 0) {
    (void)pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    (void)pthread_mutex_init(m, &a);
    (void)pthread_mutexattr_destroy(&a);
  }
}

#define NEXUS_PI_MUTEX(name)                                       \
  static pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER;         \
  __attribute__((constructor)) static void name##_pi_init(void)    \
  {                                                                \
    nexus_pi_mutex_init(&name);                                    \
  }

#endif
