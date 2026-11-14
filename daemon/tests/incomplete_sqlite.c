/* SPDX-License-Identifier: GPL-2.0-only */
/* An intentionally incomplete library exercises all-or-nothing dlopen binding. */
typedef struct sqlite3 sqlite3;
int sqlite3_open_v2(const char *name, sqlite3 **db, int flags, const char *vfs);
int sqlite3_open_v2(const char *name, sqlite3 **db, int flags, const char *vfs)
{
	(void)name;
	(void)db;
	(void)flags;
	(void)vfs;
	return 1;
}
