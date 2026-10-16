/*
 * ctnd —— critical task name daemon   v2.1
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
 * v2.1 的改动（用户要求：云控里的 ctn 被改了也要生效 + 性能/占用/体积）：
 *   [核心] **游戏运行中检测 ctn 变化并重新下发**。旧版只在 game_pid 变化的瞬间
 *          应用一次，游戏跑着的时候云控改了配置永远不生效（要重启游戏或 daemon）。
 *          现在游戏运行期间每 recheck 秒复查一次云控：值变了就重写节点、
 *          配置里 ctn 被移除就恢复原值、ctb 变了（-e 模式）跟着重写。
 *   [速度/占用] 复查不是每轮都拷库：先看库文件 stat 指纹（mtime+size），
 *          没变就直接用缓存 —— 大部分复查是零拷贝的；变了才拷 600KB 开 SQLite。
 *   [占用] /dev/kmsg 的 fd 常开（旧版每条日志 open+close 一次，纯浪费）。
 *   [兼容] 新增 -r/--recheck 调复查周期；复查失败不影响已应用状态。
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

#define CTND_VERSION "2.1"

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
/* 游戏运行中复查云控 ctn 的周期（秒）。复查自带 stat 指纹：库没变时零拷贝，
 * 常驻开销可忽略。-r/--recheck 可覆盖；0 = 关闭复查。 */
#define RECHECK_S    30
#define RECHECK_S_MIN 5

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

/*
 * 库文件 stat 指纹：复查时先看这个，没变就不拷库、不开 SQLite（零开销）。
 * 注意 -wal 在 COSA 手里可能一直在变，指纹变了只是「值得再查一次」，
 * 不代表配置真的变了 —— 查完没变就把指纹记下来，下次同样的指纹直接跳过。
 *
 * 指纹**跟着缓存条目走**（v2.1 修正）：早期版本用一个全局指纹，会出现
 * 「B 包缓存于 F0 → 库变成 F1 → A 包查询把全局指纹刷成 F1 → B 包再启动时
 * 指纹又"相同"了 → 拿到改库前的旧缓存」这种串味。存进条目里就没有了。
 */
struct db_fp {
	long long mtime_db, size_db, mtime_wal, size_wal;
	bool valid;
};

struct cfg_entry {
	char pkg[CACHE_PKG];
	char ctn[CACHE_NAME];
	int  ctb;
	enum cfg_form form;
	time_t ts;			/* 这条缓存的写入时刻（TTL 用） */
	struct db_fp fp;		/* 查库成功那一刻的库指纹（本条目专用） */
	bool used;
};

static struct cfg_entry g_cache[CACHE_MAX];

/* /dev/kmsg 常开一个 fd：旧版每条日志 open+close 一次，纯浪费。
 * 打不开（权限/内核不支持）就置 -1，日志只走 stderr，不影响正事。 */
static int g_kmsg_fd = -1;

static void open_kmsg(void)
{
	if (g_kmsg_fd < 0)
		g_kmsg_fd = open(KMSG_PATH, O_WRONLY | O_CLOEXEC);
}

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
	 * 问题对着 dmesg 看就够，不用来回翻两个日志。fd 常开（v2.1），
	 * 打不开就算了，不影响正事。
	 */
	if (g_kmsg_fd >= 0) {
		va_start(ap, fmt);
		vdprintf(g_kmsg_fd, fmt, ap);
		va_end(ap);
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

/*
 * 运行中变化检测（v2.1）：记住当前游戏包名、以及本次实际写入节点的内容。
 * 复查发现云控里的 ctn 变了 → 重新写；配置里 ctn 被移除 → 恢复启动时原值。
 */
static char g_cur_pkg[256];
static char g_applied[300];
static bool g_applied_valid;
static time_t g_last_recheck;
static long g_recheck_s = RECHECK_S;

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
	/*
	 * 必须找到的是**键**：挑到候选位置后要求后面紧跟冒号。
	 * 不能只用 strstr 取第一次出现 —— game_config 里某条**数据**的值如果正好
	 * 等于 "ctn"（比如 {"a":"ctn","ctn":"真名字"}），strstr 会先命中那个值，
	 * 冒号检查不过就直接 return false，真名字反而漏掉。所以这里往后逐个候选试。
	 */
	for (p = strstr(json, pat); p; p = strstr(p + 1, pat)) {
		const char *q2 = p + strlen(pat);

		while (*q2 && isspace((unsigned char)*q2))
			q2++;
		if (*q2 == ':') {
			p = q2;
			break;
		}
	}
	if (!p)
		return false;
	p++;				/* 原代码在冒号检查后本来就有这一句，别再加 */
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
	/* 同上：只认后面跟冒号的**键** */
	for (p = strstr(json, pat); p; p = strstr(p + 1, pat)) {
		const char *q2 = p + strlen(pat);

		while (*q2 && isspace((unsigned char)*q2))
			q2++;
		if (*q2 == ':') {
			p = q2;
			break;
		}
	}
	if (!p)
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

/* --------------------------- 库指纹 --------------------------- */

/*
 * 记录库文件（主库 + -wal）的 mtime/size。复查时先比对指纹：
 * 没变 → 库内容大概率没变，直接用缓存（零拷贝）；变了 → 重新查。
 * 只在**成功查到一次**后更新，失败的查询不动指纹（下次照样重试）。
 */
static bool db_fingerprint(const char *db, struct db_fp *fp)
{
	char wal[PATH_MAX];
	struct stat st, wst;

	memset(fp, 0, sizeof(*fp));
	if (stat(db, &st) != 0)
		return false;
	fp->mtime_db = (long long)st.st_mtim.tv_sec;
	fp->size_db = (long long)st.st_size;
	fp->valid = true;
	snprintf(wal, sizeof(wal), "%s-wal", db);
	if (stat(wal, &wst) == 0) {
		fp->mtime_wal = (long long)wst.st_mtim.tv_sec;
		fp->size_wal = (long long)wst.st_size;
	}
	return true;
}

static bool db_fp_same(const struct db_fp *a, const struct db_fp *b)
{
	return a->valid && b->valid &&
	       a->mtime_db == b->mtime_db && a->size_db == b->size_db &&
	       a->mtime_wal == b->mtime_wal && a->size_wal == b->size_wal;
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
		if (!copy_file(s, d)) {
			if (suffixes[i][0] == '\0') {
				logmsg("库: 拷贝 %s 失败: %s", s, strerror(errno));
				return false;
			}
			/* -wal 没拷到：主库还在，但可能读到**没合并进主库的**旧配置。
			 * 出声，别静默按旧数据办事。 */
			logmsg("库: 拷贝 %s 失败: %s（可能读到旧配置）",
			       s, strerror(errno));
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

/*
 * 取一个包的关键线程名，结果写进 e。缓存带 TTL + 库 stat 指纹两层判定。
 *
 * 返回值 = **这次查询本身成功了吗**（v2.1 起区分）：
 *   true  —— 查到了：e->form 说明「有 ctn / 没 ctn / 库里没这个游戏」；
 *   false —— 查不动：找不到库 / 拷库失败 / SQLite 打不开。**e 里不是结论**。
 * 这个区分很关键：复查路径拿到「库里没这个游戏」要恢复原值，而拿到
 * 「查不动」必须保持现状 —— 否则一次临时 IO 故障就会把正在加速的游戏
 * 恢复成默认名（30 秒后下个周期又写回来，来回跳）。
 */
static bool lookup_cfg(const char *pkg, struct cfg_entry *e)
{
	const char *db = find_db();
	struct cfg_entry *c;
	char dbpath[PATH_MAX];
	struct db_fp fp;
	bool fresh;
	sqlite3 *sq = NULL;
	sqlite3_stmt *st = NULL;
	const unsigned char *txt;
	int rc;

	memset(e, 0, sizeof(*e));
	e->form = CFG_NONE;
	e->ctb = -1;

	if (!db) {
		logmsg("库: 找不到可读的云控库");
		return false;
	}

	/*
	 * 两层缓存判定（v2.1）：
	 *   1. TTL（600s）：到期必查一次 —— 兜底，云控改动最迟几分钟生效；
	 *   2. TTL 内看库文件 stat 指纹（mtime+size）：没变 → 直接用缓存（零拷贝，
	 *      不拷 600KB、不开 SQLite）；变了 → 立刻重查。这样「云控改了配置」
	 *      最快一个复查周期内生效，而库没变的复查是零开销的。
	 * 旧版教训仍在：-wal 在 COSA 手里一直在变，指纹变了不代表配置真变了 ——
	 * 查完没变就把指纹记下来，同样的指纹不再重复查。
	 * 指纹比的是**本条目自己记下的那次**（c->fp），不是全局的 —— 见 struct db_fp 说明。
	 */
	c = cache_get(pkg);
	fresh = c->used && c->ts && (time(NULL) - c->ts) < CACHE_TTL_S;
	if (fresh && db_fingerprint(db, &fp) && db_fp_same(&fp, &c->fp)) {
		*e = *c;		/* 指纹没变：不拷库、不开 SQLite、不查 */
		return true;
	}

	if (!load_sqlite())
		return false;
	if (!stage_db(db, dbpath, sizeof(dbpath)))
		return false;

	rc = p_sqlite3_open_v2(dbpath, &sq, SQLITE_OPEN_READONLY, NULL);
	if (rc != SQLITE_OK) {
		logmsg("库: 打开失败 rc=%d", rc);
		if (sq) p_sqlite3_close(sq);
		return false;
	}
	rc = p_sqlite3_prepare_v2(sq,
		"SELECT game_config FROM PackageConfigBean WHERE package_name = ?1",
		-1, &st, NULL);
	if (rc != SQLITE_OK) {
		logmsg("库: prepare 失败: %s", p_sqlite3_errmsg(sq));
		p_sqlite3_close(sq);
		return false;
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

	/* 写回缓存（连同**这次**查库时刻的指纹，供本条目下次自查） */
	snprintf(c->pkg, CACHE_PKG, "%s", pkg);
	snprintf(c->ctn, CACHE_NAME, "%s", e->ctn);
	c->ctb = e->ctb;
	c->form = e->form;
	c->fp = fp;
	c->ts = time(NULL);
	c->used = true;
	return true;
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
 * 归一化（**不写节点**，静默）：2 个 → 原样；1 个 → 同名一遍（内核里两个槽记
 * 同一线程同一 CPU，等价于只填一个）；0 个 → false；>2 个取前两个。
 * 静默的原因：复查路径每 30 秒就要比一次，警告日志只在真正写入时出
 * （apply_names），不然一个 >15 字符的名字能把日志刷满一整局。
 * 名字超过 15 字符内核永远匹配不到（task->comm 只有 16 字节含 NUL）——
 * 由 apply_names 告警。
 */
static bool normalize_names(const char *names, char *out, size_t outlen)
{
	char buf[512];
	char *tok[8];
	int n = 0;
	char *p;

	if (outlen < 2)
		return false;
	snprintf(buf, sizeof(buf), "%s", names ? names : "");
	for (p = strtok(buf, " \t\r\n"); p && n < 8; p = strtok(NULL, " \t\r\n"))
		tok[n++] = p;

	if (n == 0)
		return false;	/* 上游没给有效配置：不写节点（v2.0 起） */
	if (n == 1) {
		/* 单名直接收 —— 内核会把第二个槽填成同一个（v2.1 起） */
		snprintf(out, outlen, "%s", tok[0]);
	} else {
		snprintf(out, outlen, "%s %s", tok[0], tok[1]);
	}
	return true;
}

/*
 * 归一化 + 写节点 + 日志。written 带回实际写入的内容（调用方记进应用状态，
 * 复查时比对用）。
 */
static bool apply_names(const char *names, char *written, size_t wlen)
{
	char buf[512];
	char *tok[4];
	int n = 0;
	char *p;
	int i;

	if (wlen < 2 || !normalize_names(names, written, wlen))
		return false;

	/* >15 字符的名字内核匹配不到（task->comm 只有 16 字节含 NUL），写一次警告 */
	snprintf(buf, sizeof(buf), "%s", written);
	for (p = strtok(buf, " \t"); p && n < 2; p = strtok(NULL, " \t"))
		tok[n++] = p;
	for (i = 0; i < n; i++)
		if (strlen(tok[i]) >= 16)
			logmsg("名字: 警告 [%s] 超过 15 字符，内核匹配不到"
			       "（task->comm 只有 16 字节）", tok[i]);

	if (!write_file(NODE_PATH, written)) {
		logmsg("写入失败 %s: %s（模块加载了吗？）", NODE_PATH, strerror(errno));
		return false;
	}
	logmsg("已写入 [%s]", written);
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
	ssize_t n;
	int fd;

	/*
	 * 用 /proc/self/exe 拿自己的真实路径，不用调用方给的 argv[0]：
	 * argv[0] 若是不含 '/' 的相对名字（被别的程序 execv("ctnd",...) 拉起、
	 * 或从别处调进来），按它算出的目录会落到**当前工作目录** —— 锁和 pidfile
	 * 就跟脚本找的位置（$MODDIR/ctnd.pid）对不上了。exe 不受调用方式影响。
	 */
	n = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (n > 0) {
		path[n] = '\0';
	} else {
		snprintf(path, sizeof(path), "%s", exe);	/* 极端兜底 */
	}
	slash = strrchr(path, '/');
	if (slash)
		slash[1] = '\0';		/* 截到最后一个 / 之后 */
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

	/*
	 * 记住当前游戏包名 —— 运行中复查（v2.1）按它查云控。
	 * 这次没拿到 ctn 也要记：云控可能过一会儿才把配置推下来，
	 * 复查发现新配置后照常应用。
	 */
	snprintf(g_cur_pkg, sizeof(g_cur_pkg), "%s", pkg);
	g_applied_valid = false;
	g_last_recheck = time(NULL);

	/*
	 * 查库失败（临时故障：拷库失败/SQLite 打不开）不能当结论用 ——
	 * 直接跳过本次，什么都不动；包名已记下，下轮复查会重试。
	 */
	if (!lookup_cfg(pkg, &e)) {
		logmsg("游戏启动: %s → 查库失败，本次不写节点（复查会重试）", pkg);
		return;
	}
	g_last_recheck = time(NULL);

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
		/* written 写进 g_applied：运行中复查拿它判断「值有没有变」 */
		if (apply_names(e.ctn, g_applied, sizeof(g_applied))) {
			g_applied_valid = true;
			g_patched = true;	/* 记住"我们动过节点"，退出时才需要恢复 */
			if (g_manage_cten && e.ctb >= 0) {
				const char *want = e.ctb ? "1" : "0";

				if (write_file(CTEN_PATH, want))
					logmsg("ct_enable 置 %s（按配置 ctb=%d）", want, e.ctb);
				else
					logmsg("写 ct_enable 失败: %s", strerror(errno));
			}
		} else {
			logmsg("ctn 归一化后为空，不写节点");
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

/* --------------------------- 运行中复查（v2.1 核心） --------------------------- */

/*
 * 游戏运行中每 g_recheck_s 秒复查一次云控：
 *   - ctn 变了        → 重新写节点（这是「db 里的 ctn 参数被修改也要应用」的主路径）；
 *   - ctn 被移除/变空 → 恢复启动时的原值（配置不再要求加速这个游戏）；
 *   - ctb 变了（-e 模式）→ ct_enable 跟着重写；
 *   - 没变化          → 一行日志都不出；
 *   - **查库失败**     → 什么都不动（v2.1 修正，见下）。
 * 查询自带 stat 指纹：库没变时是零拷贝的（不拷 600KB、不开 SQLite）。
 * 单实例 + 串行主循环，不存在并发写节点的问题。
 */
static void recheck_running_game(void)
{
	struct cfg_entry e;
	char want[300];

	/*
	 * 查不动库（拷库失败/SQLite 打不开）≠ 云控删了配置。
	 * 这里必须保持现状：早期版本把两者混为一谈，一次临时 IO 故障就会把
	 * 正在加速的节点恢复成默认名，30 秒后下个周期又写回来 —— 来回跳。
	 */
	if (!lookup_cfg(g_cur_pkg, &e)) {
		logmsg("复查: 查库失败，保持现状（不动节点，下轮重试）");
		return;
	}

	if (e.form == CFG_JSON || e.form == CFG_B64JSON) {
		if (!normalize_names(e.ctn, want, sizeof(want)))
			return;		/* 归一化后为空：等价于 ctn 被移除，走下面 */
		if (g_applied_valid && strcmp(want, g_applied) == 0)
			return;		/* 没变化：静默 */

		logmsg("复查: 云控 ctn 变化 [%s] → [%s]，重新应用",
		       g_applied_valid ? g_applied : "(未应用)", want);
		if (apply_names(e.ctn, g_applied, sizeof(g_applied))) {
			g_applied_valid = true;
			g_patched = true;
			if (g_manage_cten && e.ctb >= 0) {
				const char *w = e.ctb ? "1" : "0";

				if (write_file(CTEN_PATH, w))
					logmsg("复查: ct_enable 置 %s（按配置 ctb=%d）",
					       w, e.ctb);
			}
		}
		return;
	}

	/* 配置里的 ctn 没了（云控下掉了 / 换成加密形态）→ 撤销本次应用。
	 * 只有我们确实动过节点才恢复；恢复成启动时读到的原值。 */
	if (g_applied_valid) {
		logmsg("复查: 云控 ctn 已移除（form=%d），恢复原值 [%s]",
		       e.form, g_orig_names);
		if (g_orig_names[0] && write_file(NODE_PATH, g_orig_names))
			g_patched = false;
		g_applied_valid = false;
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
		} else if (!strcmp(argv[i], "-r") ||
			   !strcmp(argv[i], "--recheck")) {
			char *end;
			long v;

			if (i + 1 >= argc) {
				fprintf(stderr, "!! -r 后面要跟秒数\n");
				return 2;
			}
			v = strtol(argv[++i], &end, 10);
			/* 非数字必须报错：atol("abc")=0 会**静默关掉复查**，
			 * 看着像"参数没生效"，实际是功能被关了，很难查。 */
			if (*end != '\0' || end == argv[i]) {
				fprintf(stderr, "!! -r 要跟一个整数秒数（收到 \"%s\"）\n",
					argv[i]);
				return 2;
			}
			/* 0 = 关闭运行中复查；太小会高频拷库，钳到 5 秒 */
			if (v == 0)
				g_recheck_s = 0;
			else
				g_recheck_s = v < RECHECK_S_MIN ? RECHECK_S_MIN : v;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			printf("ctnd %s —— 按云控配置自动写 critical_task_name\n"
			       "用法: ctnd [-e] [-d 库路径] [-r 秒] [-V]\n"
			       "  -e, --enable-ctb   连 ct_enable 一起按配置的 ctb 开关\n"
			       "                     （默认不管：HAL 自己会写，同时管可能互相打架）\n"
			       "  -d, --db <路径>    指定云控库（默认按内置候选路径找；\n"
			       "                     换机型/库不在候选列表时用这个）\n"
			       "  -r, --recheck <秒> 游戏运行中复查云控 ctn 的周期（默认 %ld，\n"
			       "                     最小 %d，0=关闭；ctn 变了会重新下发）\n"
			       "  -V, --version      打印版本\n"
			       "只有云控里真有 ctn 才写节点；没有就完全不碰（v2.0 起）\n"
			       "游戏运行中每轮复查云控 ctn，被修改会自动重新应用（v2.1 起）\n"
			       "日志走 stderr（service.sh 转到 daemon.log），同时写 /dev/kmsg\n",
			       CTND_VERSION, g_recheck_s, RECHECK_S_MIN);
			return 0;
		}
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	/* 父进程（service.sh）退出时不要把我们带走 */
	signal(SIGHUP, SIG_IGN);

	/* 日志要写 /dev/kmsg：fd 常开，省掉每条日志一次 open+close（v2.1） */
	open_kmsg();

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

		if (!read_file(GAMEPID_PATH, buf, sizeof(buf))) {
			continue;
		}

		p = parse_game_pid(buf);
		if (p == last) {
			/*
			 * 游戏还在跑（pid 没变）：周期性复查云控 ctn（v2.1）。
			 * 「db 里的 ctn 参数被修改也要应用」就是走这里 ——
			 * 查询带 stat 指纹，库没变时零拷贝，常驻开销可忽略。
			 */
			if (last > 0 && g_recheck_s > 0 &&
			    g_cur_pkg[0] &&
			    time(NULL) - g_last_recheck >= g_recheck_s) {
				g_last_recheck = time(NULL);
				recheck_running_game();
			}
			continue;
		}

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
			/* 复查状态一并清掉：没有"当前游戏"就没有复查对象 */
			g_cur_pkg[0] = '\0';
			g_applied_valid = false;
			g_last_recheck = 0;
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
	if (g_kmsg_fd >= 0)
		close(g_kmsg_fd);
	return 0;
}
