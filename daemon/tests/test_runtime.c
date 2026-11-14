/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "../ctnd_runtime.h"
#include "../ctnd_fs.h"
#include "test_helper.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void put(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	CHECK(fd >= 0);
	CHECK(ctnd_write_all(fd, text, strlen(text)));
	CHECK(close(fd) == 0);
}

int main(void)
{
	char root[] = "/tmp/ctnd-runtime-test-XXXXXX";
	char runtime[512], boot[512], path[512], linkpath[512], other[512];
	struct ctnd_runtime rt;
	struct ctnd_journal journal = {0}, loaded;
	struct stat st;
	pid_t child;
	int status, fd;
	char buf[64];
	size_t n;

	CHECK(mkdtemp(root) != NULL);
	CHECK(snprintf(runtime, sizeof(runtime), "%s/runtime", root) > 0);
	CHECK(snprintf(boot, sizeof(boot), "%s/boot", root) > 0);
	put(boot, "00112233-4455-6677-8899-aabbccddeeff\n");
	CHECK(ctnd_runtime_open(&rt, runtime, boot) == 0);
	CHECK(snprintf(path, sizeof(path), "%s/%s", runtime, CTND_PID_FILE) > 0);
	CHECK(ctnd_read_path(path, buf, sizeof(buf), &n));
	CHECK(strtol(buf, NULL, 10) == (long)getpid());
	CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
	CHECK(ctnd_runtime_load(&rt, &loaded));
	CHECK(!loaded.nodes[0].owed);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		struct ctnd_runtime duplicate;
		int result;

		close(rt.lockfd);
		close(rt.dirfd);
		if (chdir("/")) _exit(9);
		result = ctnd_runtime_open(&duplicate, runtime, boot);
		if (!result) ctnd_runtime_close(&duplicate);
		_exit(result);
	}
	CHECK(waitpid(child, &status, 0) == child);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == CTND_ALREADY_RUNNING);
	journal.nodes[0].owed = true;
	journal.nodes[0].applied_valid = true;
	memcpy(journal.nodes[0].original, "CustomOne CustomTwo", sizeof("CustomOne CustomTwo"));
	memcpy(journal.nodes[0].applied, "Game Render", sizeof("Game Render"));
	CHECK(ctnd_runtime_save(&rt, &journal));
	CHECK(snprintf(path, sizeof(path), "%s/%s", runtime, CTND_STATE_FILE) > 0);
	CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
	CHECK(ctnd_runtime_load(&rt, &loaded));
	CHECK(loaded.nodes[0].owed && !strcmp(loaded.nodes[0].original, "CustomOne CustomTwo"));
	ctnd_runtime_close(&rt);
	CHECK(ctnd_runtime_open(&rt, runtime, boot) == 0);
	CHECK(ctnd_runtime_load(&rt, &loaded) && loaded.nodes[0].owed);
	memset(&loaded, 0, sizeof(loaded));
	CHECK(ctnd_runtime_save(&rt, &loaded));
	CHECK(lstat(path, &st) != 0 && errno == ENOENT);
	CHECK(ctnd_runtime_save(&rt, &journal));
	ctnd_runtime_close(&rt);
	put(boot, "10112233-4455-6677-8899-aabbccddeeff\n");
	CHECK(ctnd_runtime_open(&rt, runtime, boot) == 0);
	CHECK(ctnd_runtime_load(&rt, &loaded) && !loaded.nodes[0].owed);
	CHECK(lstat(path, &st) != 0 && errno == ENOENT);
	CHECK(ctnd_runtime_save(&rt, &journal));
	fd = open(path, O_WRONLY | O_CLOEXEC);
	CHECK(fd >= 0 && write(fd, "BAD", 3) == 3 && close(fd) == 0);
	CHECK(!ctnd_runtime_load(&rt, &loaded));
	CHECK(unlink(path) == 0);
	CHECK(symlink(boot, path) == 0);
	CHECK(!ctnd_runtime_load(&rt, &loaded));
	CHECK(!ctnd_runtime_save(&rt, &journal));
	CHECK(unlink(path) == 0);
	CHECK(snprintf(other, sizeof(other), "%s/other", root) > 0);
	put(other, "untouched");
	CHECK(link(other, path) == 0);
	CHECK(!ctnd_runtime_save(&rt, &journal));
	CHECK(unlink(path) == 0);
	CHECK(snprintf(path, sizeof(path), "%s/%s", runtime, CTND_PID_FILE) > 0);
	ctnd_runtime_close(&rt);
	CHECK(lstat(path, &st) != 0 && errno == ENOENT);
	CHECK(symlink(other, path) == 0);
	CHECK(ctnd_runtime_open(&rt, runtime, boot) != 0);
	CHECK(unlink(path) == 0);
	CHECK(snprintf(path, sizeof(path), "%s/%s", runtime, CTND_DAEMON_LOCK) > 0);
	CHECK(unlink(path) == 0);
	CHECK(symlink(other, path) == 0);
	CHECK(ctnd_runtime_open(&rt, runtime, boot) != 0);
	CHECK(unlink(path) == 0);
	CHECK(snprintf(linkpath, sizeof(linkpath), "%s/alias", root) > 0);
	CHECK(symlink(runtime, linkpath) == 0);
	CHECK(ctnd_runtime_open(&rt, linkpath, boot) != 0);
	CHECK(unlink(linkpath) == 0);
	CHECK(chmod(runtime, 0777) == 0);
	CHECK(ctnd_runtime_open(&rt, runtime, boot) != 0);
	CHECK(chmod(runtime, 0700) == 0);
	CHECK(ctnd_read_path(other, buf, sizeof(buf), &n) && !strcmp(buf, "untouched"));
	CHECK(rmdir(runtime) == 0);
	CHECK(unlink(boot) == 0 && unlink(other) == 0 && rmdir(root) == 0);
	printf("PASS runtime: %u checks\n", test_count);
	return 0;
}
