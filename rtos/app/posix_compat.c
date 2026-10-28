/*
 * POSIX *at() calls and flock() for the Rust std library on RTEMS, which has no *at system calls.
 * Paths relative to AT_FDCWD or absolute are served by the plain calls; a real directory file
 * descriptor is not supported (ENOSYS) - nexus2 does not rely on it. flock() always succeeds:
 * there is one program in the image, so advisory locks between processes cannot conflict.
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
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef AT_FDCWD
#define AT_FDCWD -2
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 8
#endif

static int at_ok(int dirfd, const char *path)
{
  if (path == NULL) {
    errno = EFAULT;
    return 0;
  }
  if (path[0] == '/' || dirfd == AT_FDCWD) {
    return 1;
  }
  errno = ENOSYS;
  return 0;
}

int openat(int dirfd, const char *path, int flags, ...)
{
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  return at_ok(dirfd, path) ? open(path, flags, mode) : -1;
}

int unlinkat(int dirfd, const char *path, int flag)
{
  if (!at_ok(dirfd, path)) {
    return -1;
  }
  return (flag & AT_REMOVEDIR) ? rmdir(path) : unlink(path);
}

int mkdirat(int dirfd, const char *path, mode_t mode)
{
  return at_ok(dirfd, path) ? mkdir(path, mode) : -1;
}

int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath)
{
  return (at_ok(olddirfd, oldpath) && at_ok(newdirfd, newpath)) ? rename(oldpath, newpath) : -1;
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
