/* aura-rocks file I/O helpers, called through std/ffi c-func.
 *
 * Aura's file primitives are whole-file only: write-file opens with O_TRUNC,
 * and there is no append, rename or fsync (filed upstream, see README). A WAL
 * needs an O_APPEND write and an atomic MANIFEST swap needs rename(2), so
 * these three calls live here. lib/rio.aura falls back to read+rewrite when
 * this .so is missing (correct, but O(file) per append and not crash-atomic).
 *
 * Strings cross c-func as (pointer, length): Aura strings may hold NUL bytes. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

EXPORT const uint64_t aot_emit_version = 0;

/* Append n bytes of data to path (created 0644). sync != 0 -> fdatasync.
 * Returns bytes written, or -errno. */
EXPORT long rocks_append(const char *path, const char *data, long n, long sync) {
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (fd < 0) return -errno;
  long left = n;
  const char *p = data;
  while (left > 0) {
    ssize_t w = write(fd, p, (size_t)left);
    if (w < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      close(fd);
      return -e;
    }
    p += w;
    left -= w;
  }
  if (sync && fdatasync(fd) != 0) {
    int e = errno;
    close(fd);
    return -e;
  }
  close(fd);
  return n;
}

/* Atomic replace: rename(from, to). 0 or -errno. */
EXPORT long rocks_rename(const char *from, const char *to) {
  return rename(from, to) == 0 ? 0 : -errno;
}

/* unlink(path). 0 or -errno (missing file -> -ENOENT). */
EXPORT long rocks_unlink(const char *path) {
  return unlink(path) == 0 ? 0 : -errno;
}

/* fsync a directory or file by path (after rename / create). 0 or -errno. */
EXPORT long rocks_fsync_path(const char *path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -errno;
  int r = fsync(fd);
  int e = errno;
  close(fd);
  return r == 0 ? 0 : -e;
}
