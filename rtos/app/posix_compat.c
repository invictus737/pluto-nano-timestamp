/*
 * POSIX *at() calls and flock() for the Rust std library on RTEMS, which has no *at system calls.
 * Paths relative to AT_FDCWD or absolute are served by the plain calls. A directory descriptor
 * works when it was opened with O_DIRECTORY (open/openat are wrapped and remember the path of
 * such descriptors until close; Rust std's remove_dir_all and nexus2's RF switch store open
 * directories that way); any other descriptor gives ENOSYS. flock() always succeeds: there is
 * one program in the image, so advisory locks between processes cannot conflict.
 *
 * rename() and fsync()/fdatasync() are wrapped (nexus2-link.sh: -Wl,--wrap=...) for every caller
 * in the image, Rust std included:
 * - RTEMS rename() looks the new name up with RTEMS_FS_EXCLUSIVE (cpukit/libcsupport/src/
 *   _rename_r.c, "FIXME: This is not POSIX conform"), so renaming over an existing file fails with
 *   EEXIST on every file system (IMFS, dosfs, JFFS2), although the POSIX users guide documents
 *   replacement. The usual "write a temporary file, rename it over the old one" save fails from
 *   the second save on. The wrapper replaces the target: the old file is moved aside first and
 *   put back if the second step fails (not atomic: a reset in between leaves <new>.rnm~).
 * - IMFS and JFFS2 have no fsync handler (rtems_filesystem_default_fsync_or_fdatasync: EINVAL);
 *   their writes are already in RAM / on the NOR flash when write() returns, so for regular files
 *   and directories EINVAL means "nothing to do" and File::sync_all() must not fail on it.
 * exit() is wrapped as well: newlib exit() ends in rtems_shutdown_executive(), so a nexus2 start
 * error (std::process::exit, clap) would stop the whole system - shell, SSH and Wi-Fi included.
 * Called from the thread registered with nexus_exit_guard(), it ends only that thread.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _GNU_SOURCE /* pthread_*affinity_np, CPU_SET */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nexus_mutex.h"
#include "nexus_station.h"

#ifndef AT_FDCWD
#define AT_FDCWD -2
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 8
#endif

#ifndef O_DIRECTORY
#define O_DIRECTORY 0x200000
#endif

/* ---- directory descriptors opened with O_DIRECTORY: fd -> absolute path ---- */

#define DIRFD_MAX 256 /* CONFIGURE_MAXIMUM_FILE_DESCRIPTORS */
static char *dirfd_path[DIRFD_MAX];
NEXUS_PI_MUTEX(dirfd_lock)

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);

/* absolute form of path (relative to the current directory when not absolute), or NULL */
static char *absolute(const char *path)
{
  if (path[0] == '/') {
    return strdup(path);
  }
  char cwd[256];
  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    return NULL;
  }
  size_t n = strlen(cwd) + strlen(path) + 2;
  char *a = malloc(n);
  if (a != NULL) {
    snprintf(a, n, "%s%s%s", cwd, cwd[strlen(cwd) - 1] == '/' ? "" : "/", path);
  }
  return a;
}

static void dirfd_remember(int fd, const char *abs_path)
{
  if (fd < 0 || fd >= DIRFD_MAX || abs_path == NULL) {
    return;
  }
  char *c = strdup(abs_path);
  pthread_mutex_lock(&dirfd_lock);
  free(dirfd_path[fd]);
  dirfd_path[fd] = c;
  pthread_mutex_unlock(&dirfd_lock);
}

/* path for an *at() call: absolute or AT_FDCWD as given, else joined to the directory of dirfd
 * (into buf). NULL with errno set when dirfd is unknown. */
static const char *at_path(int dirfd, const char *path, char *buf, size_t cap)
{
  if (path == NULL) {
    errno = EFAULT;
    return NULL;
  }
  if (path[0] == '/' || dirfd == AT_FDCWD) {
    return path;
  }
  const char *res = NULL;
  pthread_mutex_lock(&dirfd_lock);
  if (dirfd >= 0 && dirfd < DIRFD_MAX && dirfd_path[dirfd] != NULL) {
    const char *d = dirfd_path[dirfd];
    size_t dl = strlen(d);
    if (path[0] == '\0') {
      errno = ENOENT;
    } else if ((size_t)snprintf(buf, cap, "%s%s%s", d, (dl > 0 && d[dl - 1] == '/') ? "" : "/",
                                path) >= cap) {
      errno = ENAMETOOLONG;
    } else {
      res = buf;
    }
  } else {
    errno = (dirfd >= 0 && dirfd < DIRFD_MAX) ? ENOSYS : EBADF;
  }
  pthread_mutex_unlock(&dirfd_lock);
  return res;
}

int __wrap_open(const char *path, int flags, ...)
{
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  int fd = __real_open(path, flags, mode);
  if (fd >= 0 && (flags & O_DIRECTORY) != 0 && path != NULL) {
    char *a = absolute(path);
    dirfd_remember(fd, a);
    free(a);
  }
  return fd;
}

int __wrap_close(int fd)
{
  if (fd >= 0 && fd < DIRFD_MAX && dirfd_path[fd] != NULL) {
    pthread_mutex_lock(&dirfd_lock);
    free(dirfd_path[fd]);
    dirfd_path[fd] = NULL;
    pthread_mutex_unlock(&dirfd_lock);
  }
  return __real_close(fd);
}

int openat(int dirfd, const char *path, int flags, ...)
{
  char buf[256];
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  const char *p = at_path(dirfd, path, buf, sizeof(buf));
  return p != NULL ? __wrap_open(p, flags, mode) : -1;
}

int unlinkat(int dirfd, const char *path, int flag)
{
  char buf[256];
  const char *p = at_path(dirfd, path, buf, sizeof(buf));
  if (p == NULL) {
    return -1;
  }
  return (flag & AT_REMOVEDIR) ? rmdir(p) : unlink(p);
}

int mkdirat(int dirfd, const char *path, mode_t mode)
{
  char buf[256];
  const char *p = at_path(dirfd, path, buf, sizeof(buf));
  return p != NULL ? mkdir(p, mode) : -1;
}

int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath)
{
  char ob[256], nb[256];
  const char *o = at_path(olddirfd, oldpath, ob, sizeof(ob));
  const char *n = o != NULL ? at_path(newdirfd, newpath, nb, sizeof(nb)) : NULL;
  return n != NULL ? rename(o, n) : -1;
}

int flock(int fd, int operation)
{
  (void)fd;
  (void)operation;
  return 0;
}

int __real_rename(const char *oldpath, const char *newpath);
int __real_fsync(int fd);
int __real_fdatasync(int fd);

NEXUS_PI_MUTEX(rename_lock)

int __wrap_rename(const char *oldpath, const char *newpath)
{
  int rc = __real_rename(oldpath, newpath);
  if (rc == 0 || errno != EEXIST) {
    return rc;
  }
  struct stat so, sn;
  if (stat(oldpath, &so) != 0 || stat(newpath, &sn) != 0) {
    errno = EEXIST;
    return -1;
  }
  if (so.st_dev == sn.st_dev && so.st_ino == sn.st_ino) {
    return 0; /* POSIX: same file, nothing to do */
  }
  if (S_ISDIR(sn.st_mode) || S_ISDIR(so.st_mode)) {
    if (!S_ISDIR(so.st_mode)) {
      errno = EISDIR;
      return -1;
    }
    if (!S_ISDIR(sn.st_mode)) {
      errno = ENOTDIR;
      return -1;
    }
    if (rmdir(newpath) != 0) { /* POSIX replaces an empty directory only */
      return -1;
    }
    return __real_rename(oldpath, newpath);
  }
  char stash[256];
  if ((size_t)snprintf(stash, sizeof(stash), "%s.rnm~", newpath) >= sizeof(stash)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  pthread_mutex_lock(&rename_lock);
  (void)unlink(stash);
  rc = __real_rename(newpath, stash);
  if (rc == 0) {
    rc = __real_rename(oldpath, newpath);
    if (rc != 0) {
      int e = errno;
      (void)__real_rename(stash, newpath);
      errno = e;
    } else {
      (void)unlink(stash);
    }
  }
  pthread_mutex_unlock(&rename_lock);
  return rc;
}

static int sync_result(int fd, int rc)
{
  struct stat st;
  if (rc != 0 && errno == EINVAL && fstat(fd, &st) == 0 &&
      (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))) {
    return 0;
  }
  return rc;
}

int __wrap_fsync(int fd)
{
  return sync_result(fd, __real_fsync(fd));
}

int __wrap_fdatasync(int fd)
{
  return sync_result(fd, __real_fdatasync(fd));
}

/* ---- exit() of the station thread ---- */

void __real_exit(int status) __attribute__((noreturn));
static pthread_t exit_guard_thread;
static volatile int exit_guard_on;
static void (*exit_guard_cb)(int status);

/* exit() from the calling thread ends just this thread (cb(status) first); 0 = off */
void nexus_exit_guard(int on, void (*cb)(int status))
{
  exit_guard_cb = cb;
  exit_guard_thread = pthread_self();
  exit_guard_on = on;
}

void __wrap_exit(int status)
{
  if (exit_guard_on && pthread_equal(pthread_self(), exit_guard_thread)) {
    exit_guard_on = 0;
    fflush(stdout);
    fflush(stderr);
    printf("exit(%d) from the station thread: thread ended, system kept running\n", status);
    if (exit_guard_cb != NULL) {
      exit_guard_cb(status);
    }
    pthread_exit(NULL);
  }
  __real_exit(status);
}


/*
 * CPU placement of the station (Pluto RTOS), the Nexus-Light pattern: one core for the deadline
 * path, the other for everything that waits on I/O.
 *
 * RTEMS pthread_create does not inherit the creator's affinity (the attribute's set applies, all
 * CPUs by default) and, with the default PTHREAD_INHERIT_SCHED, a thread created by a radio
 * thread would inherit its real-time priority. Every thread created by a station thread (the
 * nexus2 entry thread marks itself with nexus_station_mark_self(), its descendants inherit the
 * mark through a start trampoline) is therefore placed here by layout, before it runs:
 *
 * - "light" (default): CPU0 at the station base priority (nexus_station_base_prio). nexus2's
 *   radio threads then move themselves (rf-front rt_thread.rs, NEXUS2_RTEMS_LAYOUT=light): the
 *   DSP/protocol roles and the SDR stream I/O to CPU1 (NEXUS2_RTEMS_IO_CPU=0 puts the I/O on
 *   CPU0 for an A/B); the Pluto driver's DMA service threads and interrupts go to CPU1 as well
 *   (main.c), so CPU0 serves USB, Wi-Fi and the network stack alone.
 * - "cpu1": the previous layout, every station thread on CPU1 with the inherited priority;
 *   CPU0 left to USB, Wi-Fi and the network stack.
 * - "float": no pinning, priorities only (scheduling A/B).
 *
 * An attribute that already names a single CPU (the Pluto driver's hook) or asks for explicit
 * scheduling keeps those settings. Threads created outside the station are never touched.
 */
#include <pthread.h>
#include <sched.h>

int nexus_station_layout = NEXUS_LAYOUT_LIGHT;
int nexus_station_base_prio = 4; /* POSIX SCHED_FIFO; low: below libbsd, shell and SSH */

static __thread int station_thread;

const char *nexus_station_layout_name(int layout)
{
  return layout == NEXUS_LAYOUT_CPU1 ? "cpu1" : layout == NEXUS_LAYOUT_FLOAT ? "float" : "light";
}

/* -1 when v is not a layout name */
int nexus_station_layout_parse(const char *v)
{
  if (v == NULL || strcmp(v, "light") == 0) {
    return NEXUS_LAYOUT_LIGHT;
  }
  if (strcmp(v, "cpu1") == 0) {
    return NEXUS_LAYOUT_CPU1;
  }
  if (strcmp(v, "float") == 0) {
    return NEXUS_LAYOUT_FLOAT;
  }
  return -1;
}

void nexus_station_mark_self(void)
{
  station_thread = 1;
}

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start)(void *), void *arg);

struct station_start {
  void *(*start)(void *);
  void *arg;
};

static void *station_trampoline(void *p)
{
  struct station_start s = *(struct station_start *)p;
  free(p);
  station_thread = 1;
  return s.start(s.arg);
}

/* the attribute's set names exactly one CPU */
static int single_cpu(const cpu_set_t *set)
{
  int n = 0;
  for (int cpu = 0; cpu < CPU_SETSIZE && n < 2; ++cpu) {
    n += CPU_ISSET(cpu, set) ? 1 : 0;
  }
  return n == 1;
}

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start)(void *), void *arg)
{
  if (!station_thread || nexus_station_layout == NEXUS_LAYOUT_FLOAT) {
    if (station_thread) {
      struct station_start *s = malloc(sizeof(*s));
      if (s != NULL) {
        s->start = start;
        s->arg = arg;
        int rc = __real_pthread_create(thread, attr, station_trampoline, s);
        if (rc != 0) {
          free(s);
        }
        return rc;
      }
    }
    return __real_pthread_create(thread, attr, start, arg);
  }
  pthread_attr_t a;
  if (attr != NULL) {
    a = *attr;
    /* the copy must own its CPU set: the original points into the caller's attribute */
    if (attr->affinityset != NULL && attr->affinitysetsize == sizeof(a.affinitysetpreallocated)) {
      a.affinitysetpreallocated = *attr->affinityset;
      a.affinityset = &a.affinitysetpreallocated;
    }
  } else if (pthread_attr_init(&a) != 0) {
    return __real_pthread_create(thread, attr, start, arg);
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  int explicit_cpu = pthread_attr_getaffinity_np(&a, sizeof(set), &set) == 0 && single_cpu(&set);
  if (!explicit_cpu) {
    CPU_ZERO(&set);
    CPU_SET(nexus_station_layout == NEXUS_LAYOUT_CPU1 ? 1 : 0, &set);
    (void)pthread_attr_setaffinity_np(&a, sizeof(set), &set);
  }
  int inherit = PTHREAD_INHERIT_SCHED;
  (void)pthread_attr_getinheritsched(&a, &inherit);
  if (nexus_station_layout == NEXUS_LAYOUT_LIGHT && inherit == PTHREAD_INHERIT_SCHED) {
    /* never inherit a radio thread's real-time priority on the I/O core */
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = nexus_station_base_prio;
    (void)pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
    (void)pthread_attr_setschedpolicy(&a, SCHED_FIFO);
    (void)pthread_attr_setschedparam(&a, &sp);
  }
  struct station_start *s = malloc(sizeof(*s));
  int rc;
  if (s != NULL) {
    s->start = start;
    s->arg = arg;
    rc = __real_pthread_create(thread, &a, station_trampoline, s);
    if (rc != 0) {
      free(s);
    }
  } else {
    rc = __real_pthread_create(thread, &a, start, arg);
  }
  if (attr == NULL) {
    (void)pthread_attr_destroy(&a);
  }
  return rc;
}

/* ---- console log ring ----
 * write() is wrapped (nexus2-link.sh): what any thread writes to the console descriptors 1 and 2
 * (nexus2 tracing, printf, libbsd/AIC driver messages) is also kept in a 256 KiB RAM ring, so a
 * headless station can be diagnosed over SSH with `log` without the serial console. Shell
 * sessions write to their pty descriptor and are not recorded.
 *
 * No lock anywhere on this path: a writer reserves its span with one atomic add and copies;
 * `log` copies the ring without stopping writers (a line being overwritten at the oldest edge
 * may come out torn). Radio threads (station threads above the station base priority) do not
 * write to the slow serial console at all: their bytes also go to a 32 KiB console ring that a
 * low-priority CPU0 thread forwards to the console (nexus_log_drainer_start); when the console
 * falls behind, their output is dropped and counted ("[console: N bytes dropped]"), it is never
 * waited for. Everything else writes synchronously, as before. */
#include <stdatomic.h>
#include <stdint.h>

#define NEXUS_LOG_RING (256u * 1024u) /* powers of two: positions are free-running uint32 */
#define NEXUS_CON_RING (32u * 1024u)
static char log_ring[NEXUS_LOG_RING];
static atomic_uint log_reserved;   /* bytes ever reserved */
static atomic_uint log_clear_mark; /* `log clear`: log_reserved at that time */
static char con_ring[NEXUS_CON_RING];
static atomic_uint con_reserved;   /* bytes ever reserved for the console */
static atomic_uint con_done;       /* of those, bytes fully copied */
static atomic_uint con_read;       /* bytes forwarded (or skipped) by the drainer */
static atomic_uint con_dropped;    /* radio-thread bytes the console had no room for */
static atomic_int con_drainer_on;

ssize_t __real_write(int fd, const void *buf, size_t n);

static void ring_copy(char *ring, uint32_t size, uint32_t start, const char *p, uint32_t n)
{
  uint32_t pos = start & (size - 1u);
  uint32_t first = size - pos;
  if (first > n) {
    first = n;
  }
  memcpy(ring + pos, p, first);
  if (n > first) {
    memcpy(ring, p + first, n - first);
  }
}

/* the calling thread is a radio thread: a station thread raised above the station base priority */
static int radio_thread(void)
{
  if (!station_thread || !atomic_load_explicit(&con_drainer_on, memory_order_relaxed)) {
    return 0;
  }
  int policy;
  struct sched_param sp;
  return pthread_getschedparam(pthread_self(), &policy, &sp) == 0 && policy != SCHED_OTHER &&
         sp.sched_priority > nexus_station_base_prio;
}

/* queue for the drainer; 0 = no room (dropped and counted) */
static int con_put(const char *p, uint32_t n)
{
  if (n > NEXUS_CON_RING / 2) {
    atomic_fetch_add_explicit(&con_dropped, n, memory_order_relaxed);
    return 0;
  }
  uint32_t start = atomic_load_explicit(&con_reserved, memory_order_relaxed);
  do {
    if (start + n - atomic_load_explicit(&con_read, memory_order_acquire) > NEXUS_CON_RING) {
      atomic_fetch_add_explicit(&con_dropped, n, memory_order_relaxed);
      return 0;
    }
  } while (!atomic_compare_exchange_weak_explicit(&con_reserved, &start, start + n,
                                                  memory_order_relaxed, memory_order_relaxed));
  ring_copy(con_ring, NEXUS_CON_RING, start, p, n);
  atomic_fetch_add_explicit(&con_done, n, memory_order_release);
  return 1;
}

ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
  if ((fd == 1 || fd == 2) && n > 0 && buf != NULL) {
    const char *p = buf;
    uint32_t len = n > NEXUS_LOG_RING ? NEXUS_LOG_RING : (uint32_t)n;
    uint32_t start = atomic_fetch_add_explicit(&log_reserved, len, memory_order_relaxed);
    ring_copy(log_ring, NEXUS_LOG_RING, start, p + (n - len), len);
    if (radio_thread()) {
      (void)con_put(p, n > UINT32_MAX ? UINT32_MAX : (uint32_t)n);
      return (ssize_t)n; /* the console gets it from the drainer, or it is counted as dropped */
    }
  }
  return __real_write(fd, buf, n);
}

static void *log_drainer(void *arg)
{
  (void)arg;
  uint32_t reported = 0;
  for (;;) {
    uint32_t rd = atomic_load_explicit(&con_read, memory_order_relaxed);
    uint32_t r1 = atomic_load_explicit(&con_reserved, memory_order_acquire);
    uint32_t done = atomic_load_explicit(&con_done, memory_order_acquire);
    uint32_t r2 = atomic_load_explicit(&con_reserved, memory_order_acquire);
    /* done == reserved, unchanged around the read: every reserved byte up to r1 is copied */
    if (r1 != rd && r1 == r2 && done == r1) {
      uint32_t pos = rd & (NEXUS_CON_RING - 1u);
      uint32_t n = r1 - rd;
      uint32_t first = NEXUS_CON_RING - pos;
      if (first > n) {
        first = n;
      }
      (void)__real_write(1, con_ring + pos, first);
      if (n > first) {
        (void)__real_write(1, con_ring, n - first);
      }
      atomic_store_explicit(&con_read, r1, memory_order_release);
      continue;
    }
    uint32_t dropped = atomic_load_explicit(&con_dropped, memory_order_relaxed);
    if (dropped != reported) {
      char line[64];
      int k = snprintf(line, sizeof(line), "[console: %u bytes dropped]\n", (unsigned)(dropped - reported));
      (void)__real_write(1, line, (size_t)k);
      reported = dropped;
    }
    /* a writer still copying: look again soon; idle: 5 ms */
    usleep(r1 != rd ? 1000 : 5000);
  }
  return NULL;
}

/* forwarder of the radio threads' console output: CPU0, below the network stack and the shell */
void nexus_log_drainer_start(void)
{
  static int started;
  if (started) {
    return;
  }
  pthread_attr_t at;
  struct sched_param sp = {.sched_priority = 5};
  cpu_set_t set;
  pthread_t t;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 8 * 1024);
  pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&at, SCHED_FIFO);
  pthread_attr_setschedparam(&at, &sp);
  CPU_ZERO(&set);
  CPU_SET(0, &set);
  pthread_attr_setaffinity_np(&at, sizeof(set), &set);
  if (__real_pthread_create(&t, &at, log_drainer, NULL) == 0) {
    started = 1;
    pthread_setname_np(t, "log-drain");
    atomic_store_explicit(&con_drainer_on, 1, memory_order_release);
  }
  pthread_attr_destroy(&at);
}

/* copy of the ring since the last `log clear`, oldest byte first; writers are never stopped */
static char *log_snapshot(size_t *out_len)
{
  uint32_t end = atomic_load_explicit(&log_reserved, memory_order_acquire);
  uint32_t len = end - atomic_load_explicit(&log_clear_mark, memory_order_relaxed);
  if (len > NEXUS_LOG_RING) {
    len = NEXUS_LOG_RING;
  }
  char *c = malloc(len + 1);
  if (c != NULL) {
    uint32_t pos = (end - len) & (NEXUS_LOG_RING - 1u);
    uint32_t first = NEXUS_LOG_RING - pos;
    if (first > len) {
      first = len;
    }
    memcpy(c, log_ring + pos, first);
    memcpy(c + first, log_ring, len - first);
    c[len] = '\0';
  }
  *out_len = c != NULL ? len : 0;
  return c;
}

int nexus_log_shell(int argc, char **argv)
{
  if (argc >= 2 && strcmp(argv[1], "clear") == 0) {
    atomic_store_explicit(&log_clear_mark, atomic_load_explicit(&log_reserved, memory_order_relaxed),
                          memory_order_relaxed);
    return 0;
  }
  size_t len;
  char *c = log_snapshot(&len);
  if (c == NULL) {
    printf("log: no memory\n");
    return 1;
  }
  for (size_t i = 0; i < len; ++i) {
    if (c[i] == '\0') {
      c[i] = ' ';
    }
  }
  if (argc >= 3 && strcmp(argv[1], "find") == 0) {
    /* every line containing any of the words */
    char *line = c;
    while (line != NULL && *line != '\0') {
      char *nl = strchr(line, '\n');
      if (nl != NULL) {
        *nl = '\0';
      }
      for (int a = 2; a < argc; ++a) {
        if (strstr(line, argv[a]) != NULL) {
          printf("%s\n", line);
          break;
        }
      }
      line = nl != NULL ? nl + 1 : NULL;
    }
  } else {
    size_t want = 8192;
    if (argc >= 2) {
      long v = strtol(argv[1], NULL, 0);
      if (v <= 0) {
        printf("usage: log [bytes] | log find <word>... | log clear\n");
        free(c);
        return 1;
      }
      want = (size_t)v;
    }
    const char *start = len > want ? c + (len - want) : c;
    fwrite(start, 1, strlen(start), stdout);
    unsigned dropped = atomic_load_explicit(&con_dropped, memory_order_relaxed);
    if (dropped != 0) {
      printf("\n(log: %u bytes of radio-thread output skipped the serial console; the ring keeps them)\n",
             dropped);
    }
    fflush(stdout);
  }
  free(c);
  return 0;
}
