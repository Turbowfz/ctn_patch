/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "ctnd_fs.h"
#include "ctnd_paths.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

int ctnd_open_dir(const char *path, bool create_last, bool private_dir)
{
	char part[NAME_MAX + 1];
	const char *p;
	int fd;
	struct stat st;

	if (!path || path[0] != '/' || strlen(path) >= PATH_MAX) { errno = EINVAL; return -1; }
	fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) return -1;
	p = path + 1;
	while (*p) {
		const char *end;
		size_t n;
		int next;

		while (*p == '/') p++;
		if (!*p) break;
		end = strchr(p, '/');
		n = end ? (size_t)(end - p) : strlen(p);
		if (!n || n > NAME_MAX || (n == 1 && p[0] == '.') ||
		    (n == 2 && p[0] == '.' && p[1] == '.')) { errno = EINVAL; goto fail; }
		memcpy(part, p, n);
		part[n] = '\0';
		if (create_last && (!end || !end[1]) && mkdirat(fd, part, 0700) && errno != EEXIST)
			goto fail;
		next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (next < 0) goto fail;
		close(fd);
		fd = next;
		p += n;
	}
	if (fstat(fd, &st) || !S_ISDIR(st.st_mode)) goto fail;
	if (private_dir && (st.st_uid != geteuid() || (st.st_mode & 077))) {
		errno = EPERM;
		goto fail;
	}
	return fd;
fail:
	{
		int saved = errno;
		close(fd);
		errno = saved;
	}
	return -1;
}

int ctnd_open_parent(const char *path, char *leaf, size_t cap)
{
	char parent[PATH_MAX];
	const char *slash;
	size_t n;

	if (!path || path[0] != '/' || strlen(path) >= PATH_MAX) { errno = EINVAL; return -1; }
	slash = strrchr(path, '/');
	if (!slash || !slash[1] || strlen(slash + 1) >= cap || !strcmp(slash + 1, ".") ||
	    !strcmp(slash + 1, "..")) { errno = EINVAL; return -1; }
	memcpy(leaf, slash + 1, strlen(slash + 1) + 1);
	n = (size_t)(slash - path);
	if (!n) n = 1;
	memcpy(parent, path, n);
	parent[n] = '\0';
	return ctnd_open_dir(parent, false, false);
}

bool ctnd_regular(int fd, bool private_file)
{
	struct stat st;

	if (fstat(fd, &st)) return false;
	if (!S_ISREG(st.st_mode) || st.st_nlink != 1 ||
	    (private_file && (st.st_uid != geteuid() || (st.st_mode & 077)))) {
		errno = EPERM;
		return false;
	}
	return true;
}

bool ctnd_daemon_lock_held(int dirfd, int lockfd)
{
	struct stat dir, held, linked, checked;
	int probe, saved;
	bool blocked;

	if (fstat(dirfd, &dir) || !S_ISDIR(dir.st_mode) || dir.st_uid != geteuid() ||
	    (dir.st_mode & 07777) != 0700 || !ctnd_regular(lockfd, true) ||
	    fstat(lockfd, &held) ||
	    fstatat(dirfd, CTND_DAEMON_LOCK, &linked, AT_SYMLINK_NOFOLLOW) ||
	    held.st_dev != linked.st_dev || held.st_ino != linked.st_ino) return false;
	/* A separate open must be blocked, and our supplied description must own EX.
	 * This rejects both an unlocked fd and a lock held by a different process. */
	probe = openat(dirfd, CTND_DAEMON_LOCK, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (probe < 0) return false;
	if (!ctnd_regular(probe, true) || fstat(probe, &checked) ||
	    held.st_dev != checked.st_dev || held.st_ino != checked.st_ino) {
		close(probe);
		return false;
	}
	blocked = flock(probe, LOCK_EX | LOCK_NB) != 0;
	saved = errno;
	if (!blocked) (void)flock(probe, LOCK_UN);
	if (close(probe) || !blocked || (saved != EWOULDBLOCK && saved != EAGAIN) ||
	    flock(lockfd, LOCK_EX | LOCK_NB)) return false;
	return fstatat(dirfd, CTND_DAEMON_LOCK, &linked, AT_SYMLINK_NOFOLLOW) == 0 &&
	       held.st_dev == linked.st_dev && held.st_ino == linked.st_ino;
}

bool ctnd_read_all(int fd, char *buf, size_t cap, size_t *len)
{
	size_t n = 0;

	if (!cap) { errno = EINVAL; return false; }
	while (n < cap - 1) {
		ssize_t r = read(fd, buf + n, cap - 1 - n);

		if (r < 0 && errno == EINTR) continue;
		if (r < 0) return false;
		if (!r) { buf[n] = '\0'; *len = n; return true; }
		n += (size_t)r;
	}
	{
		char extra;
		ssize_t r;

		do { r = read(fd, &extra, 1); } while (r < 0 && errno == EINTR);
		if (r != 0) { if (r > 0) errno = EOVERFLOW; return false; }
	}
	buf[n] = '\0';
	*len = n;
	return true;
}

bool ctnd_write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;

	while (len) {
		ssize_t n = write(fd, p, len);

		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) { if (!n) errno = EIO; return false; }
		p += n;
		len -= (size_t)n;
	}
	return true;
}

bool ctnd_read_path(const char *path, char *buf, size_t cap, size_t *len)
{
	char leaf[NAME_MAX + 1];
	int dirfd, fd;
	bool ok;

	dirfd = ctnd_open_parent(path, leaf, sizeof(leaf));
	if (dirfd < 0) return false;
	fd = openat(dirfd, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	close(dirfd);
	if (fd < 0) return false;
	ok = ctnd_read_all(fd, buf, cap, len);
	if (close(fd)) ok = false;
	return ok;
}

bool ctnd_atomic_file(int dirfd, const char *name, const void *data, size_t len)
{
	char temp[80];
	unsigned long nonce;
	static unsigned long serial;
	int fd = -1, i;
	bool ok = false;
	struct stat st;

	/* The parent is private and held by fd; never truncate an existing inode. */
	if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
		if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
		    (st.st_mode & 077)) { errno = EPERM; return false; }
	} else if (errno != ENOENT) return false;
	for (i = 0; i < 16; i++) {
		nonce = ++serial;
		(void)snprintf(temp, sizeof(temp), ".write-%ld-%lu", (long)getpid(), nonce);
		fd = openat(dirfd, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (fd >= 0 || errno != EEXIST) break;
	}
	if (fd < 0) return false;
	ok = ctnd_write_all(fd, data, len) && fsync(fd) == 0;
	if (close(fd)) ok = false;
	if (ok && renameat(dirfd, temp, dirfd, name) == 0) {
		/* A failed directory fsync is still a failure: no node write may follow. */
		return fsync(dirfd) == 0;
	}
	(void)unlinkat(dirfd, temp, 0);
	return false;
}
