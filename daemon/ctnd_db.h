/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CTND_DB_H
#define CTND_DB_H

#include "ctnd.h"
#include <limits.h>

/* Deliberately a tiny dynamic ABI: the production library remains device-owned. */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
struct ctnd_sqlite {
	void *handle;
	int (*open_v2)(const char *, sqlite3 **, int, const char *);
	int (*prepare_v2)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
	int (*bind_text)(sqlite3_stmt *, int, const char *, int, void (*)(void *));
	int (*step)(sqlite3_stmt *);
	const unsigned char *(*column_text)(sqlite3_stmt *, int);
	int (*column_bytes)(sqlite3_stmt *, int);
	int (*column_type)(sqlite3_stmt *, int);
	int (*finalize)(sqlite3_stmt *);
	int (*close)(sqlite3 *);
	int (*close_v2)(sqlite3 *);
	int (*errcode)(sqlite3 *);
	int (*busy_timeout)(sqlite3 *, int);
	int (*db_readonly)(sqlite3 *, const char *);
};

struct ctnd_file_fp {
	uint64_t device, inode, size;
	int64_t mtime_s, mtime_ns, ctime_s, ctime_ns;
	bool exists;
};

struct ctnd_db_fp {
	struct ctnd_file_fp db, wal;
	uint64_t dir_device, dir_inode;
	bool valid;
};

struct ctnd_cache_entry {
	char package[CTND_PKG_LEN];
	struct ctnd_cfg cfg;
	struct ctnd_db_fp fp;
	uint64_t ts;
	bool used;
};

#ifdef CTND_TEST
/* Hooks inject faults into the real DB/loader implementation, never copies. */
enum ctnd_db_hook {
	CTND_HOOK_OPEN, CTND_HOOK_PREPARE, CTND_HOOK_BIND,
	CTND_HOOK_STEP, CTND_HOOK_COLUMN, CTND_HOOK_FINALIZE, CTND_HOOK_CLOSE,
	CTND_HOOK_COPY_DB, CTND_HOOK_COPY_WAL, CTND_HOOK_AFTER_DB, CTND_HOOK_AFTER_COPY
};
#endif

struct ctnd_db {
	struct ctnd_sqlite sql;
	struct ctnd_cache_entry cache[CTND_CACHE_MAX];
	char runtime[PATH_MAX];
	char override[PATH_MAX];
	size_t replace;
	unsigned int queries;
#ifdef CTND_TEST
	void *hook_ctx;
	bool (*hook)(void *ctx, enum ctnd_db_hook phase);
#endif
};

bool ctnd_sqlite_load(struct ctnd_sqlite *sql, const char *const candidates[]);
void ctnd_sqlite_unload(struct ctnd_sqlite *sql);
bool ctnd_db_init(struct ctnd_db *db, const char *runtime, const char *override);
void ctnd_db_close(struct ctnd_db *db);
/* Call at startup while holding the shared runtime lock; unsafe entries stay intact. */
bool ctnd_db_reap_snapshots(int dirfd, int lockfd);
bool ctnd_db_fingerprint(const char *path, struct ctnd_db_fp *fp);
bool ctnd_db_fp_same(const struct ctnd_db_fp *a, const struct ctnd_db_fp *b);
enum ctnd_result ctnd_db_lookup(struct ctnd_db *db, const char *package, uint64_t now,
				struct ctnd_cfg *cfg);

#endif
