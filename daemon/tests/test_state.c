/* SPDX-License-Identifier: GPL-2.0-only */
#include "../ctnd.h"
#include "test_helper.h"
#include <string.h>

struct fake {
	char node[CTND_NODE_COUNT][CTND_NAMES_LEN];
	bool missing[CTND_NODE_COUNT];
	unsigned int writes[CTND_NODE_COUNT], attempts[CTND_NODE_COUNT];
	unsigned int write_fail[CTND_NODE_COUNT];
	unsigned int saves, save_fail_at, save_publish_fail_at;
	unsigned int lookups, package_fail;
	struct ctnd_cfg cfg;
	struct ctnd_journal disk;
};

static bool read_node(void *ctx, enum ctnd_node node, char *out, size_t cap)
{
	struct fake *f = ctx;

	if (f->missing[node] || strlen(f->node[node]) >= cap) return false;
	memcpy(out, f->node[node], strlen(f->node[node]) + 1);
	return true;
}

static bool write_node(void *ctx, enum ctnd_node node, const char *value)
{
	struct fake *f = ctx;

	f->attempts[node]++;
	if (f->missing[node]) return false;
	if (f->write_fail[node]) { f->write_fail[node]--; return false; }
	f->writes[node]++;
	memcpy(f->node[node], value, strlen(value) + 1);
	return true;
}

static bool save(void *ctx, const struct ctnd_journal *journal)
{
	struct fake *f = ctx;

	if (++f->saves == f->save_fail_at) return false;
	f->disk = *journal;
	/* Model rename succeeding before directory fsync fails. */
	return f->saves != f->save_publish_fail_at;
}

static bool package(void *ctx, int pid, char *out, size_t cap)
{
	struct fake *f = ctx;

	if (f->package_fail) { f->package_fail--; return false; }
	return snprintf(out, cap, "com.test.game%d", pid) > 0;
}

static enum ctnd_result lookup(void *ctx, const char *pkg, uint64_t now, struct ctnd_cfg *cfg)
{
	struct fake *f = ctx;

	(void)pkg;
	(void)now;
	f->lookups++;
	*cfg = f->cfg;
	return cfg->result;
}

static void config(struct fake *f, const char *raw)
{
	CHECK(ctnd_parse_cfg(raw, strlen(raw), &f->cfg) != CTND_ERROR);
}

static void setup(struct fake *f, struct ctnd_state *s, struct ctnd_ops *ops, bool manage)
{
	memset(f, 0, sizeof(*f));
	memset(ops, 0, sizeof(*ops));
	memcpy(f->node[0], "CustomOne CustomTwo", sizeof("CustomOne CustomTwo"));
	memcpy(f->node[1], "0", 2);
	config(f, "{\"ctn\":\"Game Render\",\"ctb\":1}");
	ops->ctx = f;
	ops->read_node = read_node;
	ops->write_node = write_node;
	ops->save = save;
	ops->package = package;
	ops->lookup = lookup;
	ctnd_state_init(s, manage, 5000, NULL);
}

static void applied_and_retried(void)
{
	struct fake f;
	struct ctnd_state s;
	struct ctnd_ops ops;

	setup(&f, &s, &ops, false);
	f.write_fail[0] = 1;
	ctnd_state_tick(&s, &ops, true, 1, 0);
	CHECK(!s.journal.nodes[0].applied_valid);
	CHECK(s.journal.nodes[0].owed && s.journal.nodes[0].pending_valid);
	CHECK(!strcmp(f.node[0], "CustomOne CustomTwo"));
	ctnd_state_tick(&s, &ops, true, 1, 5000);
	CHECK(s.journal.nodes[0].applied_valid && !strcmp(s.journal.nodes[0].applied, "Game Render"));
	CHECK(f.writes[0] == 1 && f.attempts[0] == 2);
	CHECK(f.writes[1] == 0);
	config(&f, "{\"ctn\":\"Changed Render\"}");
	f.write_fail[0] = 1;
	ctnd_state_tick(&s, &ops, true, 1, 10000);
	CHECK(!strcmp(s.journal.nodes[0].applied, "Game Render"));
	CHECK(!strcmp(f.node[0], "Game Render"));
	ctnd_state_tick(&s, &ops, true, 1, 15000);
	CHECK(!strcmp(s.journal.nodes[0].applied, "Changed Render"));
	CHECK(ctnd_state_shutdown(&s, &ops));
	CHECK(!strcmp(f.node[0], "CustomOne CustomTwo"));
	CHECK(!ctnd_state_has_debt(&s));
}

static void restore_debt(void)
{
	struct fake f;
	struct ctnd_state s;
	struct ctnd_ops ops;

	setup(&f, &s, &ops, false);
	ctnd_state_tick(&s, &ops, true, 1, 0);
	config(&f, "{\"ctn\":\"  \"}");
	f.write_fail[0] = 2;
	ctnd_state_tick(&s, &ops, true, 1, 5000);
	CHECK(ctnd_state_has_debt(&s) && s.journal.nodes[0].applied_valid);
	ctnd_state_tick(&s, &ops, true, 1, 10000);
	CHECK(ctnd_state_has_debt(&s));
	ctnd_state_tick(&s, &ops, true, 1, 15000);
	CHECK(!ctnd_state_has_debt(&s));
	CHECK(!strcmp(f.node[0], "CustomOne CustomTwo"));
	setup(&f, &s, &ops, false);
	ctnd_state_tick(&s, &ops, true, 1, 0);
	f.write_fail[0] = 1;
	ctnd_state_tick(&s, &ops, true, -1, 800);
	ctnd_state_tick(&s, &ops, true, -1, 5800);
	CHECK(s.pid == -1 && ctnd_state_has_debt(&s));
	ctnd_state_tick(&s, &ops, true, -1, 10800);
	CHECK(!ctnd_state_has_debt(&s));
	CHECK(!strcmp(f.node[0], "CustomOne CustomTwo"));
}

static void pid_debounce(void)
{
	struct fake f;
	struct ctnd_state s;
	struct ctnd_ops ops;

	setup(&f, &s, &ops, false);
	ctnd_state_tick(&s, &ops, true, 1, 0);
	ctnd_state_tick(&s, &ops, true, -1, 1000);
	ctnd_state_tick(&s, &ops, true, 1, 4999);
	CHECK(!s.gone);
	ctnd_state_tick(&s, &ops, true, -1, 5000);
	ctnd_state_tick(&s, &ops, true, -1, 9999);
	CHECK(s.pid == 1 && f.writes[0] == 1);
	ctnd_state_tick(&s, &ops, false, -1, 10000);
	CHECK(!s.gone && s.pid == 1);
	ctnd_state_tick(&s, &ops, true, -1, 11000);
	ctnd_state_tick(&s, &ops, true, -1, 15999);
	CHECK(s.pid == 1);
	ctnd_state_tick(&s, &ops, true, -1, 16000);
	CHECK(s.pid == -1 && f.writes[0] == 2);
}

static void errors_and_switches(void)
{
	struct fake f;
	struct ctnd_state s;
	struct ctnd_ops ops;

	setup(&f, &s, &ops, false);
	ctnd_state_tick(&s, &ops, true, 1, 0);
	f.cfg.result = CTND_ERROR;
	ctnd_state_tick(&s, &ops, true, 1, 5000);
	CHECK(f.writes[0] == 1 && ctnd_state_has_debt(&s));
	ctnd_state_tick(&s, &ops, true, 2, 6000);
	CHECK(f.writes[0] == 1 && ctnd_state_has_debt(&s));
	f.cfg.result = CTND_UNSUPPORTED;
	ctnd_state_tick(&s, &ops, true, 2, 11000);
	CHECK(f.writes[0] == 1 && ctnd_state_has_debt(&s));
	f.cfg.result = CTND_MISSING;
	f.cfg.have_ctb = false;
	ctnd_state_tick(&s, &ops, true, 2, 16000);
	CHECK(f.writes[0] == 2 && !ctnd_state_has_debt(&s));
	setup(&f, &s, &ops, false);
	f.cfg.result = CTND_UNSUPPORTED;
	ctnd_state_tick(&s, &ops, true, 1, 0);
	CHECK(f.writes[0] == 0 && f.saves == 0);
	CHECK(ctnd_state_shutdown(&s, &ops));
}

static void late_nodes_and_package(void)
{
	struct fake f;
	struct ctnd_state s;
	struct ctnd_ops ops;

	setup(&f, &s, &ops, false);
	s.recheck_ms = 0;
	f.missing[0] = true;
	f.package_fail = 1;
	ctnd_state_tick(&s, &ops, true, 1, 0);
	CHECK(!s.package[0] && f.lookups == 0);
	ctnd_state_tick(&s, &ops, true, 1, 5000);
	CHECK(f.lookups == 1 && f.saves == 0);
	memcpy(f.node[0], "LateOriginal One", sizeof("LateOriginal One"));
	f.missing[0] = false;
	ctnd_state_tick(&s, &ops, true, 1, 10000);
	CHECK(f.lookups == 1 && f.writes[0] == 1);
	CHECK(ctnd_state_shutdown(&s, &ops));
	CHECK(!strcmp(f.node[0], "LateOriginal One"));
}

static void ctb_and_ownership(void)
{
	struct fake f;
	struct ctnd_state s;
	struct ctnd_ops ops;

	setup(&f, &s, &ops, true);
	ctnd_state_tick(&s, &ops, true, 1, 0);
	CHECK(f.writes[0] == 1 && f.writes[1] == 1);
	config(&f, "{\"ctn\":\"Game Render\",\"ctb\":0}");
	ctnd_state_tick(&s, &ops, true, 1, 5000);
	CHECK(f.writes[0] == 1 && f.writes[1] == 2 && !strcmp(f.node[1], "0"));
	config(&f, "{\"ctn\":\"Game Render\",\"ctb\":1}");
	f.write_fail[1] = 1;
	ctnd_state_tick(&s, &ops, true, 1, 10000);
	CHECK(f.writes[0] == 1 && f.writes[1] == 2);
	ctnd_state_tick(&s, &ops, true, 1, 15000);
	CHECK(f.writes[0] == 1 && f.writes[1] == 3);
	memcpy(f.node[1], "0", 2); /* HAL wins. */
	ctnd_state_tick(&s, &ops, true, 1, 20000);
	CHECK(f.writes[1] == 3 && s.yielded[1] && !s.journal.nodes[1].owed);
	ctnd_state_tick(&s, &ops, true, 1, 25000);
	CHECK(f.writes[1] == 3);
	CHECK(ctnd_state_shutdown(&s, &ops));
	CHECK(f.writes[1] == 3 && !strcmp(f.node[1], "0"));
	setup(&f, &s, &ops, true);
	config(&f, "{\"ctb\":1}");
	ctnd_state_tick(&s, &ops, true, 1, 0);
	CHECK(f.writes[0] == 0 && f.writes[1] == 1);
	CHECK(ctnd_state_shutdown(&s, &ops));
	CHECK(f.writes[0] == 0 && f.writes[1] == 2 && !strcmp(f.node[1], "0"));
}

static void staged_save_shutdown(bool fail_shutdown)
{
	struct fake f;
	struct ctnd_state s;
	struct ctnd_ops ops;
	struct ctnd_journal authoritative;

	setup(&f, &s, &ops, false);
	authoritative = s.journal;
	f.save_publish_fail_at = 1;
	ctnd_state_tick(&s, &ops, true, 1, 0);
	CHECK(f.disk.nodes[0].owed && f.disk.nodes[0].pending_valid && !f.disk.nodes[0].applied_valid);
	CHECK(!strcmp(f.disk.nodes[0].original, "CustomOne CustomTwo"));
	CHECK(!strcmp(f.disk.nodes[0].pending, "Game Render") && !f.disk.nodes[1].owed);
	CHECK(!memcmp(&s.journal, &authoritative, sizeof(authoritative)));
	CHECK(f.attempts[0] == 0 && f.attempts[1] == 0 && f.writes[0] == 0 && f.writes[1] == 0);
	if (fail_shutdown) f.save_fail_at = 2;
	CHECK(ctnd_state_shutdown(&s, &ops) == !fail_shutdown);
	if (fail_shutdown) {
		CHECK(s.journal_dirty && ctnd_state_has_debt(&s));
		CHECK(f.saves == 2 && f.disk.nodes[0].owed && f.disk.nodes[0].pending_valid);
		CHECK(!memcmp(&s.journal, &authoritative, sizeof(authoritative)));
		CHECK(f.attempts[0] == 0 && f.attempts[1] == 0);
		CHECK(ctnd_state_shutdown(&s, &ops));
	}
	CHECK(!f.disk.nodes[0].owed && !f.disk.nodes[1].owed);
	CHECK(f.saves == (fail_shutdown ? 3U : 2U));
	CHECK(!s.journal_dirty && !ctnd_state_has_debt(&s));
	CHECK(f.attempts[0] == 0 && f.attempts[1] == 0 && f.writes[0] == 0 && f.writes[1] == 0);
	CHECK(!strcmp(f.node[0], "CustomOne CustomTwo") && !strcmp(f.node[1], "0"));
}

static void staged_save_retry(void)
{
	struct fake f;
	struct ctnd_state s;
	struct ctnd_ops ops;
	struct ctnd_journal authoritative;

	setup(&f, &s, &ops, true);
	authoritative = s.journal;
	f.save_publish_fail_at = 1;
	ctnd_state_tick(&s, &ops, true, 1, 0);
	CHECK(f.attempts[0] == 0 && f.attempts[1] == 0);
	CHECK(f.disk.nodes[0].pending_valid);
	CHECK(!memcmp(&s.journal, &authoritative, sizeof(authoritative)));
	CHECK(s.journal_dirty && ctnd_state_has_debt(&s));
	f.save_fail_at = 2; /* Rollback must succeed before either node can be applied. */
	ctnd_state_tick(&s, &ops, true, 1, 5000);
	CHECK(f.saves == 2 && s.journal_dirty && f.disk.nodes[0].pending_valid);
	CHECK(f.attempts[0] == 0 && f.attempts[1] == 0);
	f.save_fail_at = 4; /* Save 3 rolls back; reject the next staged intent. */
	ctnd_state_tick(&s, &ops, true, 1, 10000);
	CHECK(!f.disk.nodes[0].owed && !f.disk.nodes[1].owed);
	CHECK(f.saves == 4 && s.journal_dirty && ctnd_state_has_debt(&s));
	CHECK(!memcmp(&s.journal, &authoritative, sizeof(authoritative)));
	CHECK(f.attempts[0] == 0 && f.attempts[1] == 0);
	ctnd_state_tick(&s, &ops, true, 1, 15000);
	CHECK(f.saves == 9 && !s.journal_dirty);
	CHECK(f.attempts[0] == 1 && f.attempts[1] == 1 && f.writes[0] == 1 && f.writes[1] == 1);
	CHECK(s.journal.nodes[0].applied_valid && !s.journal.nodes[0].pending_valid);
	CHECK(s.journal.nodes[1].applied_valid && !s.journal.nodes[1].pending_valid);
	CHECK(!strcmp(s.journal.nodes[0].original, "CustomOne CustomTwo"));
	CHECK(!strcmp(s.journal.nodes[1].original, "0"));
	CHECK(ctnd_state_shutdown(&s, &ops));
	CHECK(!ctnd_state_has_debt(&s) && !f.disk.nodes[0].owed && !f.disk.nodes[1].owed);
}

static void staged_save_applied(void)
{
	struct fake f;
	struct ctnd_state s;
	struct ctnd_ops ops;
	struct ctnd_journal authoritative;

	setup(&f, &s, &ops, false);
	ctnd_state_tick(&s, &ops, true, 1, 0);
	authoritative = s.journal;
	config(&f, "{\"ctn\":\"Changed Render\"}");
	f.save_publish_fail_at = 3;
	ctnd_state_tick(&s, &ops, true, 1, 5000);
	CHECK(f.disk.nodes[0].applied_valid && f.disk.nodes[0].pending_valid);
	CHECK(!strcmp(f.disk.nodes[0].applied, "Game Render"));
	CHECK(!strcmp(f.disk.nodes[0].pending, "Changed Render"));
	CHECK(!memcmp(&s.journal, &authoritative, sizeof(authoritative)));
	CHECK(f.attempts[0] == 1 && f.writes[0] == 1 && !strcmp(f.node[0], "Game Render"));
	CHECK(s.journal_dirty && ctnd_state_has_debt(&s));
	f.save_fail_at = 4;
	CHECK(!ctnd_state_shutdown(&s, &ops));
	CHECK(f.attempts[0] == 1 && f.disk.nodes[0].pending_valid && s.journal_dirty);
	CHECK(!memcmp(&s.journal, &authoritative, sizeof(authoritative)));
	CHECK(ctnd_state_shutdown(&s, &ops));
	CHECK(f.saves == 6 && f.attempts[0] == 2 && f.writes[0] == 2);
	CHECK(!strcmp(f.node[0], "CustomOne CustomTwo"));
	CHECK(!ctnd_state_has_debt(&s) && !f.disk.nodes[0].owed);
}

static void durable_and_restart(void)
{
	struct fake f;
	struct ctnd_state s, restart;
	struct ctnd_ops ops;
	struct ctnd_journal crash;

	setup(&f, &s, &ops, false);
	f.save_fail_at = 1;
	ctnd_state_tick(&s, &ops, true, 1, 0);
	CHECK(f.writes[0] == 0 && s.journal_dirty && ctnd_state_has_debt(&s));
	CHECK(!s.journal.nodes[0].owed && !f.disk.nodes[0].owed);
	ctnd_state_tick(&s, &ops, true, 1, 5000);
	CHECK(f.writes[0] == 1 && ctnd_state_has_debt(&s));
	f.write_fail[0] = 1;
	CHECK(!ctnd_state_shutdown(&s, &ops));
	CHECK(f.disk.nodes[0].owed);
	crash = f.disk;
	ctnd_state_init(&restart, false, 5000, &crash);
	f.cfg.result = CTND_ERROR;
	ctnd_state_tick(&restart, &ops, true, 2, 10000);
	CHECK(!strcmp(f.node[0], "CustomOne CustomTwo"));
	CHECK(!ctnd_state_has_debt(&restart) && !f.disk.nodes[0].owed);

	setup(&f, &s, &ops, false);
	f.save_fail_at = 2; /* Crash window: intent persisted, node written, commit failed. */
	ctnd_state_tick(&s, &ops, true, 1, 0);
	CHECK(s.journal_dirty && f.disk.nodes[0].pending_valid && !f.disk.nodes[0].applied_valid);
	crash = f.disk;
	ctnd_state_init(&restart, false, 5000, &crash);
	CHECK(ctnd_state_shutdown(&restart, &ops));
	CHECK(!strcmp(f.node[0], "CustomOne CustomTwo"));

	setup(&f, &s, &ops, false);
	ctnd_state_tick(&s, &ops, true, 1, 0);
	memcpy(f.node[0], "External Names", sizeof("External Names"));
	crash = f.disk;
	ctnd_state_init(&restart, false, 5000, &crash);
	CHECK(ctnd_state_shutdown(&restart, &ops));
	CHECK(!strcmp(f.node[0], "External Names") && f.writes[0] == 1);
}

static void recovery_yield(void)
{
	struct fake f;
	struct ctnd_state s, restart;
	struct ctnd_ops ops;
	struct ctnd_journal crash;

	setup(&f, &s, &ops, false);
	ctnd_state_tick(&s, &ops, true, 1, 0);
	crash = f.disk;
	memcpy(f.node[0], "HAL New", sizeof("HAL New"));
	ctnd_state_init(&restart, false, 5000, &crash);
	f.save_fail_at = f.saves + 1;
	ctnd_state_tick(&restart, &ops, true, 1, 10000);
	CHECK(restart.recovering && restart.journal_dirty && restart.yielded[0]);
	CHECK(f.disk.nodes[0].owed && f.writes[0] == 1);
	ctnd_state_tick(&restart, &ops, false, -1, 15000);
	CHECK(!restart.recovering && restart.yielded[0] && !ctnd_state_has_debt(&restart));
	CHECK(!f.disk.nodes[0].owed && f.writes[0] == 1);
	ctnd_state_tick(&restart, &ops, true, -1, 16000);
	ctnd_state_tick(&restart, &ops, true, 1, 20000);
	CHECK(restart.pid == 1 && restart.yielded[0] && !restart.initial_session);
	CHECK(!strcmp(f.node[0], "HAL New") && f.writes[0] == 1);
	ctnd_state_tick(&restart, &ops, true, 1, 25000);
	CHECK(restart.yielded[0] && f.writes[0] == 1);
	ctnd_state_tick(&restart, &ops, true, -1, 26000);
	ctnd_state_tick(&restart, &ops, true, 1, 30000);
	CHECK(restart.yielded[0] && f.writes[0] == 1);
	ctnd_state_tick(&restart, &ops, true, 2, 31000);
	CHECK(!restart.yielded[0] && f.writes[0] == 2);
	CHECK(!strcmp(restart.journal.nodes[0].original, "HAL New"));
	CHECK(ctnd_state_shutdown(&restart, &ops));

	setup(&f, &s, &ops, false);
	ctnd_state_tick(&s, &ops, true, 1, 0);
	crash = f.disk;
	memcpy(f.node[0], "HAL Idle", sizeof("HAL Idle"));
	ctnd_state_init(&restart, false, 5000, &crash);
	ctnd_state_tick(&restart, &ops, true, -1, 10000);
	CHECK(restart.yielded[0] && restart.initial_session && !ctnd_state_has_debt(&restart));
	ctnd_state_tick(&restart, &ops, false, -1, 14000);
	ctnd_state_tick(&restart, &ops, true, -1, 15000);
	ctnd_state_tick(&restart, &ops, true, -1, 19999);
	CHECK(restart.yielded[0] && restart.initial_session);
	ctnd_state_tick(&restart, &ops, true, -1, 20000);
	CHECK(!restart.yielded[0] && !restart.initial_session);
	ctnd_state_tick(&restart, &ops, true, 2, 21000);
	CHECK(f.writes[0] == 2 && !strcmp(f.node[0], "Game Render"));
	CHECK(ctnd_state_shutdown(&restart, &ops));
}

static void recovery_original(enum ctnd_node node, bool pending)
{
	struct fake f;
	struct ctnd_state s, restart;
	struct ctnd_ops ops;
	struct ctnd_journal crash;
	unsigned int attempts, writes;

	setup(&f, &s, &ops, node == CTND_NODE_ENABLE);
	if (node == CTND_NODE_ENABLE) config(&f, "{\"ctb\":1}");
	ctnd_state_tick(&s, &ops, true, 1, 0);
	if (pending) {
		config(&f, "{\"ctn\":\"Changed Render\"}");
		f.write_fail[node] = 1;
		ctnd_state_tick(&s, &ops, true, 1, 5000);
	}
	crash = f.disk;
	CHECK(crash.nodes[node].owed && crash.nodes[node].applied_valid);
	CHECK(crash.nodes[node].pending_valid == pending);
	attempts = f.attempts[node];
	writes = f.writes[node];
	/* HAL restored the original after the daemon's applied journal survived a crash. */
	memcpy(f.node[node], crash.nodes[node].original, strlen(crash.nodes[node].original) + 1);
	ctnd_state_init(&restart, node == CTND_NODE_ENABLE, 5000, &crash);
	ctnd_state_tick(&restart, &ops, true, 1, 10000);
	CHECK(f.attempts[node] == attempts && f.writes[node] == writes);
	CHECK(!restart.recovering && restart.yielded[node] && !ctnd_state_has_debt(&restart));
	CHECK(restart.pid == 1 && !restart.initial_session && !f.disk.nodes[node].owed);
	CHECK(!strcmp(f.node[node], crash.nodes[node].original));
	ctnd_state_tick(&restart, &ops, true, 1, 15000);
	CHECK(restart.yielded[node] && f.attempts[node] == attempts && f.writes[node] == writes);
	ctnd_state_tick(&restart, &ops, true, -1, 16000);
	ctnd_state_tick(&restart, &ops, true, 1, 20000);
	CHECK(restart.yielded[node] && f.attempts[node] == attempts && f.writes[node] == writes);
	ctnd_state_tick(&restart, &ops, true, 2, 21000);
	CHECK(!restart.yielded[node] && f.attempts[node] == attempts + 1 && f.writes[node] == writes + 1);
	CHECK(restart.journal.nodes[node].applied_valid && !restart.journal.nodes[node].pending_valid);
	CHECK(!strcmp(restart.journal.nodes[node].original, crash.nodes[node].original));
	CHECK(!strcmp(f.node[node], node == CTND_NODE_ENABLE ? "1" : pending ? "Changed Render" : "Game Render"));
	CHECK(ctnd_state_shutdown(&restart, &ops));
	CHECK(!ctnd_state_has_debt(&restart) && !f.disk.nodes[node].owed);
	CHECK(!strcmp(f.node[node], crash.nodes[node].original));
}

static void recovery_pending_original(enum ctnd_node node)
{
	struct fake f;
	struct ctnd_state s, restart;
	struct ctnd_ops ops;
	struct ctnd_journal crash;

	setup(&f, &s, &ops, node == CTND_NODE_ENABLE);
	if (node == CTND_NODE_ENABLE) config(&f, "{\"ctb\":1}");
	f.write_fail[node] = 1;
	ctnd_state_tick(&s, &ops, true, 1, 0);
	crash = f.disk;
	CHECK(crash.nodes[node].owed && crash.nodes[node].pending_valid && !crash.nodes[node].applied_valid);
	CHECK(!strcmp(f.node[node], crash.nodes[node].original) && f.writes[node] == 0);
	ctnd_state_init(&restart, node == CTND_NODE_ENABLE, 5000, &crash);
	ctnd_state_tick(&restart, &ops, true, 1, 10000);
	CHECK(!restart.recovering && !restart.yielded[node] && restart.pid == 1);
	CHECK(f.attempts[node] == 2 && f.writes[node] == 1);
	CHECK(restart.journal.nodes[node].applied_valid && !restart.journal.nodes[node].pending_valid);
	CHECK(!strcmp(restart.journal.nodes[node].original, crash.nodes[node].original));
	CHECK(!strcmp(f.node[node], node == CTND_NODE_ENABLE ? "1" : "Game Render"));
	ctnd_state_tick(&restart, &ops, true, 1, 15000);
	CHECK(!restart.yielded[node] && f.attempts[node] == 2 && f.writes[node] == 1);
	CHECK(ctnd_state_shutdown(&restart, &ops));
	CHECK(!ctnd_state_has_debt(&restart) && !f.disk.nodes[node].owed);
	CHECK(!strcmp(f.node[node], crash.nodes[node].original));
}

static void game_observations(void)
{
	struct fake f;
	struct ctnd_state s;
	struct ctnd_ops ops;

	setup(&f, &s, &ops, true);
	ctnd_state_tick(&s, &ops, true, 1, 0);
	ctnd_state_tick(&s, &ops, true, -1, 1000);
	ctnd_state_tick(&s, &ops, true, -1, 6000);
	CHECK(!ctnd_state_has_debt(&s) && s.observed_valid[0] && s.observed_valid[1]);
	memcpy(f.node[0], "HAL Idle", sizeof("HAL Idle"));
	memcpy(f.node[1], "1", 2);
	config(&f, "{\"ctn\":\"Next Render\",\"ctb\":0}");
	ctnd_state_tick(&s, &ops, true, 2, 7000);
	CHECK(!s.yielded[0] && !s.yielded[1] && f.writes[0] == 3 && f.writes[1] == 3);
	CHECK(!strcmp(f.node[0], "Next Render") && !strcmp(f.node[1], "0"));
	CHECK(!strcmp(s.journal.nodes[0].original, "HAL Idle"));
	CHECK(!strcmp(s.journal.nodes[1].original, "1"));
	CHECK(ctnd_state_shutdown(&s, &ops));
	CHECK(!strcmp(f.node[0], "HAL Idle") && !strcmp(f.node[1], "1"));

	setup(&f, &s, &ops, false);
	memcpy(f.node[0], "Game Render", sizeof("Game Render"));
	ctnd_state_tick(&s, &ops, true, 1, 0);
	CHECK(s.observed_valid[0] && f.writes[0] == 0);
	memcpy(f.node[0], "HAL Next", sizeof("HAL Next"));
	config(&f, "{\"ctn\":\"Next Render\"}");
	ctnd_state_tick(&s, &ops, true, 2, 1000);
	CHECK(!s.yielded[0] && !s.observed_valid[0] && f.writes[0] == 1);
	CHECK(!strcmp(s.journal.nodes[0].original, "HAL Next"));
	CHECK(ctnd_state_shutdown(&s, &ops));

	setup(&f, &s, &ops, false);
	ctnd_state_tick(&s, &ops, true, 1, 0);
	config(&f, "{\"ctn\":\"Next Render\"}");
	ctnd_state_tick(&s, &ops, true, 2, 1000);
	CHECK(f.writes[0] == 2 && s.journal.nodes[0].owed);
	CHECK(!strcmp(s.journal.nodes[0].original, "CustomOne CustomTwo"));
	memcpy(f.node[0], "HAL Owned", sizeof("HAL Owned"));
	ctnd_state_tick(&s, &ops, true, 3, 2000);
	CHECK(s.yielded[0] && !ctnd_state_has_debt(&s) && f.writes[0] == 2);
	CHECK(!strcmp(f.node[0], "HAL Owned"));

	setup(&f, &s, &ops, false);
	f.write_fail[0] = 1;
	ctnd_state_tick(&s, &ops, true, 1, 0);
	CHECK(s.journal.nodes[0].pending_valid);
	memcpy(f.node[0], "HAL Pending", sizeof("HAL Pending"));
	ctnd_state_tick(&s, &ops, true, 2, 1000);
	CHECK(s.yielded[0] && !ctnd_state_has_debt(&s) && f.writes[0] == 0);
	CHECK(!strcmp(f.node[0], "HAL Pending"));
}

int main(void)
{
	staged_save_shutdown(false);
	staged_save_shutdown(true);
	staged_save_retry();
	staged_save_applied();
	recovery_original(CTND_NODE_NAMES, false);
	recovery_original(CTND_NODE_NAMES, true);
	recovery_original(CTND_NODE_ENABLE, false);
	recovery_pending_original(CTND_NODE_NAMES);
	recovery_pending_original(CTND_NODE_ENABLE);
	applied_and_retried();
	restore_debt();
	pid_debounce();
	errors_and_switches();
	late_nodes_and_package();
	ctb_and_ownership();
	durable_and_restart();
	recovery_yield();
	game_observations();
	printf("PASS state: %u checks\n", test_count);
	return 0;
}
