/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "ctnd.h"
#include "ctnd_db.h"
#include "ctnd_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct harness {
	struct ctnd_db db;
	struct ctnd_runtime runtime;
};

static int failures;
static unsigned int checks;

static void check(bool ok, const char *message)
{
	checks++;
	printf("%s %s\n", ok ? "PASS" : "FAIL", message);
	fflush(stdout);
	if (!ok) failures++;
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
	struct harness *h = ctx;
	return ctnd_runtime_save(&h->runtime, journal);
}

static bool package_name(void *ctx, int pid, char *out, size_t cap)
{
	(void)ctx;
	if (pid <= 0 || cap <= strlen("com.ctnp.synthetic")) return false;
	strcpy(out, "com.ctnp.synthetic");
	return true;
}

static enum ctnd_result lookup(void *ctx, const char *pkg, uint64_t now,
				struct ctnd_cfg *cfg)
{
	struct harness *h = ctx;
	return ctnd_db_lookup(&h->db, pkg, now, cfg);
}

static void set_fixture(struct harness *h, const char *directory, const char *name)
{
	char path[4096];
	ctnd_db_close(&h->db);
	if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int)sizeof(path) ||
	    !ctnd_db_init(&h->db, CTND_RUNTIME_DIR, path)) {
		fprintf(stderr, "fixture setup failed\n");
		exit(2);
	}
}

static void expect_names(const char *expected, const char *message)
{
	char value[CTND_NAMES_LEN];
	check(ctnd_runtime_read_node(CTND_NODE_NAMES, value, sizeof(value)) &&
	      !strcmp(value, expected), message);
}

int main(int argc, char **argv)
{
	struct harness h;
	struct ctnd_state state;
	struct ctnd_journal journal;
	struct ctnd_ops ops;
	char original[CTND_NAMES_LEN], enable[CTND_NAMES_LEN], after[CTND_NAMES_LEN];
	bool restored;

	if (argc != 2) return 2;
	memset(&h, 0, sizeof(h));
	if (ctnd_runtime_open(&h.runtime, CTND_RUNTIME_DIR, CTND_BOOT_ID_PATH)) return 2;
	if (!ctnd_runtime_load(&h.runtime, &journal) ||
	    !ctnd_runtime_read_node(CTND_NODE_NAMES, original, sizeof(original)) ||
	    !ctnd_runtime_read_node(CTND_NODE_ENABLE, enable, sizeof(enable))) {
		ctnd_runtime_close(&h.runtime);
		return 2;
	}
	for (size_t i = 0; i < CTND_NODE_COUNT; i++)
		if (journal.nodes[i].owed) { ctnd_runtime_close(&h.runtime); return 2; }
	set_fixture(&h, argv[1], "valid.db");
	ops = (struct ctnd_ops){ .ctx = &h, .read_node = read_node,
		.write_node = write_node, .save = save_state,
		.package = package_name, .lookup = lookup };
	ctnd_state_init(&state, false, 5000, &journal);
	ctnd_state_tick(&state, &ops, true, 100001, 10000);
	expect_names("CTNMain CTNRender", "SQLite fixture applies to actual CTN node");
	check(ctnd_state_has_debt(&state), "original value persisted before node write");
	check(ctnd_runtime_read_node(CTND_NODE_ENABLE, after, sizeof(after)) &&
	      !strcmp(enable, after), "default leaves ct_enable unchanged");

	set_fixture(&h, argv[1], "broken.db");
	ctnd_state_tick(&state, &ops, true, 100001, 20000);
	expect_names("CTNMain CTNRender", "SQLite error retains applied names");
	set_fixture(&h, argv[1], "unsupported.db");
	ctnd_state_tick(&state, &ops, true, 100001, 30000);
	expect_names("CTNMain CTNRender", "unknown config is not interpreted as deletion");
	set_fixture(&h, argv[1], "changed.db");
	ctnd_state_tick(&state, &ops, true, 100001, 40000);
	expect_names("CTNNew CTNNewGfx", "changed CTN reapplies while PID is unchanged");
	set_fixture(&h, argv[1], "empty.db");
	ctnd_state_tick(&state, &ops, true, 100001, 50000);
	expect_names(original, "empty CTN restores original names");

	set_fixture(&h, argv[1], "valid.db");
	ctnd_state_tick(&state, &ops, true, 100002, 60000);
	expect_names("CTNMain CTNRender", "next game applies valid CTN");
	set_fixture(&h, argv[1], "space.db");
	ctnd_state_tick(&state, &ops, true, 100002, 70000);
	expect_names(original, "whitespace CTN restores original names");

	set_fixture(&h, argv[1], "valid.db");
	ctnd_state_tick(&state, &ops, true, 100003, 80000);
	expect_names("CTNMain CTNRender", "reapply before direct switch");
	set_fixture(&h, argv[1], "missing.db");
	ctnd_state_tick(&state, &ops, true, 100004, 81000);
	expect_names(original, "direct switch to no-row game restores original names");

	set_fixture(&h, argv[1], "valid.db");
	ctnd_state_tick(&state, &ops, true, 100005, 90000);
	ctnd_state_tick(&state, &ops, true, -1, 91000);
	ctnd_state_tick(&state, &ops, true, 100005, 92000);
	ctnd_state_tick(&state, &ops, true, -1, 93000);
	ctnd_state_tick(&state, &ops, true, -1, 97000);
	expect_names("CTNMain CTNRender", "positive PID resets exit debounce");
	ctnd_state_tick(&state, &ops, true, -1, 98000);
	expect_names(original, "continuous negative PID restores after 5 seconds");

	ctnd_state_tick(&state, &ops, true, 100006, 110000);
	expect_names("CTNMain CTNRender", "reapply before graceful shutdown");
	restored = ctnd_state_shutdown(&state, &ops);
	check(restored, "shutdown repays actual node restoration debt");
	expect_names(original, "shutdown preserves original CTN value");
	check(ctnd_runtime_read_node(CTND_NODE_ENABLE, after, sizeof(after)) &&
	      !strcmp(enable, after), "ct_enable still unchanged after all tests");
	ctnd_db_close(&h.db);
	ctnd_runtime_close(&h.runtime);
	printf("RESULT checks=%u failures=%d restored=%d\n", checks, failures, restored);
	return failures || !restored ? 1 : 0;
}
