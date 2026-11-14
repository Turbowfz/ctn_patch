/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CTND_FS_H
#define CTND_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>

/* Walk absolute paths with O_NOFOLLOW on every component. */
int ctnd_open_dir(const char *path, bool create_last, bool private_dir);
int ctnd_open_parent(const char *path, char *leaf, size_t cap);
bool ctnd_regular(int fd, bool private_file);
bool ctnd_daemon_lock_held(int dirfd, int lockfd);
bool ctnd_read_all(int fd, char *buf, size_t cap, size_t *len);
bool ctnd_write_all(int fd, const void *buf, size_t len);
bool ctnd_read_path(const char *path, char *buf, size_t cap, size_t *len);
bool ctnd_atomic_file(int dirfd, const char *name, const void *data, size_t len);

#endif
