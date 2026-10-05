// SPDX-License-Identifier: GPL-2.0-only
/*
 * ctnd —— critical task name daemon   v1.1
 * Copyright (c) Turbo
 *
 * 为什么需要它：
 *   ctn_patch 模块把 /proc/game_opt/task_boost/critical_task_name 补出来了，
 *   但**没有任何进程会去写它** —— 一加 6.1 的 gameopt HAL 走的是 sched_assist
 *   那条路（ioctl + pipeline_pids_cpus），从来没碰过内核的 critical_task[]。
 *   所以节点一直停在默认的 UnityMain / UnityGfxDevice，对非 Unity 游戏毫无作用。
 *
 * 判断链：
 *   1. 盯 /proc/game_opt/game_pid —— HAL 在游戏启动时写 pid、退出写 -1
 *   2. 游戏一起来，从 pid 取包名
 *   3. 按优先级取该游戏的关键线程名：
 *        a) 本地覆盖文件 /data/adb/ctn_patch/ctn.conf（手写兜底，优先级最高）
 *        b) COSA 云控库 /data/user/0/com.oplus.cosa/databases/db_game_database
 *           的 game_config 列，取里面的 "ctn"
 *   4. 写进 critical_task_name
 *
 * v1.1 相对 v1.0 的改动（性能 / 占用 / 兼容性）：
 *   [性能] 配置缓存 —— 同一个包的 ctn 只解析一次；库没变（比对 db+wal 的
 *          mtime/size/inode 指纹）就直接用缓存，不再重复拷贝 600KB 的库。
 *   [性能] 轮询 500ms → 800ms；且只在 game_pid 真的变化时才干活。
 *   [占用] 只拷 db + wal，不再拷 -shm：-shm 是正在被 COSA mmap 的共享内存，
 *          拷它既没必要（SQLite 会自己重建），还可能拷到撕裂内容。省 32KB 也更安全。
 *   [占用] 缓存上限 64 条，长期运行不会无限增长。
 *   [兼容] 配置形态：明文 JSON 直接取；base64 编码的 JSON 先解码再取；
 *          两者都不是（加密/未知形态）→ 明确告警 + 回退到本地覆盖文件，
 *          而不是悄悄写个默认值了事。
 *   [兼容] 本地覆盖文件 ctn.conf：云控没覆盖的游戏、或库里是加密内容读不出来时，
 *          手写一行就能用 —— 保证「注入」这条路始终走得通。
 *   [兼容] SQLite 库 / 云控库都支持多候选路径；`--db <路径>` 可手工指定库
 *          （其他机型库不在候选列表里时的出口）。
 *
 * 构建：见同目录 build.sh（NDK clang，aarch64-linux-android）
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------ 常量 ------------------------------ */

#define CTND_VERSION "1.1"

#define NODE_PATH    "/proc/game_opt/task_boost/critical_task_name"
#define CTEN_PATH    "/proc/game_opt/task_boost/ct_enable"
#define GAMEPID_PATH "/proc/game_opt/game_pid"

/* 本地覆盖文件：手写兜底，优先级高于云控库 */
#define LOCAL_CONF   "/data/adb/ctn_patch/ctn.conf"

/* 云控库候选路径 */
static const char *DB_CANDIDATES[] = {
	"/data/user/0/com.oplus.cosa/databases/db_game_database",
	"/data/data/com.oplus.cosa/databases/db_game_database",
	NULL,
};

#define DEFAULT_NAMES "UnityMain UnityGfxDevice"

/* SQLite 库候选路径（这台设备叫 libsqlite.so，不是 libsqlite3.so） */
static const char *SQLITE_CANDIDATES[] = {
	"/system/lib64/libsqlite.so",
	"/system/lib64/libsqlite3.so",
	"/apex/com.android.runtime/lib64/libsqlite3.so",
	"/apex/com.android.art/lib64/libsqlite3.so",
	NULL,
};

/* 退出码：3 = 已有另一个实例在跑。service.sh 的守护循环见到 3 就不再重启，
 * 否则重复的守护循环会每 5 秒起一次、每次都被锁挡回来，白刷日志。 */
#define EXIT_ALREADY_RUNNING 3

#define NAME_MAX_LEN 100          /* 与内核接口一致（官方 6.6 用 %99s） */
#define TMP_DIR      "/data/local/tmp/.ctnd"
#define POLL_MS      800
/* 连续读到多少次 -1 才认定游戏真的退出（挡掉加载期的抖动） */
#define GONE_POLLS   6            /* 6 × 800ms ≈ 5 秒 */

/* 配置缓存：同一个包只解析一次，库没变就一直复用 */
#define CACHE_MAX    64
#define CACHE_PKG    256          /* 和调用方的 pkg[256] 对齐，免得截断告警 */
#define CACHE_NAME   256

/* 配置形态 */
enum cfg_form {
	CFG_NONE = 0,		/* 库里没有这个包 */
	CFG_JSON,		/* 明文 JSON */
	CFG_B64JSON,		/* base64 编码的 JSON */
	CFG_EMPTY,		/* 是 JSON 但没有 ctn（Unity 游戏） */
	CFG_OPAQUE,		/* 加密 / 未知形态 */
};

struct cfg_entry {
	char pkg[CACHE_PKG];
	char ctn[CACHE_NAME];
	int  ctb;
	enum cfg_form form;
	unsigned long long dbver;	/* 读这条时的库指纹 */
	bool used;
};

static struct cfg_entry g_cache[CACHE_MAX];
static unsigned long long g_dbver;	/* 当前库指纹 */

/* ------------------------------ 日志 ------------------------------ */

/*
 * 只写 stderr：service.sh 用 >> daemon.log 2>&1 重定向，日志就落到模块目录。
 * 不做文件日志是刻意的 —— 少一处 fd、少一处出错可能。
 * format(printf,1,2) 让编译器去查每个调用点的格式串对不对。
 */
__attribute__((format(printf, 1, 2)))
static void logmsg(const char *fmt, ...)
{
	char ts[32];
	time_t now = time(NULL);
	struct tm tm;
	va_list ap;

	localtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &tm);

	fprintf(stderr, "[%s] ", ts);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);		/* 掉电/被杀时也别丢最后几行 */
}

/* --------------------------- SQLite 动态加载 --------------------------- */
/*
 * 用 dlopen 而不是链接：libsqlite.so 是平台私有库，NDK 里没有它的 stub，
 * 链接会失败；而且不同机型路径还不一样，dlopen 更好做回退。
 */

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

#define SQLITE_OK 0
#define SQLITE_ROW 100
#define SQLITE_OPEN_READONLY 0x00000001

static void *g_sqlite;
static int (*p_sqlite3_open_v2)(const char *, sqlite3 **, int, const char *);
static int (*p_sqlite3_prepare_v2)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
static int (*p_sqlite3_bind_text)(sqlite3_stmt *, int, const char *, int, void (*)(void *));
static int (*p_sqlite3_step)(sqlite3_stmt *);
static const unsigned char *(*p_sqlite3_column_text)(sqlite3_stmt *, int);
static int (*p_sqlite3_finalize)(sqlite3_stmt *);
static int (*p_sqlite3_close)(sqlite3 *);
static const char *(*p_sqlite3_errmsg)(sqlite3 *);

static bool load_sqlite(void)
{
	int i;

	if (g_sqlite)
		return true;

	for (i = 0; SQLITE_CANDIDATES[i]; i++) {
		void *h = dlopen(SQLITE_CANDIDATES[i], RTLD_NOW | RTLD_LOCAL);
		if (h) {
			logmsg("sqlite: 加载 %s 成功", SQLITE_CANDIDATES[i]);
			g_sqlite = h;
			break;
		}
	}
	if (!g_sqlite) {
		logmsg("sqlite: 所有候选路径都加载失败，最后错误: %s", dlerror());
		return false;
	}

#define BIND(sym)                                                     \
	do {                                                          \
		*(void **)(&p_##sym) = dlsym(g_sqlite, #sym);         \
		if (!p_##sym) {                                       \
			logmsg("sqlite: 缺少符号 %s", #sym);         \
			return false;                                 \
		}                                                     \
	} while (0)

	BIND(sqlite3_open_v2);
	BIND(sqlite3_prepare_v2);
	BIND(sqlite3_bind_text);
	BIND(sqlite3_step);
	BIND(sqlite3_column_text);
	BIND(sqlite3_finalize);
	BIND(sqlite3_close);
	BIND(sqlite3_errmsg);
#undef BIND

	return true;
}

/* --------------------------- 文件小工具 --------------------------- */

static bool read_file(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return false;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n <= 0)
		return false;
	buf[n] = '\0';
	return true;
}

static bool write_file(const char *path, const char *data)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return false;
	n = write(fd, data, strlen(data));
	close(fd);
	return n == (ssize_t)strlen(data);
}

static bool copy_file(const char *src, const char *dst)
{
	char buf[65536];
	int in, out;
	ssize_t n;

	in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return false;
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (out < 0) {
		close(in);
		return false;
	}
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, n) != n) {
			close(in);
			close(out);
			return false;
		}
	}
	close(in);
	close(out);
	return n >= 0;
}

/*
 * 库指纹：把 db 和 -wal 的 (mtime, size, inode) 揉成一个数。
 * 库没变就不用重拷、也不用重新解析 —— 同一个包反复启停时能省掉绝大多数拷贝。
 */
static unsigned long long db_fingerprint(const char *db)
{
	static const char *suffixes[] = { "", "-wal", NULL };
	unsigned long long fp = 1469598103934665603ULL;	/* FNV offset */
	int i;

	for (i = 0; suffixes[i]; i++) {
		char p[PATH_MAX];
		struct stat st;

		snprintf(p, sizeof(p), "%s%s", db, suffixes[i]);
		if (stat(p, &st) != 0)
			continue;
		fp = (fp ^ (unsigned long long)st.st_mtime) * 1099511628211ULL;
		fp = (fp ^ (unsigned long long)st.st_size)  * 1099511628211ULL;
		fp = (fp ^ (unsigned long long)st.st_ino)   * 1099511628211ULL;
	}
	return fp;
}

/* 找一个可读的云控库。--db 指定过就直接用它（其他机型库不在候选列表里时的出口）。 */
static const char *g_db_override;

static const char *find_db(void)
{
	int i;

	if (g_db_override) {
		if (access(g_db_override, R_OK) == 0)
			return g_db_override;
		logmsg("库: --db 指定的 %s 读不了，回退到候选路径", g_db_override);
	}

	for (i = 0; DB_CANDIDATES[i]; i++)
		if (access(DB_CANDIDATES[i], R_OK) == 0)
			return DB_CANDIDATES[i];
	return NULL;
}

/* --------------------------- base64 解码 --------------------------- */
/*
 * 云控下发的配置有可能是 base64 编码过的 JSON（"enc" 的一种常见形态）。
 * 自己实现一个解码器，省掉 libcrypto 依赖。
 */
static int b64val(int c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

/* 解码成功返回 true；out 保证 NUL 结尾。失败说明输入不是 base64。 */
static bool b64_decode(const char *in, char *out, size_t outlen)
{
	int quad[4];
	int n = 0;
	size_t o = 0;

	if (outlen < 2)
		return false;

	for (; *in; in++) {
		int v;

		if (isspace((unsigned char)*in))
			continue;
		if (*in == '=')
			break;		/* 填充：到此为止，残余统一在下面处理 */
		v = b64val((unsigned char)*in);
		if (v < 0)
			return false;	/* 混进了非 base64 字符 → 不是编码过的内容 */
		quad[n++] = v;
		if (n == 4) {
			if (o + 3 >= outlen)
				return false;	/* 解出来太长，肯定不是我们要的 */
			out[o++] = (char)((quad[0] << 2) | (quad[1] >> 4));
			out[o++] = (char)(((quad[1] & 0xF) << 4) | (quad[2] >> 2));
			out[o++] = (char)(((quad[2] & 0x3) << 6) | quad[3]);
			n = 0;
		}
	}

	/*
	 * 处理末尾不足一组的残余。这一步必须有 —— 无填充的 base64
	 * （比如 "aGVsbG8" 解 "hello"）不处理的话尾部会整段丢掉，
	 * 解出来的 JSON 被截断，反而更糟。单元测试抓过这个 bug。
	 */
	if (n == 3) {
		if (o + 2 >= outlen)
			return false;
		out[o++] = (char)((quad[0] << 2) | (quad[1] >> 4));
		out[o++] = (char)(((quad[1] & 0xF) << 4) | (quad[2] >> 2));
	} else if (n == 2) {
		if (o + 1 >= outlen)
			return false;
		out[o++] = (char)((quad[0] << 2) | (quad[1] >> 4));
	} else if (n == 1) {
		return false;		/* 单字符不可能是合法的 base64 结尾 */
	}

	if (o == 0)
		return false;
	out[o] = '\0';
	return true;
}

/* --------------------------- 极简 JSON 取值 --------------------------- */
/*
 * game_config 是扁平 JSON（所有键都在顶层），所以不需要完整解析器。
 * 注意：ctn 是字符串、ctb/htb/ctep 是数字，两种类型要分开取值 ——
 * 混用会取不到数字（v1.0 踩过）。
 */
static bool json_get_string(const char *json, const char *key, char *out, size_t outlen)
{
	char pat[64];
	const char *p, *q;
	size_t k = 0;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(json, pat);
	if (!p)
		return false;
	p += strlen(pat);

	while (*p && isspace((unsigned char)*p))
		p++;
	if (*p != ':')
		return false;
	p++;
	while (*p && isspace((unsigned char)*p))
		p++;
	if (*p != '"')
		return false;		/* 不是字符串（可能是 null / 数字 / 对象） */
	p++;

	q = p;
	while (*q && k + 1 < outlen) {
		if (*q == '\\' && q[1]) {
			q++;
			switch (*q) {
			case 'n': out[k++] = '\n'; break;
			case 't': out[k++] = '\t'; break;
			case 'r': out[k++] = '\r'; break;
			default:  out[k++] = *q;   break;
			}
			q++;
			continue;
		}
		if (*q == '"')
			break;
		out[k++] = *q++;
	}
	out[k] = '\0';
	return true;
}

static bool json_get_int(const char *json, const char *key, int *out)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(json, pat);
	if (!p)
		return false;
	p += strlen(pat);

	while (*p && isspace((unsigned char)*p))
		p++;
	if (*p != ':')
		return false;
	p++;
	while (*p && isspace((unsigned char)*p))
		p++;

	if (*p == '-' || (*p >= '0' && *p <= '9')) {
		*out = atoi(p);
		return true;
	}
	return false;
}

/* --------------------------- 本地覆盖文件 --------------------------- */
/*
 * 格式（一行一条，# 开头是注释）：
 *     com.tencent.tmgp.pubgmhd = RenderThread Thread-
 * 优先级高于云控库。用途：
 *   - 云控没覆盖的游戏
 *   - 库里存的是加密内容、daemon 读不出来时手工兜底
 */
static bool local_conf_lookup(const char *pkg, char *out, size_t outlen)
{
	char buf[16384];
	char *line, *save;

	if (access(LOCAL_CONF, R_OK) != 0)
		return false;
	if (!read_file(LOCAL_CONF, buf, sizeof(buf)))
		return false;

	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char *eq, *k, *v, *p;

		while (*line && isspace((unsigned char)*line))
			line++;
		p = line + strlen(line);
		while (p > line && isspace((unsigned char)p[-1]))
			*--p = '\0';
		if (!*line || *line == '#')
			continue;

		eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = '\0';
		k = line;
		v = eq + 1;
		while (*k && isspace((unsigned char)k[strlen(k) - 1]))
			k[strlen(k) - 1] = '\0';
		while (*v && isspace((unsigned char)*v))
			v++;
		if (strcmp(k, pkg) != 0)
			continue;

		snprintf(out, outlen, "%s", v);
		return true;
	}
	return false;
}

/* --------------------------- 缓存 --------------------------- */

static struct cfg_entry *cache_get(const char *pkg)
{
	int i, free_slot = -1;

	for (i = 0; i < CACHE_MAX; i++) {
		if (!g_cache[i].used) {
			if (free_slot < 0)
				free_slot = i;
			continue;
		}
		if (strcmp(g_cache[i].pkg, pkg) == 0)
			return &g_cache[i];
	}
	if (free_slot < 0)
		free_slot = 0;		/* 满了就覆盖第 0 条（简单轮转，够用） */
	memset(&g_cache[free_slot], 0, sizeof(g_cache[free_slot]));
	snprintf(g_cache[free_slot].pkg, CACHE_PKG, "%s", pkg);
	g_cache[free_slot].used = true;
	return &g_cache[free_slot];
}

/* --------------------------- 查库 --------------------------- */

/* 把云控库拷到临时目录再读。只拷 db + wal：-shm 是 COSA 正在 mmap 的
 * 共享内存，拷它既没必要（SQLite 会重建）也可能拷到撕裂内容。 */
static bool stage_db(const char *src, char *out_path, size_t outlen)
{
	static const char *suffixes[] = { "", "-wal", NULL };
	char d[PATH_MAX];
	int i;

	mkdir(TMP_DIR, 0700);
	for (i = 0; suffixes[i]; i++) {
		char s[PATH_MAX];

		snprintf(s, sizeof(s), "%s%s", src, suffixes[i]);
		snprintf(d, sizeof(d), "%s/db%s", TMP_DIR, suffixes[i]);
		unlink(d);
		if (!copy_file(s, d) && suffixes[i][0] == '\0') {
			logmsg("库: 拷贝 %s 失败: %s", s, strerror(errno));
			return false;
		}
	}
	/* 清掉可能残留的旧 -shm，让 SQLite 自己重建 */
	snprintf(d, sizeof(d), "%s/db-shm", TMP_DIR);
	unlink(d);

	snprintf(out_path, outlen, "%s/db", TMP_DIR);
	return true;
}

/*
 * 判定配置形态并取出 ctn/ctb。
 *
 * 支持两种形态：
 *   一、明文 JSON          —— 直接取 "ctn" / "ctb"
 *   二、base64 编码的 JSON —— 先解码，再在解码结果里取
 * 都不是（加密/未知形态）→ 标 CFG_OPAQUE，交给上层告警并回退，
 * 不硬猜、也不悄悄写个默认值了事。
 *
 * base64 里不含 '{'，所以拿 '{' 做判别很稳。
 */
static void parse_cfg(const char *raw, struct cfg_entry *e)
{
	char dec[CACHE_NAME * 8];
	const char *src = raw;

	e->form = CFG_OPAQUE;
	e->ctn[0] = '\0';
	e->ctb = -1;

	if (strchr(raw, '{')) {
		e->form = CFG_JSON;
	} else if (b64_decode(raw, dec, sizeof(dec))) {
		src = dec;
		e->form = CFG_B64JSON;
	} else {
		return;			/* 既不是 JSON 也不像 base64 → OPAQUE */
	}

	/* ctn 和 ctb 都必须从同一个 src 里取 —— v1.0 只在原始串上找 ctb，
	 * base64 形态就永远取不到（单元测试抓过）。 */
	if (json_get_string(src, "ctn", e->ctn, sizeof(e->ctn)))
		;			/* 有 ctn：保持 JSON / B64JSON */
	else
		e->form = CFG_EMPTY;	/* 是配置，但没有 ctn（Unity 游戏） */

	json_get_int(src, "ctb", &e->ctb);
}

/* 取一个包的关键线程名，结果写进 e。命中缓存就直接返回，不碰库。 */
static void lookup_cfg(const char *pkg, struct cfg_entry *e)
{
	const char *db = find_db();
	struct cfg_entry *c;
	char dbpath[PATH_MAX];
	sqlite3 *sq = NULL;
	sqlite3_stmt *st = NULL;
	const unsigned char *txt;
	unsigned long long fp;
	int rc;

	memset(e, 0, sizeof(*e));
	e->form = CFG_NONE;
	e->ctb = -1;

	if (!db) {
		logmsg("库: 找不到可读的云控库");
		return;
	}

	fp = db_fingerprint(db);
	if (fp != g_dbver) {
		g_dbver = fp;
		logmsg("库: 内容有变（指纹 %016llx），缓存失效", fp);
	}

	c = cache_get(pkg);
	if (c->form != CFG_NONE && c->dbver == g_dbver) {
		*e = *c;		/* 命中缓存，省掉一次拷贝 + 解析 */
		return;
	}

	if (!load_sqlite())
		return;
	if (!stage_db(db, dbpath, sizeof(dbpath)))
		return;

	rc = p_sqlite3_open_v2(dbpath, &sq, SQLITE_OPEN_READONLY, NULL);
	if (rc != SQLITE_OK) {
		logmsg("库: 打开失败 rc=%d", rc);
		if (sq) p_sqlite3_close(sq);
		return;
	}
	rc = p_sqlite3_prepare_v2(sq,
		"SELECT game_config FROM PackageConfigBean WHERE package_name = ?1",
		-1, &st, NULL);
	if (rc != SQLITE_OK) {
		logmsg("库: prepare 失败: %s", p_sqlite3_errmsg(sq));
		p_sqlite3_close(sq);
		return;
	}

	p_sqlite3_bind_text(st, 1, pkg, -1, NULL);
	rc = p_sqlite3_step(st);
	if (rc == SQLITE_ROW) {
		txt = p_sqlite3_column_text(st, 0);
		if (txt && *txt)
			parse_cfg((const char *)txt, e);
		else
			e->form = CFG_EMPTY;	/* 有行但 game_config 是空的 */
	} else {
		e->form = CFG_NONE;		/* 库里没这个包 */
	}

	p_sqlite3_finalize(st);
	p_sqlite3_close(sq);

	/* 写回缓存（连同这次的库指纹） */
	snprintf(c->pkg, CACHE_PKG, "%s", pkg);
	snprintf(c->ctn, CACHE_NAME, "%s", e->ctn);
	c->ctb = e->ctb;
	c->form = e->form;
	c->dbver = g_dbver;
	c->used = true;
}

/* --------------------------- 从 /proc 取包名 --------------------------- */

/*
 * 用 cmdline 取进程名。游戏主进程的 cmdline 一般就是包名；
 * 有些游戏主进程 cmdline 会带参数，所以只取第一段。
 * 若 cmdline 为空（内核线程/已退出），退回 comm。
 */
static bool get_package_name(pid_t pid, char *out, size_t outlen)
{
	char path[64];
	char buf[256];
	ssize_t n;
	size_t want;
	int fd;

	/* 只读目标缓冲装得下的量：多读没用，还会招来 -Wformat-truncation */
	want = outlen < sizeof(buf) ? outlen : sizeof(buf);
	if (want < 2)
		return false;

	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		n = read(fd, buf, want - 1);
		close(fd);
		if (n > 0) {
			size_t len;
			char *z;

			buf[n] = '\0';
			len = (size_t)n;
			z = memchr(buf, '\0', len);
			if (z)
				len = (size_t)(z - buf);	/* 第一个 NUL 之前就是进程名 */
			if (len > 0) {
				memcpy(out, buf, len);
				out[len] = '\0';
				return true;
			}
		}
	}

	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		n = read(fd, buf, want - 1);
		close(fd);
		if (n > 0) {
			buf[n] = '\0';
			buf[strcspn(buf, "\n")] = '\0';
			if (buf[0]) {
				memcpy(out, buf, strlen(buf) + 1);
				return true;
			}
		}
	}
	return false;
}

/* --------------------------- 写节点 --------------------------- */

/*
 * 节点要求"恰好两个名字"。这里做归一化：
 *   2 个   -> 原样
 *   1 个   -> 同名写两遍（内核里两个槽记同一线程同一 CPU，等价于只填一个）
 *   0 个或 >2 个 -> 取前两个 / 用默认值
 * 名字超过 15 字符内核永远匹配不到（task->comm 只有 16 字节含 NUL），只告警不拦。
 */
static bool normalize_and_write(const char *names)
{
	char buf[512];
	char *tok[8];
	int n = 0;
	char *p;
	char out[256];
	int i;

	snprintf(buf, sizeof(buf), "%s", names ? names : "");
	for (p = strtok(buf, " \t\r\n"); p && n < 8; p = strtok(NULL, " \t\r\n"))
		tok[n++] = p;

	if (n == 0) {
		snprintf(out, sizeof(out), "%s", DEFAULT_NAMES);
		logmsg("名字: 空，用默认值 [%s]", out);
	} else if (n == 1) {
		snprintf(out, sizeof(out), "%s %s", tok[0], tok[0]);
		logmsg("名字: 只有 1 个 [%s]，同名写两遍", tok[0]);
	} else {
		snprintf(out, sizeof(out), "%s %s", tok[0], tok[1]);
		if (n > 2)
			logmsg("名字: 有 %d 个，只取前两个", n);
	}

	/* 长度提醒要在 n==0 判断之后 —— 空串时 tok[0] 是未初始化的野指针 */
	for (i = 0; i < (n < 2 ? n : 2); i++)
		if (strlen(tok[i]) >= 16)
			logmsg("名字: 警告 [%s] 超过 15 字符，内核匹配不到"
			       "（task->comm 只有 16 字节）", tok[i]);

	if (!write_file(NODE_PATH, out)) {
		logmsg("写入失败 %s: %s（模块加载了吗？）", NODE_PATH, strerror(errno));
		return false;
	}
	logmsg("已写入 [%s]", out);
	return true;
}

/* --------------------------- 单实例锁 --------------------------- */

static int g_lock_fd = -1;

static bool acquire_single_instance(const char *exe)
{
	char path[PATH_MAX];
	char *slash;
	int fd;

	snprintf(path, sizeof(path), "%s", exe);
	slash = strrchr(path, '/');
	if (slash)
		slash[1] = 0;			/* 截到最后一个 / 之后 */
	else
		snprintf(path, sizeof(path), "./");
	strncat(path, ".ctnd.lock", sizeof(path) - strlen(path) - 1);

	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd < 0) {
		/* 锁文件建不了不算致命，照常跑，只是失去防重复能力 */
		logmsg("锁: 打不开 %s: %s（继续跑）", path, strerror(errno));
		return true;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		close(fd);
		logmsg("锁: 已有另一个 ctnd 在跑，本进程退出（%s）", path);
		return false;
	}
	g_lock_fd = fd;
	return true;
}

/* --------------------------- 主循环 --------------------------- */

/* 从 "game_pid=1234 child_num=0" 里取 pid */
static int parse_game_pid(const char *s)
{
	const char *p = strstr(s, "game_pid=");

	if (!p)
		return -1;
	return atoi(p + strlen("game_pid="));
}

static volatile sig_atomic_t g_stop;
static bool g_manage_cten;	/* --enable-ctb：是否连 ct_enable 一起管 */

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void handle_game_start(pid_t pid)
{
	struct cfg_entry e;
	char pkg[256] = {0};
	char local[NAME_MAX_LEN * 2] = {0};

	if (!get_package_name(pid, pkg, sizeof(pkg))) {
		logmsg("游戏 pid=%d，但取不到包名，跳过", (int)pid);
		return;
	}
	logmsg("游戏启动: pid=%d 包名=%s", (int)pid, pkg);

	/* ① 本地覆盖文件优先 */
	if (local_conf_lookup(pkg, local, sizeof(local))) {
		logmsg("本地覆盖: [%s]", local);
		normalize_and_write(local);
		return;
	}

	/* ② 云控库 */
	lookup_cfg(pkg, &e);

	switch (e.form) {
	case CFG_JSON:
		logmsg("云控 ctn = \"%s\"（明文 JSON，ctb=%d）", e.ctn, e.ctb);
		normalize_and_write(e.ctn);
		break;
	case CFG_B64JSON:
		logmsg("云控 ctn = \"%s\"（base64 编码的 JSON，ctb=%d）", e.ctn, e.ctb);
		normalize_and_write(e.ctn);
		break;
	case CFG_EMPTY:
		logmsg("云控里没有 ctn（Unity 游戏），写回默认值");
		normalize_and_write(DEFAULT_NAMES);
		break;
	case CFG_OPAQUE:
		/* 加密/未知形态：不硬猜，明确告诉用户怎么兜底 */
		logmsg("云控这条配置既不是 JSON 也不是 base64-JSON（加密或未知形态），"
		       "daemon 读不出来");
		logmsg("  → 想给 %s 指定名字，在 %s 里写一行：", pkg, LOCAL_CONF);
		logmsg("     %s = 名字1 名字2", pkg);
		logmsg("  本次先写回默认值");
		normalize_and_write(DEFAULT_NAMES);
		break;
	case CFG_NONE:
	default:
		logmsg("云控库里没有这个游戏，写回默认值");
		normalize_and_write(DEFAULT_NAMES);
		break;
	}

	/*
	 * ct_enable 归 HAL 管（它按 game_config.ctb + 游戏场景判定来开关），
	 * 默认我们不动它，免得两边互相打架。--enable-ctb 时才代管。
	 */
	if (g_manage_cten && e.ctb >= 0) {
		const char *want = e.ctb ? "1" : "0";
		if (write_file(CTEN_PATH, want))
			logmsg("ct_enable 置 %s（按配置 ctb=%d）", want, e.ctb);
		else
			logmsg("写 ct_enable 失败: %s", strerror(errno));
	}
}

int main(int argc, char **argv)
{
	char buf[256];
	pid_t last = -1;
	int gone = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-V") || !strcmp(argv[i], "--version")) {
			printf("ctnd %s\n", CTND_VERSION);
			return 0;
		}
		if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "--enable-ctb"))
			g_manage_cten = true;
		else if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--db")) {
			if (i + 1 >= argc) {
				fprintf(stderr, "!! --db 后面要跟库路径\n");
				return 2;
			}
			g_db_override = argv[++i];
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			printf("ctnd %s —— 按云控配置自动写 critical_task_name\n"
			       "用法: ctnd [-e] [-d 库路径] [-V]\n"
			       "  -e, --enable-ctb   连 ct_enable 一起按配置的 ctb 开关\n"
			       "                     （默认不管：HAL 自己会写，同时管可能互相打架）\n"
			       "  -d, --db <路径>    指定云控库（默认按内置候选路径找；\n"
			       "                     换机型/库不在候选列表时用这个）\n"
			       "  -V, --version      打印版本\n"
			       "本地覆盖文件（优先级高于云控库）：%s\n"
			       "  一行一条：包名 = 名字1 名字2   （# 开头是注释）\n"
			       "日志走 stderr，由 service.sh 重定向到 daemon.log\n",
			       CTND_VERSION, LOCAL_CONF);
			return 0;
		}
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	/* 父进程（service.sh）退出时不要把我们带走 */
	signal(SIGHUP, SIG_IGN);

	/* 只允许一个实例；重复启动直接退出，避免两个进程抢写同一个节点 */
	if (!acquire_single_instance(argv[0]))
		return EXIT_ALREADY_RUNNING;

	logmsg("ctnd %s 启动（节点 %s%s%s）", CTND_VERSION, NODE_PATH,
	       g_manage_cten ? "，代管 ct_enable" : "",
	       g_db_override ? "，库由 --db 指定" : "");

	/* 先确认节点在。模块没加载/加载失败时节点不存在，这时没必要每来一个
	 * 游戏就报一次写入失败；提一次就好，后面该写还是会试着写。 */
	if (access(NODE_PATH, W_OK) != 0)
		logmsg("注意: 现在写不了 %s（%s）—— ctn_patch 模块没加载？"
		       " 节点出现后本进程会照常工作", NODE_PATH, strerror(errno));

	if (access(LOCAL_CONF, R_OK) == 0)
		logmsg("本地覆盖文件已加载: %s", LOCAL_CONF);

	/* 启动时先对齐一次当前状态（比如 daemon 是被重启的，游戏已经在跑） */
	if (read_file(GAMEPID_PATH, buf, sizeof(buf))) {
		pid_t p = parse_game_pid(buf);
		if (p > 0) {
			last = p;
			handle_game_start(p);
		}
	} else {
		logmsg("读不到 %s（game_opt 起了吗？）", GAMEPID_PATH);
	}

	while (!g_stop) {
		pid_t p;

		usleep(POLL_MS * 1000);

		if (!read_file(GAMEPID_PATH, buf, sizeof(buf)))
			continue;

		p = parse_game_pid(buf);
		if (p == last)
			continue;

		if (p > 0) {
			last = p;
			gone = 0;
			handle_game_start(p);
			continue;
		}

		/*
		 * 读到 -1。实测游戏加载期 game_pid 会在正数和 -1 之间反复抖
		 * （HAL 自己也在反复判定，抖动能持续好几秒），所以要求连续
		 * GONE_POLLS 次 -1 才当真的退出，免得把刚写好的名字冲回默认值。
		 */
		if (last > 0 && ++gone >= GONE_POLLS) {
			logmsg("游戏退出（pid 连续 %d 次为 -1），恢复默认名字", GONE_POLLS);
			last = -1;
			gone = 0;
			normalize_and_write(DEFAULT_NAMES);
		}
	}

	logmsg("ctnd 退出");
	if (g_lock_fd >= 0)
		close(g_lock_fd);
	return 0;
}
