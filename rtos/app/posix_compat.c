/*
 * POSIX *at() calls and flock() for the Rust std library on RTEMS, which has no *at system calls.
 * Paths relative to AT_FDCWD or absolute are served by the plain calls; a real directory file
 * descriptor is not supported (ENOSYS) - nexus2 does not rely on it. flock() always succeeds:
 * there is one program in the image, so advisory locks between processes cannot conflict.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <errno.h>
#include <fcntl.h>
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
