/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "ctnd_db.h"
#include "ctnd_fs.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SQL_OK 0
#define SQL_ROW 100
#define SQL_DONE 101
#define SQL_TEXT 3
#define SQL_NULL 5
#define SQL_READONLY 1
#define SNAPSHOT_LIMIT (64U * 1024U * 1024U)

static const char *const libraries[] = {
#ifdef __ANDROID__
	"/system/lib64/libsqlite.so",
	"/system/lib64/libsqlite3.so",
	"/apex/com.android.runtime/lib64/libsqlite3.so",
	"/apex/com.android.art/lib64/libsqlite3.so",
#else
	"libsqlite3.so.0", "libsqlite3.so",
#endif
	NULL
};

bool ctnd_sqlite_load(struct ctnd_sqlite *sql, const char *const candidates[])
{
	size_t i;

	if (sql->handle) return true;
	memset(sql, 0, sizeof(*sql));
	for (i = 0; candidates[i]; i++) {
		struct ctnd_sqlite bound = {0};
		bool ok = true;

		bound.handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
		if (!bound.handle) continue;
#define BIND(member) do { \
	void *symbol = dlsym(bound.handle, "sqlite3_" #member); \
	_Static_assert(sizeof(bound.member) == sizeof(symbol), "POSIX dlsym ABI"); \
	if (!symbol) ok = false; \
	memcpy(&bound.member, &symbol, sizeof(symbol)); \
} while (0)
		BIND(open_v2);
		BIND(prepare_v2);
		BIND(bind_text);
		BIND(step);
		BIND(column_text);
		BIND(column_bytes);
		BIND(column_type);
		BIND(finalize);
		BIND(close);
		BIND(close_v2);
		BIND(errcode);
		BIND(busy_timeout);
		BIND(db_readonly);
#undef BIND
		if (ok) { *sql = bound; return true; }
		dlclose(bound.handle);
	}
	return false;
}

void ctnd_sqlite_unload(struct ctnd_sqlite *sql)
{
	if (sql->handle) dlclose(sql->handle);
	memset(sql, 0, sizeof(*sql));
}

struct source {
	int dirfd, dbfd, walfd;
	char leaf[NAME_MAX + 1];
	char wal[NAME_MAX + 1];
	char journal[NAME_MAX + 1];
	struct ctnd_db_fp fp;
};

static void source_close(struct source *s)
{
	if (s->walfd >= 0) close(s->walfd);
	if (s->dbfd >= 0) close(s->dbfd);
	if (s->dirfd >= 0) close(s->dirfd);
	s->dirfd = s->dbfd = s->walfd = -1;
}

static void stat_fp(const struct stat *st, struct ctnd_file_fp *fp)
{
	memset(fp, 0, sizeof(*fp));
	fp->device = (uint64_t)st->st_dev;
	fp->inode = (uint64_t)st->st_ino;
	fp->size = (uint64_t)st->st_size;
	fp->mtime_s = (int64_t)st->st_mtim.tv_sec;
	fp->mtime_ns = (int64_t)st->st_mtim.tv_nsec;
	fp->ctime_s = (int64_t)st->st_ctim.tv_sec;
	fp->ctime_ns = (int64_t)st->st_ctim.tv_nsec;
	fp->exists = true;
}

static bool file_same(const struct ctnd_file_fp *a, const struct ctnd_file_fp *b)
{
	return a->exists == b->exists && (!a->exists ||
		(a->device == b->device && a->inode == b->inode && a->size == b->size &&
		 a->mtime_s == b->mtime_s && a->mtime_ns == b->mtime_ns &&
		 a->ctime_s == b->ctime_s && a->ctime_ns == b->ctime_ns));
}

bool ctnd_db_fp_same(const struct ctnd_db_fp *a, const struct ctnd_db_fp *b)
{
	return a->valid && b->valid && a->dir_device == b->dir_device &&
	       a->dir_inode == b->dir_inode && file_same(&a->db, &b->db) && file_same(&a->wal, &b->wal);
}

static bool safe_stat(int fd, struct ctnd_file_fp *fp)
{
	struct stat st;

	if (!ctnd_regular(fd, false) || fstat(fd, &st) || st.st_size < 0 ||
	    (uint64_t)st.st_size > SNAPSHOT_LIMIT) return false;
	stat_fp(&st, fp);
	return true;
}

static bool no_journal(const struct source *s)
{
	struct stat st;

	/* A rollback-journal DB may need recovery: copying only main would be stale. */
	if (fstatat(s->dirfd, s->journal, &st, AT_SYMLINK_NOFOLLOW) == 0) {
		errno = EBUSY;
		return false;
	}
	return errno == ENOENT;
}

static bool source_open(const char *path, struct source *s)
{
	struct stat st;
	size_t len;

	memset(s, 0, sizeof(*s));
	s->dirfd = s->dbfd = s->walfd = -1;
	s->dirfd = ctnd_open_parent(path, s->leaf, sizeof(s->leaf));
	if (s->dirfd < 0) return false;
	len = strlen(s->leaf);
	if (len + sizeof("-journal") > sizeof(s->journal)) goto fail;
	memcpy(s->wal, s->leaf, len);
	memcpy(s->wal + len, "-wal", sizeof("-wal"));
	memcpy(s->journal, s->leaf, len);
	memcpy(s->journal + len, "-journal", sizeof("-journal"));
	if (!no_journal(s)) goto fail;
	s->dbfd = openat(s->dirfd, s->leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (s->dbfd < 0 || !safe_stat(s->dbfd, &s->fp.db) || fstat(s->dirfd, &st)) goto fail;
	s->fp.dir_device = (uint64_t)st.st_dev;
	s->fp.dir_inode = (uint64_t)st.st_ino;
	s->walfd = openat(s->dirfd, s->wal, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (s->walfd < 0) {
		if (errno != ENOENT) goto fail;
	} else if (!safe_stat(s->walfd, &s->fp.wal)) goto fail;
	s->fp.valid = true;
	return true;
fail:
	source_close(s);
	return false;
}

static bool check_file(const struct source *s, int fd, const char *name,
		       const struct ctnd_file_fp *expected)
{
	struct stat st;
	struct ctnd_file_fp now;

	if (!expected->exists)
		return fstatat(s->dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
	if (fd < 0 || !safe_stat(fd, &now) || !file_same(expected, &now)) return false;
	if (fstatat(s->dirfd, name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) ||
	    st.st_nlink != 1) return false;
	stat_fp(&st, &now);
	return file_same(expected, &now);
}

static bool source_same(const char *path, const struct source *s)
{
	char leaf[NAME_MAX + 1];
	struct stat st;
	int fd;
	bool ok;

	if (!no_journal(s) || !check_file(s, s->dbfd, s->leaf, &s->fp.db) ||
	    !check_file(s, s->walfd, s->wal, &s->fp.wal)) return false;
	fd = ctnd_open_parent(path, leaf, sizeof(leaf));
	if (fd < 0) return false;
	ok = fstat(fd, &st) == 0 && (uint64_t)st.st_dev == s->fp.dir_device &&
	     (uint64_t)st.st_ino == s->fp.dir_inode && !strcmp(leaf, s->leaf);
	close(fd);
	return ok;
}

bool ctnd_db_fingerprint(const char *path, struct ctnd_db_fp *fp)
{
	struct source s;
	bool ok;

	memset(fp, 0, sizeof(*fp));
	if (!source_open(path, &s)) return false;
	ok = source_same(path, &s);
	if (ok) *fp = s.fp;
	source_close(&s);
	return ok;
}

static bool snapshot_name(const char *name)
{
	size_t i;

	if (strlen(name) != sizeof(".snapshot-") - 1 + 6 ||
	    strncmp(name, ".snapshot-", sizeof(".snapshot-") - 1)) return false;
	for (i = sizeof(".snapshot-") - 1; name[i]; i++) {
		char c = name[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9'))) return false;
	}
	return true;
}

static bool snapshot_dir_safe(const struct stat *st)
{
	/* Empty private directories have two links (self and parent's entry), not one. */
	return S_ISDIR(st->st_mode) && st->st_uid == geteuid() &&
	       (st->st_mode & 07777) == 0700 && st->st_nlink == 2;
}

static bool snapshot_file_safe(const struct stat *st)
{
	return S_ISREG(st->st_mode) && st->st_uid == geteuid() &&
	       (st->st_mode & 07777) == 0600 && st->st_nlink == 1;
}

static bool reap_snapshot(int parent, const char *name)
{
	static const char *const files[] = {"db", "db-wal", "db-shm"};
	struct stat directory, linked, expected[3];
	bool present[3] = {false}, ok = true;
	struct dirent *entry;
	DIR *dir;
	int fd;
	size_t i;

	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0) return false;
	if (fstat(fd, &directory) || !snapshot_dir_safe(&directory)) {
		close(fd);
		return false;
	}
	dir = fdopendir(fd);
	if (!dir) { close(fd); return false; }
	/* Preflight the WHOLE directory before unlinking even a recognized file. */
	for (;;) {
		errno = 0;
		entry = readdir(dir);
		if (!entry) { if (errno) ok = false; break; }
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
		for (i = 0; i < 3; i++) if (!strcmp(entry->d_name, files[i])) break;
		if (i == 3 || fstatat(fd, entry->d_name, &expected[i], AT_SYMLINK_NOFOLLOW) ||
		    !snapshot_file_safe(&expected[i])) { ok = false; break; }
		present[i] = true;
	}
	if (ok && (fstatat(parent, name, &linked, AT_SYMLINK_NOFOLLOW) ||
	    !snapshot_dir_safe(&linked) || directory.st_dev != linked.st_dev ||
	    directory.st_ino != linked.st_ino)) ok = false;
	for (i = 0; ok && i < 3; i++) {
		if (!present[i]) {
			if (!fstatat(fd, files[i], &linked, AT_SYMLINK_NOFOLLOW) || errno != ENOENT) ok = false;
		} else if (fstatat(fd, files[i], &linked, AT_SYMLINK_NOFOLLOW) ||
			   !snapshot_file_safe(&linked) || expected[i].st_dev != linked.st_dev ||
			   expected[i].st_ino != linked.st_ino) ok = false;
	}
	for (i = 0; ok && i < 3; i++)
		if (present[i] && unlinkat(fd, files[i], 0)) ok = false;
	if (ok && (fstatat(parent, name, &linked, AT_SYMLINK_NOFOLLOW) ||
	    !snapshot_dir_safe(&linked) || directory.st_dev != linked.st_dev ||
	    directory.st_ino != linked.st_ino || unlinkat(parent, name, AT_REMOVEDIR))) ok = false;
	if (closedir(dir)) ok = false;
	return ok;
}

bool ctnd_db_reap_snapshots(int dirfd, int lockfd)
{
	struct dirent *entry;
	DIR *dir;
	int scan;
	bool ok = true;

	if (!ctnd_daemon_lock_held(dirfd, lockfd)) return false;
	/* A new description keeps the caller's pinned runtime directory offset intact. */
	scan = openat(dirfd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (scan < 0) return false;
	dir = fdopendir(scan);
	if (!dir) { close(scan); return false; }
	for (;;) {
		errno = 0;
		entry = readdir(dir);
		if (!entry) { if (errno) ok = false; break; }
		if (!snapshot_name(entry->d_name)) continue;
		if (!reap_snapshot(dirfd, entry->d_name)) ok = false;
	}
	if (closedir(dir)) ok = false;
	return ok;
}

struct snapshot {
	int parent, fd;
	char directory[PATH_MAX];
	char path[PATH_MAX];
	char leaf[80];
};

static bool snapshot_close(struct snapshot *snap)
{
	static const char *const files[] = {"db", "db-wal", "db-shm", "db-journal"};
	size_t i;
	bool ok = true;

	if (snap->fd >= 0) {
		for (i = 0; i < sizeof(files) / sizeof(files[0]); i++)
			if (unlinkat(snap->fd, files[i], 0) && errno != ENOENT) ok = false;
		if (close(snap->fd)) ok = false;
		snap->fd = -1;
	}
	if (snap->parent >= 0) {
		if (snap->leaf[0] && unlinkat(snap->parent, snap->leaf, AT_REMOVEDIR) && errno != ENOENT)
			ok = false;
		if (close(snap->parent)) ok = false;
		snap->parent = -1;
	}
	return ok;
}

static bool snapshot_open(const char *runtime, struct snapshot *snap)
{
	char pattern[PATH_MAX];
	char *leaf;
	struct stat st, linked;
	int n;

	memset(snap, 0, sizeof(*snap));
	snap->parent = snap->fd = -1;
	snap->parent = ctnd_open_dir(runtime, false, true);
	if (snap->parent < 0) return false;
	/* Resolve creation through the pinned, private directory rather than cwd. */
	n = snprintf(pattern, sizeof(pattern), "/proc/self/fd/%d/.snapshot-XXXXXX", snap->parent);
	if (n <= 0 || (size_t)n >= sizeof(pattern) || !mkdtemp(pattern)) goto fail;
	leaf = strrchr(pattern, '/');
	if (!leaf || strlen(leaf + 1) >= sizeof(snap->leaf)) goto fail;
	memcpy(snap->leaf, leaf + 1, strlen(leaf + 1) + 1);
	snap->fd = openat(snap->parent, snap->leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (snap->fd < 0 || fstat(snap->fd, &st) || st.st_uid != geteuid() || (st.st_mode & 077)) goto fail;
	n = snprintf(snap->directory, sizeof(snap->directory), "%s/%s", runtime, snap->leaf);
	if (n <= 0 || (size_t)n >= sizeof(snap->directory) || lstat(snap->directory, &linked) ||
	    linked.st_dev != st.st_dev || linked.st_ino != st.st_ino) goto fail;
	n = snprintf(snap->path, sizeof(snap->path), "%s/db", snap->directory);
	if (n <= 0 || (size_t)n >= sizeof(snap->path)) goto fail;
	return true;
fail:
	(void)snapshot_close(snap);
	return false;
}

static bool copy_fd(int in, int dirfd, const char *name)
{
	char buf[32768];
	int out;
	bool ok = true;
	uint64_t total = 0;

	out = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (out < 0) return false;
	for (;;) {
		ssize_t n = read(in, buf, sizeof(buf));

		if (n < 0 && errno == EINTR) continue;
		if (n < 0) { ok = false; break; }
		if (!n) break;
		total += (uint64_t)n;
		if (total > SNAPSHOT_LIMIT || !ctnd_write_all(out, buf, (size_t)n)) { ok = false; break; }
	}
	if (close(out)) ok = false;
	return ok;
}

#ifdef CTND_TEST
#define HOOK(db, phase) (!(db)->hook || (db)->hook((db)->hook_ctx, (phase)))
#else
#define HOOK(db, phase) (true)
#endif

static enum ctnd_result query(struct ctnd_db *db, const char *path, const char *package,
			       struct ctnd_cfg *cfg)
{
	struct ctnd_sqlite *sql = &db->sql;
	sqlite3 *conn = NULL;
	sqlite3_stmt *stmt = NULL;
	enum ctnd_result result = CTND_ERROR;
	int rc;

	if (!ctnd_sqlite_load(sql, libraries) || !HOOK(db, CTND_HOOK_OPEN)) return CTND_ERROR;
	db->queries++;
	rc = sql->open_v2(path, &conn, SQL_READONLY, NULL);
	if (rc != SQL_OK || !conn) goto done;
	if (sql->db_readonly(conn, "main") != 1 || sql->busy_timeout(conn, 100) != SQL_OK ||
	    !HOOK(db, CTND_HOOK_PREPARE)) goto done;
	rc = sql->prepare_v2(conn, "SELECT game_config FROM PackageConfigBean WHERE package_name=?1",
			     -1, &stmt, NULL);
	if (rc != SQL_OK || !stmt || !HOOK(db, CTND_HOOK_BIND)) goto done;
	if (sql->bind_text(stmt, 1, package, -1, NULL) != SQL_OK || !HOOK(db, CTND_HOOK_STEP)) goto done;
	rc = sql->step(stmt);
	if (rc == SQL_DONE) result = CTND_MISSING;
	else if (rc == SQL_ROW) {
		const unsigned char *raw;
		int type = sql->column_type(stmt, 0), len;

		if (!HOOK(db, CTND_HOOK_COLUMN)) goto done;
		if (type == SQL_NULL) result = CTND_UNSUPPORTED;
		else if (type != SQL_TEXT) result = CTND_UNSUPPORTED;
		else {
			int code;

			raw = sql->column_text(stmt, 0);
			len = sql->column_bytes(stmt, 0);
			code = sql->errcode(conn);
			if (!raw || len < 0 || (code != SQL_OK && code != SQL_ROW)) goto done;
			result = ctnd_parse_cfg((const char *)raw, (size_t)len, cfg);
		}
		/* Duplicate package rows are ambiguous; errors here are not deletion. */
		if (sql->step(stmt) != SQL_DONE) result = CTND_ERROR;
	}
	done:
	if (stmt) {
		rc = sql->finalize(stmt);
		stmt = NULL;
		if (rc != SQL_OK || !HOOK(db, CTND_HOOK_FINALIZE)) result = CTND_ERROR;
	}
	if (conn) {
		rc = sql->close(conn);
		if (rc != SQL_OK) { (void)sql->close_v2(conn); result = CTND_ERROR; }
		if (!HOOK(db, CTND_HOOK_CLOSE)) result = CTND_ERROR;
	}
	if (result != CTND_VALID && result != CTND_EMPTY) memset(cfg, 0, sizeof(*cfg));
	cfg->result = result;
	return result;
}

bool ctnd_db_init(struct ctnd_db *db, const char *runtime, const char *override)
{
	memset(db, 0, sizeof(*db));
	if (!runtime || runtime[0] != '/' || strlen(runtime) >= sizeof(db->runtime) ||
	    (override && (override[0] != '/' || strlen(override) >= sizeof(db->override)))) return false;
	memcpy(db->runtime, runtime, strlen(runtime) + 1);
	if (override) memcpy(db->override, override, strlen(override) + 1);
	return true;
}

void ctnd_db_close(struct ctnd_db *db)
{
	ctnd_sqlite_unload(&db->sql);
}

static const char *db_path(const struct ctnd_db *db, char *fallback, size_t cap)
{
	static const char *const roots[] = {"/data/user/0", "/data/data"};
	static const char suffix[] = "/com.oplus.cosa/databases/db_game_database";
	static const char primary[] = "/data/user/0/com.oplus.cosa/databases/db_game_database";
	const char *selected = db->override[0] ? db->override : primary;
	char resolved[PATH_MAX];
	size_t i;

	/* Only the two platform user-0 aliases may be resolved. App-owned parents,
	 * the database and its WAL must all pass the component-wise nofollow walk. */
	for (i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
		size_t n = strlen(roots[i]);
		const char *rest;
		int size;

		if (db->override[0] && (strncmp(selected, roots[i], n) || selected[n] != '/')) continue;
		rest = db->override[0] ? selected + n : suffix;
		if (!realpath(roots[i], resolved) ||
		    (strcmp(resolved, roots[0]) && strcmp(resolved, roots[1]))) continue;
		size = snprintf(fallback, cap, "%s%s", resolved, rest);
		if (size > 0 && (size_t)size < cap) return fallback;
	}
	return selected;
}

enum ctnd_result ctnd_db_lookup(struct ctnd_db *db, const char *package, uint64_t now,
				struct ctnd_cfg *cfg)
{
	struct source source;
	struct snapshot snap;
	struct ctnd_cache_entry *cached = NULL;
	struct ctnd_cfg found = {0};
	char fallback[PATH_MAX];
	const char *path;
	enum ctnd_result result = CTND_ERROR;
	size_t i;
	bool ok;

	memset(cfg, 0, sizeof(*cfg));
	cfg->result = CTND_ERROR;
	if (!package || !*package || strlen(package) >= CTND_PKG_LEN) return CTND_ERROR;
	path = db_path(db, fallback, sizeof(fallback));
	if (!source_open(path, &source)) return CTND_ERROR;
	for (i = 0; i < CTND_CACHE_MAX; i++)
		if (db->cache[i].used && !strcmp(db->cache[i].package, package)) { cached = &db->cache[i]; break; }
	/* Fingerprint is initialized on EVERY path, including first query and TTL expiry. */
	if (cached && now >= cached->ts && now - cached->ts < CTND_CACHE_TTL_MS &&
	    ctnd_db_fp_same(&cached->fp, &source.fp) && source_same(path, &source)) {
		*cfg = cached->cfg;
		source_close(&source);
		return cfg->result;
	}
	if (!snapshot_open(db->runtime, &snap)) { source_close(&source); return CTND_ERROR; }
	ok = HOOK(db, CTND_HOOK_COPY_DB) && copy_fd(source.dbfd, snap.fd, "db") &&
	     HOOK(db, CTND_HOOK_AFTER_DB);
	if (ok && source.fp.wal.exists)
		ok = HOOK(db, CTND_HOOK_COPY_WAL) && copy_fd(source.walfd, snap.fd, "db-wal");
	ok = ok && HOOK(db, CTND_HOOK_AFTER_COPY) && source_same(path, &source);
	if (ok) result = query(db, snap.path, package, &found);
	if (result != CTND_ERROR && !source_same(path, &source)) result = CTND_ERROR;
	if (!snapshot_close(&snap)) result = CTND_ERROR;
	if (result != CTND_ERROR) {
		if (!cached) {
			for (i = 0; i < CTND_CACHE_MAX; i++) if (!db->cache[i].used) { cached = &db->cache[i]; break; }
			if (!cached) { cached = &db->cache[db->replace++ % CTND_CACHE_MAX]; }
		}
		memset(cached, 0, sizeof(*cached));
		memcpy(cached->package, package, strlen(package) + 1);
		found.result = result;
		cached->cfg = found;
		cached->fp = source.fp;
		cached->ts = now;
		cached->used = true;
		*cfg = found;
	}
	source_close(&source);
	return result;
}
