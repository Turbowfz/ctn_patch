/*
 * ctnd —— critical task name daemon   v2.0
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
 *   3. 去 COSA 云控库 /data/user/0/com.oplus.cosa/databases/db_game_database
 *      的 game_config 列取该包的 "ctn"
 *   4. **只有拿到 ctn 才写节点**；写前记住节点原值，游戏退出后恢复回去
 *
 * v2.0 的改动（按反馈重写，核心是「不必要时一个字都不写」）：
 *   [移除] 本地覆盖文件 ctn.conf —— 一并去掉 ctn.conf.example 与开机铺开逻辑。
 *   [功耗] **没有 ctn 就完全不碰节点**。旧版在「云控里没 ctn / 库里没这个游戏 /
 *          配置是加密」这三种情况下会写回 UnityMain UnityGfxDevice —— 虽然值和
 *          内核默认相同，但等于「每启动一个游戏都去动内核」。现在这些情况一律
 *          只记日志、不写节点，daemon 对内核**零影响**。
 *   [安全] 退出时恢复的是**启动时读到的原值**（不是硬编码的 Unity 串），
 *          避免把用户/别的工具设过的值覆盖掉。
 *   [排查] 关键事件同时写 /dev/kmsg —— dmesg 里能看到 daemon 什么时候写了什么，
 *          和内核日志同一条时间线（排查功耗/行为问题时对着 dmesg 看就够）。
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

#define CTND_VERSION "2.2"

#define NODE_PATH    "/proc/game_opt/task_boost/critical_task_name"
#define CTEN_PATH    "/proc/game_opt/task_boost/ct_enable"
#define GAMEPID_PATH "/proc/game_opt/game_pid"

/* 关键事件也写这里，让 dmesg 能看到 daemon 干了什么（查 bug 用 dmesg） */
#define KMSG_PATH    "/dev/kmsg"

/* 云控库候选路径 */
static const char *DB_CANDIDATES[] = {
	"/data/user/0/com.oplus.cosa/databases/db_game_database",
	"/data/data/com.oplus.cosa/databases/db_game_database",
	NULL,
};

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
/* 缓存有效期（秒）。见 lookup_cfg 里的说明：云控库的 -wal 一直在变，
 * 「按指纹判失效」等于永不命中，每个游戏每次启动都要重拷 600KB 库 + 开
 * SQLite。改成 TTL：这么长时间内直接用缓存，过一个周期才重查一次。 */
#define CACHE_TTL_S  600
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
	time_t ts;			/* 这条缓存的写入时刻（TTL 用） */
	bool used;
};

static struct cfg_entry g_cache[CACHE_MAX];

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

	/*
	 * 同一条消息也写 /dev/kmsg：dmesg 里就能看到 daemon 什么时候写了什么，
	 * 和内核日志（含 ctn_patch 的 pr_info）在同一条时间线上 —— 排查行为/功耗
	 * 问题对着 dmesg 看就够，不用来回翻两个日志。写不进去就算了，不影响正事。
	 */
	{
		int kfd = open(KMSG_PATH, O_WRONLY | O_CLOEXEC);

		if (kfd >= 0) {
			va_start(ap, fmt);
			vdprintf(kfd, fmt, ap);
			va_end(ap);
			close(kfd);
		}
	}
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

/*
 * 关于 libsqlite 的内存：查完库本来想 dlclose 卸掉，实测**没用** ——
 * bionic 的 dlclose 不会真的 unmapp（这是 Android 的已知行为，库上常带
 * DT_NODELETE）。而且 libsqlite.so 是全系统共享的库，多一个进程映射它，
 * 边际成本只是页表那点，实际占不了多少。所以不做无用的卸载。
 */
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

/*
 * 启动时读一次节点，记住它的原值。游戏退出后恢复成**这个**值，而不是硬编码的
 * UnityMain/UnityGfxDevice —— 万一节点被别的工具/用户设过，不要把它覆盖掉。
 * 节点读不到（模块没加载）就留空，那种情况下也不恢复（本来就没写过）。
 */
static char g_orig_names[NAME_MAX_LEN * 2];
static bool g_patched;		/* 本次运行有没有动过节点 */

static void remember_orig_names(void)
{
	char buf[NAME_MAX_LEN * 4];

	g_orig_names[0] = '\0';
	if (!read_file(NODE_PATH, buf, sizeof(buf)))
		return;
	/* 读回来是 "名字1:-1,名字2:-1" 这种格式，转回 "名字1 名字2" */
	{
		char *colon = strchr(buf, ':');
		char *comma = strchr(buf, ',');

		if (!colon || !comma)
			return;
		*colon = '\0';
		{
			char *second = comma + 1;
			char *c2 = strchr(second, ':');

			if (!c2)
				return;
			*c2 = '\0';
			snprintf(g_orig_names, sizeof(g_orig_names), "%s %s", buf, second);
		}
	}
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
	int rc;

	memset(e, 0, sizeof(*e));
	e->form = CFG_NONE;
	e->ctb = -1;

	if (!db) {
		logmsg("库: 找不到可读的云控库");
		return;
	}

	/*
	 * 缓存命中判定用 TTL，**不再比库指纹**。
	 * 原因：云控库是 WAL 模式，-wal 每秒都在变（COSA 自己也在写），
	 * 指纹判失效的结果就是「永不命中」—— 每个游戏每次启动都要重拷
	 * 600KB 库、再开一次 SQLite（实测日志里每次都是「内容有变，缓存失效」）。
	 * TTL 的代价是云控改了配置最多 N 秒后才生效，这个可以接受（重启 daemon
	 * 会立刻重读）。
	 */
	c = cache_get(pkg);
	if (c->form != CFG_NONE && c->ts &&
	    (time(NULL) - c->ts) < CACHE_TTL_S) {
		*e = *c;		/* 命中缓存：不拷库、不开 SQLite、不查 */
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
	/*
	 * 用完就把临时库删掉：一是空闲时不留那 600KB（占用），
	 * 二是别把用户的云控库副本一直摊在 /data/local/tmp 上（隐私）。
	 * 下一次查库会重新拷一份（现在有 TTL，查库本来就很少）。
	 */
	unlink(dbpath);
	{
		char wal[PATH_MAX];

		snprintf(wal, sizeof(wal), "%s-wal", dbpath);
		unlink(wal);
		snprintf(wal, sizeof(wal), "%s-shm", dbpath);
		unlink(wal);
	}

	/* 写回缓存（连同这次的库指纹） */
	snprintf(c->pkg, CACHE_PKG, "%s", pkg);
	snprintf(c->ctn, CACHE_NAME, "%s", e->ctn);
	c->ctb = e->ctb;
	c->form = e->form;
	c->ts = time(NULL);
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
		/*
		 * 拿到空值 = 上游没给有效配置。**不写节点**（v2.0 起）：
		 * 旧版这里会写回 UnityMain UnityGfxDevice，等于每来一个游戏都去动内核。
		 * 调用方本来就只在真拿到 ctn 时才调这里，走到这说明配置是空的 ——
		 * 那就什么都不做，让内核保持原样。
		 */
		logmsg("名字: 空，不写节点（内核保持原样）");
		return false;
	}
	if (n == 1) {
		/* 只给一个名字就直接写 —— v2.2 起内核自己也接受单名（会把第二个
		 * 槽填成同一个），不用在这儿补一遍 */
		snprintf(out, sizeof(out), "%s", tok[0]);
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
/* 锁文件/pidfile 路径，由 acquire_single_instance() 填（给 write_pidfile 用） */
static char g_lock_path[PATH_MAX];
static char g_pid_path[PATH_MAX];

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

	/*
	 * 顺手把锁文件路径记下来：pidfile 放在同一个目录。
	 * （pidfile 是给脚本用的 —— 见下面 write_pidfile 的说明。）
	 */
	snprintf(g_lock_path, sizeof(g_lock_path), "%s", path);
	return true;
}

/*
 * 把自己的 pid 写进 <模块目录>/ctnd.pid，退出时删掉。
 *
 * 为什么需要它：脚本（service.sh 的守护循环、verify.sh、action.sh）要判断
 * 「daemon 在不在跑」。之前试过两条路都不行：
 *   - pgrep -x ctnd：语义因实现而异（busybox 比的是整条命令行），匹配不上；
 *   - 扫 /proc/<pid>/fd 找「谁握着锁」：能对上，但**极慢** —— 有的进程有 800+
 *     个 fd，全系统扫一遍要几万次 readlink，而守护循环每 2 秒就跑一次，
 *     等于持续在后台扒全系统的 fd（实测执行轨迹 6.7 万行还没跑完，
 *     而且这本身就是一笔实打实的后台 CPU/功耗开销）。
 * pidfile 是 O(1)：脚本读一个文件、看一眼 /proc/<pid> 在不在就够了。
 *
 * 注意：pidfile 只是"提示"，**唯一权威仍然是 flock 锁**（见上）。pidfile
 * 因为 kill -9 之类变成陈旧的没关系 —— 脚本会再确认 /proc/<pid> 在不在。
 */
static void write_pidfile(void)
{
	char buf[32];
	int fd;
	size_t n;

	if (!g_lock_path[0])
		return;
	snprintf(g_pid_path, sizeof(g_pid_path), "%s", g_lock_path);
	n = strlen(g_pid_path);
	{
		/* 注意 "\.ctnd.lock" 是 10 个字符不是 8 —— 写死 8 会剥不掉，
		 * 文件名就成了 ".ctnd.lockctnd.pid"（实测踩过），所以用 strlen 算 */
		static const char suffix[] = ".ctnd.lock";
		size_t sl = sizeof(suffix) - 1;

		if (n > sl && !strcmp(g_pid_path + n - sl, suffix))
			g_pid_path[n - sl] = '\0';
	}
	strncat(g_pid_path, "ctnd.pid", sizeof(g_pid_path) - strlen(g_pid_path) - 1);

	fd = open(g_pid_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return;
	snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
	(void)write(fd, buf, strlen(buf));
	close(fd);
}

static void remove_pidfile(void)
{
	if (g_pid_path[0])
		unlink(g_pid_path);
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

	if (!get_package_name(pid, pkg, sizeof(pkg))) {
		logmsg("游戏 pid=%d，但取不到包名，跳过", (int)pid);
		return;
	}

	lookup_cfg(pkg, &e);

	/*
	 * 只有真的拿到 ctn 才写节点 —— 这是本版最重要的一条（v2.0）。
	 * 旧版在「云控里没 ctn / 库里没这个游戏 / 配置加密」时会写回
	 * UnityMain UnityGfxDevice，虽然值和内核默认相同，但那等于**每启动一个
	 * 游戏就去动一次内核**；有反馈刷入后功耗异常，这条被一并去掉：
	 * 拿不到 ctn 就完全不碰节点，daemon 对内核零影响。
	 */
	if (e.form == CFG_JSON || e.form == CFG_B64JSON) {
		logmsg("游戏启动: %s → 云控 ctn=\"%s\"（%s, ctb=%d）", pkg, e.ctn,
		       e.form == CFG_JSON ? "明文 JSON" : "base64 JSON", e.ctb);
		if (normalize_and_write(e.ctn)) {
			g_patched = true;	/* 记住"我们动过节点"，退出时才需要恢复 */
			if (g_manage_cten && e.ctb >= 0) {
				const char *want = e.ctb ? "1" : "0";

				if (write_file(CTEN_PATH, want))
					logmsg("ct_enable 置 %s（按配置 ctb=%d）", want, e.ctb);
				else
					logmsg("写 ct_enable 失败: %s", strerror(errno));
			}
		}
		return;
	}

	/* 其余情况：不写节点，只说明为什么（查 bug 看 dmesg / daemon.log） */
	switch (e.form) {
	case CFG_JSON:
	case CFG_B64JSON:
		/* 上面 if 里已经处理并 return，走不到这儿 —— 列出来只为消除
		 * -Wswitch-enum 警告（要求把枚举值都写上） */
		break;
	case CFG_EMPTY:
		logmsg("游戏启动: %s → 云控里没有 ctn（Unity 游戏）→ **不写节点**"
		       "（内核默认值本来就是 UnityMain/UnityGfxDevice）", pkg);
		break;
	case CFG_OPAQUE:
		logmsg("游戏启动: %s → 云控配置既不是 JSON 也不是 base64-JSON"
		       "（加密或未知形态）→ **不写节点**", pkg);
		break;
	case CFG_NONE:
	default:
		logmsg("游戏启动: %s → 云控库里没有这个游戏 → **不写节点**", pkg);
		break;
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
			       "只有云控里真有 ctn 才写节点；没有就完全不碰（v2.0 起）\n"
			       "日志走 stderr（service.sh 转到 daemon.log），同时写 /dev/kmsg\n",
			       CTND_VERSION);
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

	write_pidfile();

	/* 先记下节点原值：退出时恢复成它，而不是硬编码的 Unity 串 */
	remember_orig_names();

	logmsg("ctnd %s 启动（节点 %s%s%s）", CTND_VERSION, NODE_PATH,
	       g_manage_cten ? "，代管 ct_enable" : "",
	       g_db_override ? "，库由 --db 指定" : "");

	/* 先确认节点在。模块没加载/加载失败时节点不存在，这时没必要每来一个
	 * 游戏就报一次写入失败；提一次就好，后面该写还是会试着写。 */
	if (access(NODE_PATH, W_OK) != 0)
		logmsg("注意: 现在写不了 %s（%s）—— ctn_patch 模块没加载？"
		       " 节点出现后本进程会照常工作", NODE_PATH, strerror(errno));

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
			last = -1;
			gone = 0;
			/* 只有本次真的动过节点才需要恢复；恢复成启动时读到的原值 */
			if (g_patched) {
				if (g_orig_names[0]) {
					logmsg("游戏退出（连续 %d 次 -1），恢复原值 [%s]",
					       GONE_POLLS, g_orig_names);
					if (!write_file(NODE_PATH, g_orig_names))
						logmsg("恢复失败: %s", strerror(errno));
				} else {
					logmsg("游戏退出：不知道原值（启动时节点读不到），不恢复");
				}
				g_patched = false;
			} else {
				logmsg("游戏退出（连续 %d 次 -1），本次没动过节点，什么都不做",
				       GONE_POLLS);
			}
		}
	}

	logmsg("ctnd 退出");
	remove_pidfile();
	if (g_lock_fd >= 0)
		close(g_lock_fd);
	return 0;
}
