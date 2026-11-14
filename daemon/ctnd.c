/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "ctnd.h"
#include "ctnd_db.h"
#include "ctnd_fs.h"
#include "ctnd_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;

struct app {
	struct ctnd_runtime runtime;
	struct ctnd_db db;
	int kmsg;
};

static void on_signal(int sig)
{
	(void)sig;
	stopped = 1;
}

__attribute__((format(printf, 2, 3)))
static void logmsg(struct app *app, const char *format, ...)
{
	char message[512], timestamp[32];
	struct tm tm;
	time_t now = time(NULL);
	va_list args;

	va_start(args, format);
	(void)vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	if (localtime_r(&now, &tm)) (void)strftime(timestamp, sizeof(timestamp), "%m-%d %H:%M:%S", &tm);
	else (void)snprintf(timestamp, sizeof(timestamp), "unknown-time");
	fprintf(stderr, "[%s] ctnd: %s\n", timestamp, message);
	fflush(stderr);
	if (app->kmsg >= 0) (void)dprintf(app->kmsg, "<6>ctnd: %s\n", message);
}

static bool read_node(void *ctx, enum ctnd_node node, char *out, size_t cap)
{
	(void)ctx;
	return ctnd_runtime_read_node(node, out, cap);
}

static bool write_node(void *ctx, enum ctnd_node node, const char *value)
{
	(void)ctx;
	return ctnd_runtime_write_node(node, value);
}

static bool save_state(void *ctx, const struct ctnd_journal *journal)
{
	struct app *app = ctx;

	return ctnd_runtime_save(&app->runtime, journal);
}

static bool package(void *ctx, int pid, char *out, size_t cap)
{
	(void)ctx;
	return ctnd_runtime_package(pid, out, cap);
}

static enum ctnd_result lookup(void *ctx, const char *pkg, uint64_t now, struct ctnd_cfg *cfg)
{
	struct app *app = ctx;
	enum ctnd_result result = ctnd_db_lookup(&app->db, pkg, now, cfg);

	if (result == CTND_ERROR) logmsg(app, "database query for %s failed; keeping node state", pkg);
	else if (result == CTND_UNSUPPORTED)
		logmsg(app, "unsupported configuration for %s; not interpreting it as deletion", pkg);
	return result;
}

static void event(void *ctx, const char *message)
{
	logmsg(ctx, "%s", message);
}

static void usage(void)
{
	printf("ctnd %s - read-only COSA database / critical_task_name daemon\n"
	       "usage: ctnd [-d DATABASE] [-r SECONDS] [-e] [-V|-h]\n"
	       "  -d, --db PATH      use exactly this absolute database path (no fallback)\n"
	       "  -r, --recheck SEC  default 30; minimum 5; 0 disables periodic DB checks\n"
	       "  -e, --enable-ctb   opt in to independent ct_enable updates; default off\n"
	       "                    original values are restored; external/HAL writes win\n"
	       "                    until the next game (no polling write fight)\n"
	       "  -V, --version     print version\n"
	       "  -h, --help        print this help\n"
	       "poll: 800ms; exit debounce: 5s; runtime: %s; no configuration file\n",
	       CTND_VERSION, CTND_RUNTIME_DIR);
}

int main(int argc, char **argv)
{
	struct app app;
	struct ctnd_state state;
	struct ctnd_journal journal;
	struct ctnd_ops ops;
	struct sigaction action;
	const char *database = NULL;
	uint64_t recheck = CTND_RECHECK_MS;
	bool manage = false, restored;
	int i, rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-V") || !strcmp(argv[i], "--version")) {
			printf("ctnd %s\n", CTND_VERSION);
			return 0;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage();
			return 0;
		} else if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "--enable-ctb")) manage = true;
		else if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--db")) {
			if (++i >= argc || argv[i][0] != '/' || !argv[i][1]) {
				fprintf(stderr, "--db requires an absolute database filename\n");
				return 2;
			}
			database = argv[i];
		} else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--recheck")) {
			char *end;
			unsigned long seconds;

			if (++i >= argc || argv[i][0] < '0' || argv[i][0] > '9') {
				fprintf(stderr, "--recheck requires nonnegative integer seconds\n");
				return 2;
			}
			errno = 0;
			seconds = strtoul(argv[i], &end, 10);
			if (errno || *end || seconds > 2147483647UL) {
				fprintf(stderr, "invalid --recheck value\n");
				return 2;
			}
			recheck = seconds ? (uint64_t)(seconds < 5 ? 5 : seconds) * 1000U : 0;
		} else {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			return 2;
		}
	}
	memset(&app, 0, sizeof(app));
	app.kmsg = -1;
	if (!ctnd_db_init(&app.db, CTND_RUNTIME_DIR, database)) return 2;
	memset(&action, 0, sizeof(action));
	action.sa_handler = on_signal;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL)) return 1;
	action.sa_handler = SIG_IGN;
	if (sigaction(SIGHUP, &action, NULL)) return 1;
	rc = ctnd_runtime_open(&app.runtime, CTND_RUNTIME_DIR, CTND_BOOT_ID_PATH);
	if (rc) {
		logmsg(&app, rc == CTND_ALREADY_RUNNING ? "another daemon holds the shared lock" :
		       "cannot safely open shared runtime/lock/pidfile");
		return rc;
	}
	if (!ctnd_db_reap_snapshots(app.runtime.dirfd, app.runtime.lockfd))
		logmsg(&app, "snapshot cleanup was incomplete; unknown files were preserved");
	app.kmsg = open(CTND_KMSG_PATH, O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (!ctnd_runtime_load(&app.runtime, &journal)) {
		logmsg(&app, "invalid/unreadable restore.state; refusing node writes");
		ctnd_runtime_close(&app.runtime);
		if (app.kmsg >= 0) close(app.kmsg);
		return 1;
	}
	ctnd_state_init(&state, manage, recheck, &journal);
	ops.ctx = &app;
	ops.read_node = read_node;
	ops.write_node = write_node;
	ops.save = save_state;
	ops.package = package;
	ops.lookup = lookup;
	ops.event = event;
	logmsg(&app, "version %s started; CTB %s, runtime %s", CTND_VERSION,
	       manage ? "opt-in" : "unmanaged", CTND_RUNTIME_DIR);
	while (!stopped) {
		char buf[256];
		size_t n;
		int pid = -1;
		bool valid = ctnd_read_path(CTND_GAMEPID_PATH, buf, sizeof(buf), &n) && n &&
			     !memchr(buf, '\0', n) && ctnd_game_pid(buf, &pid);
		struct timespec wait = {0, (long)CTND_POLL_MS * 1000000L};

		if (stopped) break;
		ctnd_state_tick(&state, &ops, valid, pid, ctnd_now_ms());
		while (!stopped && nanosleep(&wait, &wait) && errno == EINTR) { }
	}
	restored = ctnd_state_shutdown(&state, &ops);
	logmsg(&app, restored ? "stopped; original values restored" :
	       "stopped with restore debt; preserve runtime and retry before unloading the module");
	ctnd_db_close(&app.db);
	ctnd_runtime_close(&app.runtime);
	if (app.kmsg >= 0) close(app.kmsg);
	return restored ? 0 : 1;
}
