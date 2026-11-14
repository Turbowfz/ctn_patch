/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "ctnd_runtime.h"
#include "ctnd_fs.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct disk_debt {
	unsigned char owed, applied_valid, pending_valid, reserved;
	char original[CTND_NAMES_LEN];
	char applied[CTND_NAMES_LEN];
	char pending[CTND_NAMES_LEN];
};

struct disk_state {
	char magic[8];
	char boot[CTND_BOOT_LEN];
	struct disk_debt nodes[CTND_NODE_COUNT];
	unsigned char checksum[4];
};

static uint32_t checksum(const void *ptr, size_t n)
{
	const unsigned char *p = ptr;
	uint32_t h = UINT32_C(2166136261);

	while (n--) { h ^= *p++; h *= UINT32_C(16777619); }
	return h;
}

static bool boot_id(const char *path, char *out)
{
	char buf[64];
	size_t n, i;

	if (!ctnd_read_path(path, buf, sizeof(buf), &n) || n < CTND_BOOT_LEN - 1) return false;
	for (i = 0; i < CTND_BOOT_LEN - 1; i++) {
		char c = buf[i];

		if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
		else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
			   (c >= 'A' && c <= 'F'))) return false;
	}
	while (n > CTND_BOOT_LEN - 1 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
	if (n != CTND_BOOT_LEN - 1) return false;
	memcpy(out, buf, n);
	out[n] = '\0';
	return true;
}

int ctnd_runtime_open(struct ctnd_runtime *rt, const char *directory, const char *boot_path)
{
	char pid[32];
	int n, rc = 1;
	struct stat st, path_st;

	memset(rt, 0, sizeof(*rt));
	rt->dirfd = rt->lockfd = -1;
	rt->dirfd = ctnd_open_dir(directory, true, true);
	if (rt->dirfd < 0) return 1;
	rt->lockfd = openat(rt->dirfd, CTND_DAEMON_LOCK,
			    O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
	if (rt->lockfd < 0 || !ctnd_regular(rt->lockfd, true)) goto fail;
	if (flock(rt->lockfd, LOCK_EX | LOCK_NB)) {
		if (errno == EWOULDBLOCK || errno == EAGAIN) rc = CTND_ALREADY_RUNNING;
		goto fail;
	}
	if (fstat(rt->lockfd, &st) ||
	    fstatat(rt->dirfd, CTND_DAEMON_LOCK, &path_st, AT_SYMLINK_NOFOLLOW) ||
	    st.st_dev != path_st.st_dev || st.st_ino != path_st.st_ino) goto fail;
	if (!boot_id(boot_path, rt->boot_id)) goto fail;
	n = snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
	if (n <= 0 || (size_t)n >= sizeof(pid) ||
	    !ctnd_atomic_file(rt->dirfd, CTND_PID_FILE, pid, (size_t)n)) goto fail;
	rt->pid_written = true;
	return 0;
fail:
	ctnd_runtime_close(rt);
	return rc;
}

static bool valid_value(enum ctnd_node node, const char *text)
{
	char normalized[CTND_NAMES_LEN];
	bool ok;

	if (!memchr(text, '\0', CTND_NAMES_LEN)) return false;
	if (node == CTND_NODE_NAMES)
		ok = ctnd_names(text, normalized, sizeof(normalized));
	else
		ok = ctnd_node_enable(text, normalized, sizeof(normalized));
	return ok && !strcmp(text, normalized);
}

bool ctnd_runtime_load(struct ctnd_runtime *rt, struct ctnd_journal *journal)
{
	struct disk_state disk;
	char buffer[sizeof(disk) + 1];
	size_t n, i;
	int fd;
	uint32_t h, stored = 0;
	bool ok;

	memset(journal, 0, sizeof(*journal));
	fd = openat(rt->dirfd, CTND_STATE_FILE, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return errno == ENOENT;
	ok = ctnd_regular(fd, true) && ctnd_read_all(fd, buffer, sizeof(buffer), &n);
	if (close(fd)) ok = false;
	if (!ok || n != sizeof(disk)) return false;
	memcpy(&disk, buffer, sizeof(disk));
	if (memcmp(disk.magic, "CTNDst01", sizeof(disk.magic)) ||
	    disk.boot[CTND_BOOT_LEN - 1] != '\0') return false;
	h = checksum(&disk, offsetof(struct disk_state, checksum));
	for (i = 0; i < 4; i++) stored |= (uint32_t)disk.checksum[i] << (i * 8);
	if (stored != h) return false;
	if (strcmp(disk.boot, rt->boot_id)) {
		/* A different kernel boot has no ownership relationship to this debt. */
		return ctnd_runtime_save(rt, journal);
	}
	for (i = 0; i < CTND_NODE_COUNT; i++) {
		const struct disk_debt *d = &disk.nodes[i];
		struct ctnd_debt *out = &journal->nodes[i];

		if (d->owed > 1 || d->applied_valid > 1 || d->pending_valid > 1 || d->reserved)
			return false;
		if (!d->owed) {
			if (d->applied_valid || d->pending_valid) return false;
			continue;
		}
		if (!valid_value((enum ctnd_node)i, d->original) ||
		    (!d->applied_valid && !d->pending_valid) ||
		    (d->applied_valid && !valid_value((enum ctnd_node)i, d->applied)) ||
		    (d->pending_valid && !valid_value((enum ctnd_node)i, d->pending))) return false;
		out->owed = true;
		out->applied_valid = d->applied_valid != 0;
		out->pending_valid = d->pending_valid != 0;
		memcpy(out->original, d->original, sizeof(out->original));
		if (out->applied_valid) memcpy(out->applied, d->applied, sizeof(out->applied));
		if (out->pending_valid) memcpy(out->pending, d->pending, sizeof(out->pending));
	}
	return true;
}

bool ctnd_runtime_save(struct ctnd_runtime *rt, const struct ctnd_journal *journal)
{
	struct disk_state disk = {0};
	uint32_t h;
	size_t i;
	bool owed = false;

	for (i = 0; i < CTND_NODE_COUNT; i++) owed |= journal->nodes[i].owed;
	if (!owed) {
		struct stat st;

		if (fstatat(rt->dirfd, CTND_STATE_FILE, &st, AT_SYMLINK_NOFOLLOW))
			return errno == ENOENT;
		if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 ||
		    (st.st_mode & 077)) return false;
		return unlinkat(rt->dirfd, CTND_STATE_FILE, 0) == 0 && fsync(rt->dirfd) == 0;
	}
	memcpy(disk.magic, "CTNDst01", sizeof(disk.magic));
	memcpy(disk.boot, rt->boot_id, sizeof(disk.boot));
	for (i = 0; i < CTND_NODE_COUNT; i++) {
		const struct ctnd_debt *d = &journal->nodes[i];
		struct disk_debt *out = &disk.nodes[i];

		if (!d->owed) continue;
		if (!valid_value((enum ctnd_node)i, d->original) ||
		    (!d->applied_valid && !d->pending_valid) ||
		    (d->applied_valid && !valid_value((enum ctnd_node)i, d->applied)) ||
		    (d->pending_valid && !valid_value((enum ctnd_node)i, d->pending))) return false;
		out->owed = 1;
		out->applied_valid = d->applied_valid ? 1 : 0;
		out->pending_valid = d->pending_valid ? 1 : 0;
		memcpy(out->original, d->original, strlen(d->original) + 1);
		if (d->applied_valid) memcpy(out->applied, d->applied, strlen(d->applied) + 1);
		if (d->pending_valid) memcpy(out->pending, d->pending, strlen(d->pending) + 1);
	}
	h = checksum(&disk, offsetof(struct disk_state, checksum));
	for (i = 0; i < 4; i++) disk.checksum[i] = (unsigned char)(h >> (i * 8));
	return ctnd_atomic_file(rt->dirfd, CTND_STATE_FILE, &disk, sizeof(disk));
}

void ctnd_runtime_close(struct ctnd_runtime *rt)
{
	if (rt->pid_written && rt->dirfd >= 0) {
		char actual[32], expected[32];
		size_t n;
		int fd = openat(rt->dirfd, CTND_PID_FILE, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);

		(void)snprintf(expected, sizeof(expected), "%ld\n", (long)getpid());
		if (fd >= 0) {
			if (ctnd_regular(fd, true) && ctnd_read_all(fd, actual, sizeof(actual), &n) &&
			    !strcmp(actual, expected)) (void)unlinkat(rt->dirfd, CTND_PID_FILE, 0);
			close(fd);
		}
	}
	if (rt->lockfd >= 0) close(rt->lockfd);
	if (rt->dirfd >= 0) close(rt->dirfd);
	rt->dirfd = rt->lockfd = -1;
	rt->pid_written = false;
}

static const char *node_path(enum ctnd_node node)
{
	switch (node) {
	case CTND_NODE_NAMES: return CTND_NAMES_PATH;
	case CTND_NODE_ENABLE: return CTND_ENABLE_PATH;
	case CTND_NODE_COUNT: break;
	}
	return NULL;
}

bool ctnd_runtime_read_node(enum ctnd_node node, char *out, size_t cap)
{
	char buf[512];
	size_t n;
	const char *path = node_path(node);

	if (!path || !ctnd_read_path(path, buf, sizeof(buf), &n) || !n || memchr(buf, '\0', n))
		return false;
	return node == CTND_NODE_NAMES ? ctnd_node_names(buf, out, cap) :
		ctnd_node_enable(buf, out, cap);
}

bool ctnd_runtime_write_node(enum ctnd_node node, const char *value)
{
	char leaf[NAME_MAX + 1];
	const char *path = node_path(node);
	int dirfd, fd;
	ssize_t n;
	size_t len;
	bool ok;

	if (!path) return false;
	len = strlen(value);
	dirfd = ctnd_open_parent(path, leaf, sizeof(leaf));
	if (dirfd < 0) return false;
	fd = openat(dirfd, leaf, O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	close(dirfd);
	if (fd < 0) return false;
	/* proc handlers accept one complete command at offset zero, not write loops. */
	do { n = write(fd, value, len); } while (n < 0 && errno == EINTR);
	ok = n >= 0 && (size_t)n == len;
	if (close(fd)) ok = false;
	return ok;
}

bool ctnd_runtime_package(int pid, char *out, size_t cap)
{
	char path[64], buf[4096];
	size_t n, len, i;
	char *end;

	if (pid <= 0 || !cap) return false;
	(void)snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	if (!ctnd_read_path(path, buf, sizeof(buf), &n) || !n) return false;
	end = memchr(buf, '\0', n);
	if (!end) return false;
	len = (size_t)(end - buf);
	if (!len || len >= cap) return false;
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)buf[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':')) return false;
	}
	memcpy(out, buf, len);
	out[len] = '\0';
	return true;
}

uint64_t ctnd_now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}
