// SPDX-License-Identifier: GPL-2.0-only
/*
 * ctnd —— critical task name daemon
 *
 * 为什么需要它：
 *   ctn_patch 模块把 /proc/game_opt/task_boost/critical_task_name 补出来了，
 *   但**没有任何进程会去写它** —— 一加 6.1 的 gameopt HAL 走的是 sched_assist
 *   那条路（ioctl + pipeline_pids_cpus），从来没碰过内核的 critical_task[]。
 *   所以节点一直停在默认的 UnityMain / UnityGfxDevice，对非 Unity 游戏毫无作用。
 *
 * 它做什么：
 *   1. 盯 /proc/game_opt/game_pid —— HAL 在游戏启动时把游戏 pid 写进去，
 *      退出时写 -1。这就是最可靠的"游戏开了/关了"信号。
 *   2. 游戏一起来，从 pid 取包名，去 COSA 的云控库里查该游戏的 game_config.ctn。
 *        /data/user/0/com.oplus.cosa/databases/db_game_database  (SQLite)
 *      ctn 就是官方给的关键线程名，格式与我们的节点完全一致（空格分隔两个名字）：
 *        pubgmhd  -> "RenderThread Thread-"
 *        dfm      -> "GameThread RenderThread"
 *      Unity 游戏没有 ctn（内核默认的 UnityMain/UnityGfxDevice 正好就是它们的
 *      线程名），这种情况写回默认值。
 *   3. 把结果写进 critical_task_name。
 *
 * 为什么读 SQLite 要先拷贝：
 *   库是 WAL 模式且被 COSA 进程随时写。直接开原库会去动它的 -shm，
 *   为免干扰别人，这里把 db/-wal/-shm 三个文件拷到自己的临时目录再读。
 *   只在游戏启动那一刻做一次，开销可忽略。
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
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------ 常量 ------------------------------ */

#define NODE_PATH   "/proc/game_opt/task_boost/critical_task_name"
#define CTEN_PATH   "/proc/game_opt/task_boost/ct_enable"
#define GAMEPID_PATH "/proc/game_opt/game_pid"

#define COSA_DB     "/data/user/0/com.oplus.cosa/databases/db_game_database"
/* 备选：不同机型/版本可能放这儿 */
#define COSA_DB_ALT "/data/data/com.oplus.cosa/databases/db_game_database"

#define DEFAULT_NAMES "UnityMain UnityGfxDevice"

/* SQLite 库在设备上的候选路径（这台设备叫 libsqlite.so，不是 libsqlite3.so） */
static const char *SQLITE_CANDIDATES[] = {
	"/system/lib64/libsqlite.so",
	"/system/lib64/libsqlite3.so",
	"/apex/com.android.runtime/lib64/libsqlite3.so",
	"/apex/com.android.art/lib64/libsqlite3.so",
	NULL,
};

#define NAME_MAX_LEN 100          /* 与内核接口一致（官方 6.6 用 %99s） */
#define TMP_DIR      "/data/local/tmp/.ctnd"
#define POLL_MS      500
/* 连续读到多少次 -1 才认定游戏真的退出（挡掉加载期的抖动） */
#define GONE_POLLS   8            /* 8 × 500ms = 4 秒 */

/* ------------------------------ 日志 ------------------------------ */

static FILE *g_log;
static bool g_verbose;

static void logmsg(const char *fmt, ...)
{
	char ts[32];
	time_t now = time(NULL);
	struct tm tm;

	localtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &tm);

	va_list ap;
	va_start(ap, fmt);
	if (g_log) {
		fprintf(g_log, "[%s] ", ts);
		vfprintf(g_log, fmt, ap);
		fputc('\n', g_log);
		fflush(g_log);
	}
	if (!g_log || g_verbose) {
		fprintf(stderr, "[%s] ", ts);
		va_list ap2;
		va_start(ap2, fmt);
		vfprintf(stderr, fmt, ap2);
		va_end(ap2);
		fputc('\n', stderr);
	}
	va_end(ap);
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

/* --------------------------- 从 /proc 取包名 --------------------------- */

/*
 * 用 cmdline 取进程名。游戏主进程的 cmdline 一般就是包名；
 * 有些游戏（如某些 Unity 包）主进程 cmdline 会带参数，所以只取第一段。
 * 若 cmdline 为空（内核线程/已退出），退回 comm。
 */
static bool get_package_name(pid_t pid, char *out, size_t outlen)
{
	char path[64];
	char buf[512];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n > 0) {
			buf[n] = '\0';
			/* cmdline 用 \0 分隔参数，第一段就是进程名 */
			if (buf[0] != '\0') {
				snprintf(out, outlen, "%s", buf);
				return true;
			}
		}
	}

	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	if (read_file(path, buf, sizeof(buf))) {
		buf[strcspn(buf, "\n")] = '\0';
		if (buf[0]) {
			snprintf(out, outlen, "%s", buf);
			return true;
		}
	}
	return false;
}

/* --------------------------- 极简 JSON 取值 --------------------------- */
/*
 * game_config 是扁平 JSON（所有键都在顶层），所以不需要完整解析器。
 * 只做一件事：找到 "key" 后面那个字符串值，并处理 \" 和 \\ 转义。
 * 找不到返回 false（对应 Unity 游戏没有 ctn 的情况）。
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

	/* 跳过空白与冒号 */
	while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	if (*p != ':')
		return false;
	p++;
	while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	if (*p != '"')
		return false;		/* 不是字符串（可能是 null / 数字 / 对象） */
	p++;

	q = p;
	while (*q && k + 1 < outlen) {
		if (*q == '\\' && q[1]) {
			q++;			/* 转义：原样收下下一个字符 */
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

/*
 * 取一个数字值（如 "ctb":1）。
 * 注意 game_config 里 ctb/htb/ctep 是数字，而 ctn 是字符串，
 * 两种类型要分开处理 —— 一开始只用字符串取值器，结果 ctb 永远取不到。
 * 取不到（缺字段、是 null、是对象）返回 false。
 */
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
	return false;		/* null / 字符串 / 对象 都当取不到 */
}

/* --------------------------- 查库拿 ctn --------------------------- */

/* 把 COSA 的库（含 WAL/SHM）拷到临时目录，避免干扰原库 */
static bool stage_db(char *out_path, size_t outlen)
{
	static const char *suffixes[] = { "", "-wal", "-shm", NULL };
	const char *src = NULL;
	int i;

	if (access(COSA_DB, R_OK) == 0)
		src = COSA_DB;
	else if (access(COSA_DB_ALT, R_OK) == 0)
		src = COSA_DB_ALT;
	if (!src) {
		logmsg("库: 找不到可读的 COSA 数据库");
		return false;
	}

	mkdir(TMP_DIR, 0700);
	for (i = 0; suffixes[i]; i++) {
		char s[PATH_MAX], d[PATH_MAX];

		snprintf(s, sizeof(s), "%s%s", src, suffixes[i]);
		snprintf(d, sizeof(d), "%s/db%s", TMP_DIR, suffixes[i]);
		unlink(d);
		if (!copy_file(s, d) && suffixes[i][0] == '\0') {
			logmsg("库: 拷贝 %s 失败: %s", s, strerror(errno));
			return false;
		}
	}
	snprintf(out_path, outlen, "%s/db", TMP_DIR);
	return true;
}

/*
 * 查某个包的 game_config。
 * 返回：
 *   0 = 有 ctn（out 里是名字串）
 *   1 = 查到了包但没 ctn（Unity 游戏，内核默认名就是它的线程名）
 *   2 = 包里压根不在库里（OPLUS 不认识的游戏）→ 也用默认名
 *  -1 = 读库出错（库被锁/路径变了）→ 保持现状不动，别写错名字
 * 顺带把 ctb 取出来给 --enable-ctb 用（*ctb 置为 -1 表示没这个字段）。
 */
static int lookup_ctn(const char *pkg, char *out, size_t outlen, int *ctb)
{
	char dbpath[PATH_MAX];
	char cfg[8192];
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	const unsigned char *txt;
	int rc, ret;

	*ctb = -1;
	if (!load_sqlite())
		return -1;
	if (!stage_db(dbpath, sizeof(dbpath)))
		return -1;

	rc = p_sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY, NULL);
	if (rc != SQLITE_OK) {
		logmsg("库: 打开失败 rc=%d", rc);
		if (db) p_sqlite3_close(db);
		return -1;
	}

	rc = p_sqlite3_prepare_v2(db,
		"SELECT game_config FROM PackageConfigBean WHERE package_name = ?1",
		-1, &st, NULL);
	if (rc != SQLITE_OK) {
		logmsg("库: prepare 失败: %s", p_sqlite3_errmsg(db));
		p_sqlite3_close(db);
		return -1;
	}

	p_sqlite3_bind_text(st, 1, pkg, -1, NULL);
	rc = p_sqlite3_step(st);
	if (rc != SQLITE_ROW) {
		logmsg("库: 包 %s 不在 PackageConfigBean 里（OPLUS 不认识它）", pkg);
		ret = 2;
		goto out;
	}

	txt = p_sqlite3_column_text(st, 0);
	if (!txt) {
		ret = 1;
		goto out;
	}
	snprintf(cfg, sizeof(cfg), "%s", (const char *)txt);

	{
		int v;
		if (json_get_int(cfg, "ctb", &v))
			*ctb = v;
	}
	ret = json_get_string(cfg, "ctn", out, outlen) ? 0 : 1;

out:
	p_sqlite3_finalize(st);
	p_sqlite3_close(db);
	return ret;
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

	snprintf(buf, sizeof(buf), "%s", names ? names : "");
	for (p = strtok(buf, " \t\r\n"); p && n < 8; p = strtok(NULL, " \t\r\n"))
		tok[n++] = p;

	if (n == 0) {
		snprintf(out, sizeof(out), "%s", DEFAULT_NAMES);
		logmsg("名字: 配置里是空的，用默认值 [%s]", out);
	} else if (n == 1) {
		snprintf(out, sizeof(out), "%s %s", tok[0], tok[0]);
		logmsg("名字: 配置里只有 1 个 [%s]，同名写两遍", tok[0]);
	} else {
		snprintf(out, sizeof(out), "%s %s", tok[0], tok[1]);
		if (n > 2)
			logmsg("名字: 配置里有 %d 个，只取前两个", n);
	}

	if (strlen(tok[0]) >= 16)
		logmsg("名字: 警告 [%s] 超过 15 字符，内核匹配不到（task->comm 只有 16 字节）",
		       tok[0]);

	if (!write_file(NODE_PATH, out)) {
		logmsg("写入失败 %s: %s（模块加载了吗？）", NODE_PATH, strerror(errno));
		return false;
	}
	logmsg("已写入 [%s]", out);
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
	char pkg[256] = {0};
	char ctn[NAME_MAX_LEN * 2] = {0};
	int ctb = -1;
	int r;

	if (!get_package_name(pid, pkg, sizeof(pkg))) {
		logmsg("游戏 pid=%d，但取不到包名，跳过", (int)pid);
		return;
	}
	logmsg("游戏启动: pid=%d 包名=%s", (int)pid, pkg);

	r = lookup_ctn(pkg, ctn, sizeof(ctn), &ctb);
	if (r == 0) {
		logmsg("配置 ctn = \"%s\"（ctb=%d）", ctn, ctb);
		normalize_and_write(ctn);
	} else if (r == 1) {
		logmsg("配置里没有 ctn（Unity 游戏），写回默认值");
		normalize_and_write(DEFAULT_NAMES);
	} else if (r == 2) {
		logmsg("OPLUS 不认识这个游戏，写回默认值");
		normalize_and_write(DEFAULT_NAMES);
	} else {
		/* 读库失败：宁可不动，也别写错名字覆盖掉上一局正确的值 */
		logmsg("读库失败，保持节点现状不动");
		return;
	}

	/*
	 * ct_enable 归 HAL 管（它按 game_config.ctb + 游戏场景判定来开关），
	 * 默认我们不动它，免得两边互相打架。--enable-ctb 时才代管。
	 */
	if (g_manage_cten && ctb >= 0) {
		const char *want = ctb ? "1" : "0";
		if (write_file(CTEN_PATH, want))
			logmsg("ct_enable 置 %s（按配置 ctb=%d）", want, ctb);
		else
			logmsg("写 ct_enable 失败: %s", strerror(errno));
	}
}

int main(int argc, char **argv)
{
	char buf[256];
	pid_t last = -1;
	int gone = 0;			/* 连续读到 -1 的次数，用于退出防抖 */
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			g_verbose = true;
		else if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "--enable-ctb"))
			g_manage_cten = true;
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			printf("ctnd —— 按云控配置自动写 critical_task_name\n"
			       "用法: ctnd [-v] [-e]\n"
			       "  -v, --verbose      日志同时打到 stderr\n"
			       "  -e, --enable-ctb   连 ct_enable 一起按配置的 ctb 开关\n"
			       "                     （默认不管：HAL 自己会写，同时管可能互相打架）\n"
			       "  日志走 stderr，由 service.sh 重定向到 daemon.log\n");
			return 0;
		}
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	/* 父进程（service.sh）退出时不要把我们带走 */
	signal(SIGHUP, SIG_IGN);

	logmsg("ctnd 启动（节点 %s%s）", NODE_PATH,
	       g_manage_cten ? "，代管 ct_enable" : "");

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
			logmsg("游戏退出（pid 连续两次为 -1），恢复默认名字");
			last = -1;
			gone = 0;
			normalize_and_write(DEFAULT_NAMES);
		}
	}

	logmsg("ctnd 退出");
	if (g_log)
		fclose(g_log);
	return 0;
}
