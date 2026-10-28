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

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
static pthread_mutex_t dirfd_lock = PTHREAD_MUTEX_INITIALIZER;

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

static pthread_mutex_t rename_lock = PTHREAD_MUTEX_INITIALIZER;

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
