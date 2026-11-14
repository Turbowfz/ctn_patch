/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "../ctnd_db.h"
#include "../ctnd_fs.h"
#include "test_helper.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct writer_api {
	struct ctnd_sqlite sql;
	int (*exec)(sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *, char **);
};

static struct writer_api writer;

static void execute(sqlite3 *db, const char *sql)
{
	CHECK(writer.exec(db, sql, NULL, NULL, NULL) == 0);
}

static sqlite3 *create(const char *path, bool wal)
{
	sqlite3 *db = NULL;

	CHECK(writer.sql.open_v2(path, &db, 2 | 4, NULL) == 0 && db);
	execute(db, "CREATE TABLE PackageConfigBean(package_name TEXT,game_config TEXT)");
	execute(db, "INSERT INTO PackageConfigBean VALUES('com.a','{\"ctn\":\"A One\",\"ctb\":1}'),"
		"('com.b','{\"ctn\":\"B One\"}')");
	if (wal) {
		execute(db, "PRAGMA journal_mode=WAL");
		execute(db, "PRAGMA wal_autocheckpoint=0");
		execute(db, "UPDATE PackageConfigBean SET game_config='{\"ctn\":\"A Wal\"}' WHERE package_name='com.a'");
	}
	return db;
}

static void clear_cache(struct ctnd_db *db)
{
	memset(db->cache, 0, sizeof(db->cache));
}

static void clean_runtime(const char *runtime)
{
	DIR *dir = opendir(runtime);
	struct dirent *entry;

	CHECK(dir != NULL);
	while ((entry = readdir(dir)))
		CHECK(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."));
	CHECK(closedir(dir) == 0);
}

static int fd_count(void)
{
	DIR *dir = opendir("/proc/self/fd");
	struct dirent *entry;
	int count = 0;

	CHECK(dir != NULL);
	while ((entry = readdir(dir)))
		if (entry->d_name[0] != '.') count++;
	CHECK(closedir(dir) == 0);
	return count;
}

struct hook_state {
	enum ctnd_db_hook phase;
	sqlite3 *writer;
	const char *update;
	bool fail;
	bool called;
};

static bool hook(void *ctx, enum ctnd_db_hook phase)
{
	struct hook_state *h = ctx;

	if (phase != h->phase) return true;
	h->called = true;
	if (h->update) { execute(h->writer, h->update); h->update = NULL; }
	return !h->fail;
}

static void faults(struct ctnd_db *db, const char *runtime)
{
	static const enum ctnd_db_hook phases[] = {
		CTND_HOOK_OPEN, CTND_HOOK_PREPARE, CTND_HOOK_BIND, CTND_HOOK_STEP,
		CTND_HOOK_COLUMN, CTND_HOOK_FINALIZE, CTND_HOOK_CLOSE,
		CTND_HOOK_COPY_DB, CTND_HOOK_COPY_WAL
	};
	struct ctnd_cfg cfg;
	struct hook_state h = {0};
	size_t i;
	int before = fd_count();

	db->hook = hook;
	db->hook_ctx = &h;
	for (i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) {
		clear_cache(db);
		h.phase = phases[i]; h.fail = true; h.called = false;
		CHECK(ctnd_db_lookup(db, "com.a", 100, &cfg) == CTND_ERROR);
		CHECK(h.called && cfg.result == CTND_ERROR);
		clean_runtime(runtime);
		CHECK(fd_count() == before);
		h.fail = false;
		CHECK(ctnd_db_lookup(db, "com.a", 101, &cfg) == CTND_VALID);
		clean_runtime(runtime);
		CHECK(fd_count() == before);
	}
	db->hook = NULL;
	db->hook_ctx = NULL;
}

static void fingerprint_cases(struct ctnd_db *db, const char *path, const char *runtime)
{
	struct ctnd_db_fp first, changed;
	struct timespec times[2];
	struct stat st;
	char wal[PATH_MAX], old[PATH_MAX];
	int fd;
	struct ctnd_cfg cfg;
	sqlite3 *conn;

	CHECK(ctnd_db_fingerprint(path, &first));
	CHECK(first.valid && first.db.exists && !first.wal.exists);
	CHECK(stat(path, &st) == 0);
	times[0] = st.st_atim;
	times[1] = st.st_mtim;
	times[1].tv_nsec = times[1].tv_nsec == 999999999 ? 999999998 : times[1].tv_nsec + 1;
	CHECK(utimensat(AT_FDCWD, path, times, 0) == 0);
	CHECK(ctnd_db_fingerprint(path, &changed) && !ctnd_db_fp_same(&first, &changed));
	first = changed;
	CHECK(chmod(path, 0400) == 0);
	CHECK(ctnd_db_fingerprint(path, &changed) && !ctnd_db_fp_same(&first, &changed));
	CHECK(chmod(path, 0600) == 0);
	CHECK(snprintf(wal, sizeof(wal), "%s-wal", path) > 0);
	CHECK(ctnd_db_fingerprint(path, &first));
	fd = open(wal, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
	CHECK(fd >= 0 && close(fd) == 0);
	CHECK(ctnd_db_fingerprint(path, &changed) && changed.wal.exists && changed.wal.size == 0 &&
	      !ctnd_db_fp_same(&first, &changed));
	CHECK(unlink(wal) == 0);
	CHECK(ctnd_db_fingerprint(path, &changed) && ctnd_db_fp_same(&first, &changed));

	CHECK(snprintf(old, sizeof(old), "%s.old", path) > 0);
	CHECK(rename(path, old) == 0);
	conn = create(path, false);
	execute(conn, "UPDATE PackageConfigBean SET game_config='{\"ctn\":\"New Inode\"}' WHERE package_name='com.a'");
	CHECK(writer.sql.close(conn) == 0);
	CHECK(ctnd_db_fingerprint(path, &changed) && !ctnd_db_fp_same(&first, &changed));
	CHECK(ctnd_db_lookup(db, "com.a", 100, &cfg) == CTND_VALID && !strcmp(cfg.names, "New Inode"));
	CHECK(unlink(old) == 0);
	clean_runtime(runtime);
}

static void put_at(int dirfd, const char *name)
{
	int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);

	CHECK(fd >= 0 && ctnd_write_all(fd, "untouched", 9) && close(fd) == 0);
}

static bool exists_at(int dirfd, const char *name)
{
	struct stat st;

	return fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) == 0;
}

struct crash_hook {
	enum ctnd_db_hook phase;
	int ready;
};

static bool crash_hook(void *ctx, enum ctnd_db_hook phase)
{
	struct crash_hook *h = ctx;

	if (phase != h->phase) return true;
	if (write(h->ready, "!", 1) != 1) _exit(9);
	for (;;) pause();
}

static void snapshot_recovery(struct ctnd_db *db, const char *runtime, const char *path)
{
	static const enum ctnd_db_hook phases[] = {CTND_HOOK_AFTER_COPY, CTND_HOOK_STEP};
	int parent = ctnd_open_dir(runtime, false, true), lockfd, fd, status, pipefd[2];
	struct stat st, lock_before, lock_after;
	size_t i;
	pid_t child;
	DIR *dir;
	struct dirent *entry;
	char ready;
	unsigned int snapshots;

	CHECK(parent >= 0);
	lockfd = openat(parent, CTND_DAEMON_LOCK, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	CHECK(lockfd >= 0 && fstat(lockfd, &lock_before) == 0);
	for (i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) {
		CHECK(pipe(pipefd) == 0);
		child = fork();
		CHECK(child >= 0);
		if (!child) {
			struct ctnd_db crashed;
			struct ctnd_cfg cfg;
			struct crash_hook h = {phases[i], pipefd[1]};
			int owned;

			close(pipefd[0]);
			close(lockfd);
			owned = openat(parent, CTND_DAEMON_LOCK, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
			if (owned < 0 || flock(owned, LOCK_EX | LOCK_NB) ||
			    !ctnd_db_init(&crashed, runtime, path)) _exit(8);
			crashed.hook = crash_hook; crashed.hook_ctx = &h;
			(void)ctnd_db_lookup(&crashed, "com.a", 0, &cfg);
			_exit(7);
		}
		CHECK(close(pipefd[1]) == 0);
		CHECK(read(pipefd[0], &ready, 1) == 1 && ready == '!');
		CHECK(close(pipefd[0]) == 0);
		CHECK(!ctnd_db_reap_snapshots(parent, lockfd)); /* Another process owns EX. */
		CHECK(kill(child, SIGKILL) == 0 && waitpid(child, &status, 0) == child);
		CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
		dir = opendir(runtime);
		CHECK(dir != NULL);
		snapshots = 0;
		while ((entry = readdir(dir))) {
			if (strncmp(entry->d_name, ".snapshot-", 10)) continue;
			snapshots++;
			fd = openat(parent, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.st_nlink == 2 &&
			      (st.st_mode & 07777) == 0700 && st.st_uid == geteuid());
			CHECK(exists_at(fd, "db") && exists_at(fd, "db-wal"));
			if (phases[i] == CTND_HOOK_STEP) CHECK(exists_at(fd, "db-shm"));
			CHECK(close(fd) == 0);
		}
		CHECK(closedir(dir) == 0 && snapshots == 1);
		CHECK(!ctnd_db_reap_snapshots(parent, lockfd)); /* Unlocked is not ownership. */
		CHECK(flock(lockfd, LOCK_EX | LOCK_NB) == 0);
		CHECK(ctnd_db_reap_snapshots(parent, lockfd));
		CHECK(ctnd_db_reap_snapshots(parent, lockfd)); /* Idempotent, independent scan offset. */
		CHECK(flock(lockfd, LOCK_UN) == 0);
	}
	CHECK(flock(lockfd, LOCK_EX | LOCK_NB) == 0);
	CHECK(!ctnd_db_reap_snapshots(parent, -1));
	put_at(parent, "restore.state");
	put_at(parent, "daemon.pid");
	put_at(parent, "db");
	CHECK(mkdirat(parent, "other-project", 0700) == 0);
	CHECK(mkdirat(parent, ".snapshot-not-ours", 0700) == 0);
	CHECK(mkdirat(parent, ".snapshot-Ab0001", 0700) == 0);
	fd = openat(parent, ".snapshot-Ab0001", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	CHECK(fd >= 0);
	put_at(fd, "db");
	put_at(fd, "unknown");
	CHECK(!ctnd_db_reap_snapshots(parent, lockfd));
	CHECK(exists_at(fd, "db") && exists_at(fd, "unknown"));
	CHECK(unlinkat(fd, "unknown", 0) == 0);
	CHECK(fchmod(fd, 0755) == 0);
	CHECK(!ctnd_db_reap_snapshots(parent, lockfd) && exists_at(fd, "db"));
	CHECK(fchmod(fd, 0700) == 0);
	CHECK(fchmodat(fd, "db", 0644, 0) == 0);
	CHECK(!ctnd_db_reap_snapshots(parent, lockfd) && exists_at(fd, "db"));
	CHECK(fchmodat(fd, "db", 0600, 0) == 0);
	if (geteuid() == 0) {
		CHECK(fchown(fd, 12345, (gid_t)-1) == 0);
		CHECK(!ctnd_db_reap_snapshots(parent, lockfd) && exists_at(fd, "db"));
		CHECK(fchown(fd, geteuid(), (gid_t)-1) == 0);
		CHECK(fchownat(fd, "db", 12345, (gid_t)-1, AT_SYMLINK_NOFOLLOW) == 0);
		CHECK(!ctnd_db_reap_snapshots(parent, lockfd) && exists_at(fd, "db"));
		CHECK(fchownat(fd, "db", geteuid(), (gid_t)-1, AT_SYMLINK_NOFOLLOW) == 0);
	}
	CHECK(symlinkat(path, fd, "db-wal") == 0);
	CHECK(!ctnd_db_reap_snapshots(parent, lockfd) && exists_at(fd, "db") && exists_at(fd, "db-wal"));
	CHECK(unlinkat(fd, "db-wal", 0) == 0);
	CHECK(linkat(AT_FDCWD, path, fd, "db-wal", 0) == 0);
	CHECK(!ctnd_db_reap_snapshots(parent, lockfd) && exists_at(fd, "db"));
	CHECK(unlinkat(fd, "db-wal", 0) == 0);
	CHECK(mkfifoat(fd, "db-shm", 0600) == 0);
	CHECK(!ctnd_db_reap_snapshots(parent, lockfd) && exists_at(fd, "db"));
	CHECK(unlinkat(fd, "db-shm", 0) == 0);
	CHECK(mkdirat(fd, "unknown", 0700) == 0);
	CHECK(fstat(fd, &st) == 0 && st.st_nlink == 3);
	CHECK(!ctnd_db_reap_snapshots(parent, lockfd) && exists_at(fd, "db"));
	CHECK(unlinkat(fd, "unknown", AT_REMOVEDIR) == 0);
	CHECK(ctnd_db_reap_snapshots(parent, lockfd));
	CHECK(!exists_at(parent, ".snapshot-Ab0001") && close(fd) == 0);
	CHECK(symlinkat(path, parent, ".snapshot-Ab0002") == 0);
	CHECK(!ctnd_db_reap_snapshots(parent, lockfd) && exists_at(parent, ".snapshot-Ab0002"));
	CHECK(unlinkat(parent, ".snapshot-Ab0002", 0) == 0);
	CHECK(mkdirat(parent, ".snapshot-Ab0003", 0700) == 0);
	CHECK(ctnd_db_reap_snapshots(parent, lockfd) && !exists_at(parent, ".snapshot-Ab0003"));
	CHECK(exists_at(parent, "restore.state") && exists_at(parent, "daemon.pid") &&
	      exists_at(parent, "db") && exists_at(parent, "other-project") &&
	      exists_at(parent, ".snapshot-not-ours"));
	CHECK(fstat(lockfd, &lock_after) == 0 && lock_before.st_ino == lock_after.st_ino);
	CHECK(renameat(parent, CTND_DAEMON_LOCK, parent, ".old-lock") == 0);
	CHECK(symlinkat(path, parent, CTND_DAEMON_LOCK) == 0);
	CHECK(!ctnd_db_reap_snapshots(parent, lockfd));
	CHECK(unlinkat(parent, CTND_DAEMON_LOCK, 0) == 0);
	put_at(parent, CTND_DAEMON_LOCK);
	CHECK(!ctnd_db_reap_snapshots(parent, lockfd));
	CHECK(unlinkat(parent, CTND_DAEMON_LOCK, 0) == 0);
	CHECK(renameat(parent, ".old-lock", parent, CTND_DAEMON_LOCK) == 0);
	CHECK(fchmod(parent, 0755) == 0 && !ctnd_db_reap_snapshots(parent, lockfd));
	CHECK(fchmod(parent, 0700) == 0);
	CHECK(unlinkat(parent, "restore.state", 0) == 0 && unlinkat(parent, "daemon.pid", 0) == 0 &&
	      unlinkat(parent, "db", 0) == 0 && unlinkat(parent, "other-project", AT_REMOVEDIR) == 0 &&
	      unlinkat(parent, ".snapshot-not-ours", AT_REMOVEDIR) == 0);
	CHECK(flock(lockfd, LOCK_UN) == 0 && close(lockfd) == 0);
	CHECK(unlinkat(parent, CTND_DAEMON_LOCK, 0) == 0 && close(parent) == 0);
	clear_cache(db);
	clean_runtime(runtime);
}

int main(int argc, char **argv)
{
	char root[] = "/tmp/ctnd-db-test-XXXXXX";
	char path[PATH_MAX], runtime[PATH_MAX], wal[PATH_MAX], other[PATH_MAX], alias[PATH_MAX];
	struct ctnd_db db;
	struct ctnd_cfg cfg;
	struct ctnd_db_fp before, after;
	sqlite3 *conn;
	const char *const real_libraries[] = {"libsqlite3.so.0", "libsqlite3.so", NULL};
	const char *bad_libraries[2], *fallback[4];
	struct ctnd_sqlite loader = {0};
	void *symbol;
	unsigned int q;
	struct hook_state h;
	int fd;

	CHECK(argc == 2);
	bad_libraries[0] = argv[1]; bad_libraries[1] = NULL;
	CHECK(!ctnd_sqlite_load(&loader, bad_libraries));
	CHECK(!loader.handle && !loader.open_v2 && !loader.step);
	CHECK(!ctnd_sqlite_load(&loader, bad_libraries) && !loader.handle);
	fallback[0] = argv[1]; fallback[1] = "libsqlite3.so.0"; fallback[2] = "libsqlite3.so"; fallback[3] = NULL;
	CHECK(ctnd_sqlite_load(&loader, fallback) && loader.handle && loader.step);
	ctnd_sqlite_unload(&loader);
	CHECK(ctnd_sqlite_load(&writer.sql, real_libraries));
	symbol = dlsym(writer.sql.handle, "sqlite3_exec");
	CHECK(symbol != NULL);
	memcpy(&writer.exec, &symbol, sizeof(symbol));
	CHECK(mkdtemp(root) != NULL);
	CHECK(snprintf(runtime, sizeof(runtime), "%s/runtime", root) > 0 && mkdir(runtime, 0700) == 0);
	CHECK(snprintf(path, sizeof(path), "%s/game.db", root) > 0);
	conn = create(path, true);
	CHECK(ctnd_db_init(&db, runtime, path));
	CHECK(ctnd_db_fingerprint(path, &before));
	CHECK(ctnd_db_lookup(&db, "com.a", 0, &cfg) == CTND_VALID && !strcmp(cfg.names, "A Wal"));
	CHECK(ctnd_db_fingerprint(path, &after) && ctnd_db_fp_same(&before, &after));
	CHECK(db.queries == 1 && db.cache[0].fp.valid);
	CHECK(ctnd_db_lookup(&db, "com.a", 1, &cfg) == CTND_VALID && db.queries == 1);
	CHECK(ctnd_db_lookup(&db, "com.a", CTND_CACHE_TTL_MS, &cfg) == CTND_VALID && db.queries == 2);
	CHECK(ctnd_db_lookup(&db, "com.a", CTND_CACHE_TTL_MS + 1, &cfg) == CTND_VALID && db.queries == 2);
	CHECK(ctnd_db_lookup(&db, "com.b", 100, &cfg) == CTND_VALID && !strcmp(cfg.names, "B One"));
	execute(conn, "UPDATE PackageConfigBean SET game_config='{\"ctn\":\"B Two\"}' WHERE package_name='com.b'");
	CHECK(ctnd_db_lookup(&db, "com.a", CTND_CACHE_TTL_MS + 2, &cfg) == CTND_VALID);
	CHECK(ctnd_db_lookup(&db, "com.b", 101, &cfg) == CTND_VALID && !strcmp(cfg.names, "B Two"));
	CHECK(ctnd_db_lookup(&db, "missing", 102, &cfg) == CTND_MISSING);
	execute(conn, "INSERT INTO PackageConfigBean VALUES('empty','{}'),('bad','{\"ctn\":null}'),"
		"('base64','eyJjdG4iOiJTb2xvIn0='),('null',NULL),('blob',x'7b7d')");
	CHECK(ctnd_db_lookup(&db, "empty", 102, &cfg) == CTND_EMPTY);
	CHECK(ctnd_db_lookup(&db, "bad", 102, &cfg) == CTND_UNSUPPORTED);
	CHECK(ctnd_db_lookup(&db, "null", 102, &cfg) == CTND_UNSUPPORTED);
	CHECK(ctnd_db_lookup(&db, "blob", 102, &cfg) == CTND_UNSUPPORTED);
	CHECK(ctnd_db_lookup(&db, "base64", 102, &cfg) == CTND_VALID && !strcmp(cfg.names, "Solo"));
	execute(conn, "INSERT INTO PackageConfigBean VALUES('duplicate','{}'),('duplicate','{}')");
	CHECK(ctnd_db_lookup(&db, "duplicate", 102, &cfg) == CTND_ERROR);
	clean_runtime(runtime);
	faults(&db, runtime);
	snapshot_recovery(&db, runtime, path);
	clear_cache(&db);
	memset(&h, 0, sizeof(h));
	h.phase = CTND_HOOK_AFTER_DB; h.writer = conn;
	h.update = "UPDATE PackageConfigBean SET game_config='{\"ctn\":\"Concurrent Update\"}' WHERE package_name='com.a'";
	db.hook = hook; db.hook_ctx = &h;
	q = db.queries;
	CHECK(ctnd_db_lookup(&db, "com.a", 500, &cfg) == CTND_ERROR && h.called && db.queries == q);
	clean_runtime(runtime);
	db.hook = NULL;
	CHECK(ctnd_db_lookup(&db, "com.a", 501, &cfg) == CTND_VALID && !strcmp(cfg.names, "Concurrent Update"));
	clear_cache(&db);
	h.phase = CTND_HOOK_AFTER_COPY; h.called = false;
	h.update = "PRAGMA wal_checkpoint(TRUNCATE)";
	db.hook = hook;
	CHECK(ctnd_db_lookup(&db, "com.a", 502, &cfg) == CTND_ERROR && h.called);
	db.hook = NULL;
	CHECK(ctnd_db_lookup(&db, "com.a", 503, &cfg) == CTND_VALID);
	CHECK(writer.sql.close(conn) == 0);
	fingerprint_cases(&db, path, runtime);
	CHECK(snprintf(wal, sizeof(wal), "%s-wal", path) > 0);
	CHECK(symlink(path, wal) == 0);
	CHECK(ctnd_db_lookup(&db, "com.a", 600, &cfg) == CTND_ERROR);
	CHECK(unlink(wal) == 0);
	CHECK(snprintf(other, sizeof(other), "%s/other", root) > 0);
	CHECK(rename(path, other) == 0 && symlink(other, path) == 0);
	CHECK(ctnd_db_lookup(&db, "com.a", 600, &cfg) == CTND_ERROR);
	CHECK(unlink(path) == 0 && rename(other, path) == 0);
	CHECK(link(path, other) == 0);
	CHECK(ctnd_db_lookup(&db, "com.a", 600, &cfg) == CTND_ERROR);
	CHECK(unlink(other) == 0);
	CHECK(snprintf(alias, sizeof(alias), "%s/alias", root) > 0 && symlink(root, alias) == 0);
	CHECK(snprintf(other, sizeof(other), "%s/game.db", alias) > 0);
	ctnd_db_close(&db);
	CHECK(ctnd_db_init(&db, runtime, other));
	CHECK(ctnd_db_lookup(&db, "com.a", 600, &cfg) == CTND_ERROR);
	CHECK(unlink(alias) == 0);
	CHECK(ctnd_db_init(&db, runtime, path));
	CHECK(snprintf(other, sizeof(other), "%s-journal", path) > 0);
	fd = open(other, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
	CHECK(fd >= 0 && close(fd) == 0);
	CHECK(ctnd_db_lookup(&db, "com.a", 600, &cfg) == CTND_ERROR);
	CHECK(unlink(other) == 0);
	CHECK(unlink(path) == 0);
	CHECK(ctnd_db_lookup(&db, "com.a", 600, &cfg) == CTND_ERROR);
	fd = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
	CHECK(fd >= 0 && ctnd_write_all(fd, "not a sqlite database", 21) && close(fd) == 0);
	CHECK(ctnd_db_lookup(&db, "com.a", 600, &cfg) == CTND_ERROR);
	clean_runtime(runtime);
	CHECK(unlink(path) == 0);
	CHECK(writer.sql.open_v2(path, &conn, 2 | 4, NULL) == 0);
	execute(conn, "CREATE TABLE unrelated(x)");
	CHECK(writer.sql.close(conn) == 0);
	CHECK(ctnd_db_lookup(&db, "com.a", 600, &cfg) == CTND_ERROR);
	clean_runtime(runtime);
	ctnd_db_close(&db);
	ctnd_sqlite_unload(&writer.sql);
	CHECK(unlink(path) == 0 && rmdir(runtime) == 0 && rmdir(root) == 0);
	printf("PASS SQLite integration: %u checks\n", test_count);
	return 0;
}
