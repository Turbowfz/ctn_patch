/* SPDX-License-Identifier: GPL-2.0-only */
#include "ctnd.h"

#include <stdio.h>
#include <string.h>

static void event(const struct ctnd_ops *ops, const char *text)
{
	if (ops->event) ops->event(ops->ctx, text);
}

static bool save(struct ctnd_state *s, const struct ctnd_ops *ops)
{
	s->journal_dirty = !ops->save(ops->ctx, &s->journal);
	if (s->journal_dirty) event(ops, "restore journal could not be saved; retaining debt");
	return !s->journal_dirty;
}

bool ctnd_state_has_debt(const struct ctnd_state *s)
{
	size_t i;

	for (i = 0; i < CTND_NODE_COUNT; i++)
		if (s->journal.nodes[i].owed) return true;
	return s->journal_dirty;
}

void ctnd_state_init(struct ctnd_state *s, bool manage_ctb, uint64_t recheck_ms,
		     const struct ctnd_journal *journal)
{
	memset(s, 0, sizeof(*s));
	s->pid = -1;
	s->initial_session = true;
	s->manage_ctb = manage_ctb;
	s->recheck_ms = recheck_ms;
	if (journal) s->journal = *journal;
	s->recovering = ctnd_state_has_debt(s);
}

static bool is_ours(const struct ctnd_debt *d, const char *current)
{
	return (d->applied_valid && !strcmp(current, d->applied)) ||
	       (d->pending_valid && !strcmp(current, d->pending));
}

static bool clear_debt(struct ctnd_state *s, const struct ctnd_ops *ops, enum ctnd_node node)
{
	memset(&s->journal.nodes[node], 0, sizeof(s->journal.nodes[node]));
	return save(s, ops);
}

static bool restore(struct ctnd_state *s, const struct ctnd_ops *ops, enum ctnd_node node)
{
	struct ctnd_debt *d = &s->journal.nodes[node];
	char current[CTND_NAMES_LEN];

	if (!d->owed) return true;
	if (!ops->read_node(ops->ctx, node, current, sizeof(current))) return false;
	if (!is_ours(d, current) && (d->applied_valid || strcmp(current, d->original))) {
		/* HAL/user ownership wins, even at original after a previous apply. */
		s->yielded[node] = true;
		event(ops, "node changed externally; relinquishing ownership without a write");
		return clear_debt(s, ops, node);
	}
	if (!strcmp(current, d->original)) return clear_debt(s, ops, node);
	if (!ops->write_node(ops->ctx, node, d->original)) {
		event(ops, "node restore failed; debt will be retried");
		return false;
	}
	memcpy(s->observed[node], d->original, strlen(d->original) + 1);
	s->observed_valid[node] = true;
	event(ops, node == CTND_NODE_NAMES ? "critical_task_name restored" : "ct_enable restored");
	return clear_debt(s, ops, node);
}

static bool restore_all(struct ctnd_state *s, const struct ctnd_ops *ops)
{
	bool ok = true;
	int i;

	if (s->journal_dirty && !save(s, ops)) return false;
	for (i = 0; i < CTND_NODE_COUNT; i++) {
		if (s->journal_dirty) return false;
		if (!restore(s, ops, (enum ctnd_node)i)) ok = false;
	}
	return ok && !ctnd_state_has_debt(s);
}

bool ctnd_state_shutdown(struct ctnd_state *s, const struct ctnd_ops *ops)
{
	return restore_all(s, ops);
}

static bool apply(struct ctnd_state *s, const struct ctnd_ops *ops,
		  enum ctnd_node node, const char *want)
{
	struct ctnd_debt *d = &s->journal.nodes[node];
	struct ctnd_journal staged;
	struct ctnd_debt *next;
	char current[CTND_NAMES_LEN], message[256];

	if (s->yielded[node]) return true;
	if (!ops->read_node(ops->ctx, node, current, sizeof(current))) return false;
	if (!d->owed && s->observed_valid[node] && strcmp(current, s->observed[node])) {
		s->yielded[node] = true;
		event(ops, "previously matching node changed externally; yielding until the next game");
		return true;
	}
	if (d->owed && !is_ours(d, current) &&
	    (d->applied_valid || strcmp(current, d->original))) {
		s->yielded[node] = true;
		event(ops, "node changed externally; yielding until the next game");
		return clear_debt(s, ops, node);
	}
	if (!strcmp(current, want) && (!d->owed || (d->applied_valid && !d->pending_valid))) {
		memcpy(s->observed[node], current, strlen(current) + 1);
		s->observed_valid[node] = true;
		return true;
	}
	if (d->owed && !strcmp(want, d->original)) return restore(s, ops, node);

	staged = s->journal;
	next = &staged.nodes[node];
	if (!next->owed) {
		memset(next, 0, sizeof(*next));
		memcpy(next->original, current, strlen(current) + 1);
		next->owed = true;
	}
	memcpy(next->pending, want, strlen(want) + 1);
	next->pending_valid = true;
	/* Write-ahead intent covers a crash between the syscall and the commit. */
	if (!ops->save(ops->ctx, &staged)) {
		/* A failed save may have published staged; re-persist our unchanged journal. */
		s->journal_dirty = true;
		event(ops, "cannot persist original node value; refusing to write");
		return false;
	}
	s->journal = staged;
	d = &s->journal.nodes[node];
	if (!ops->write_node(ops->ctx, node, want)) {
		event(ops, "node write failed; applied value unchanged, retry scheduled");
		return false;
	}
	memcpy(d->applied, want, strlen(want) + 1);
	d->applied_valid = true;
	d->pending_valid = false;
	d->pending[0] = '\0';
	(void)snprintf(message, sizeof(message), "%s applied [%s]",
		       node == CTND_NODE_NAMES ? "critical_task_name" : "ct_enable", want);
	event(ops, message);
	return save(s, ops);
}

static bool reconcile(struct ctnd_state *s, const struct ctnd_ops *ops)
{
	bool ok;

	if (s->journal_dirty && !save(s, ops)) return false;
	if (!s->cfg_ready) return true;
	if (s->cfg.result == CTND_ERROR || s->cfg.result == CTND_UNSUPPORTED) return true;
	if (s->cfg.result == CTND_VALID)
		ok = apply(s, ops, CTND_NODE_NAMES, s->cfg.names);
	else
		ok = restore(s, ops, CTND_NODE_NAMES);
	if (s->journal_dirty) return false;
	/* CTB is an independent opt-in; a CTB-only change must not be skipped. */
	if (s->manage_ctb && s->cfg.have_ctb) {
		if (!apply(s, ops, CTND_NODE_ENABLE, s->cfg.ctb ? "1" : "0")) ok = false;
	} else if (!restore(s, ops, CTND_NODE_ENABLE)) ok = false;
	return ok;
}

static void reset_game(struct ctnd_state *s)
{
	memset(s->yielded, 0, sizeof(s->yielded));
	memset(s->observed_valid, 0, sizeof(s->observed_valid));
}

void ctnd_state_tick(struct ctnd_state *s, const struct ctnd_ops *ops,
		     bool pid_ok, int pid, uint64_t now)
{
	bool changed = false;

	if (s->recovering) {
		if (now < s->next_action) return;
		s->next_action = now + CTND_RETRY_MS;
		if (!restore_all(s, ops)) return;
		s->recovering = false;
		s->next_action = now;
	}
	if (!pid_ok) {
		/* Unreadable PID is unknown, not a negative sample. */
		s->gone = false;
		if (s->pid <= 0 && now >= s->next_action) {
			(void)restore_all(s, ops);
			s->next_action = now + CTND_RETRY_MS;
		}
		return;
	}
	if (pid > 0) {
		s->gone = false; /* Also clear on the SAME positive PID after a transient -1. */
		if (pid != s->pid) {
			/* Startup recovery's HAL yield belongs to the first observed game. */
			if (!s->initial_session) reset_game(s);
			s->initial_session = false;
			s->pid = pid;
			s->package[0] = '\0';
			s->cfg_ready = false;
			s->next_lookup = now;
			s->next_action = now;
			changed = true;
		}
	} else {
		if (s->pid > 0 || s->initial_session) {
			if (!s->gone) { s->gone = true; s->gone_since = now; }
			if (now - s->gone_since < CTND_GONE_MS) return;
			s->initial_session = false;
			reset_game(s);
			s->pid = -1;
			s->package[0] = '\0';
			s->cfg_ready = false;
			s->gone = false;
			s->next_action = now;
		}
		if (now >= s->next_action) {
			(void)restore_all(s, ops);
			s->next_action = now + CTND_RETRY_MS;
		}
		return;
	}
	if (now >= s->next_lookup) {
		struct ctnd_cfg cfg = {0};
		enum ctnd_result result;

		if (!s->package[0] && !ops->package(ops->ctx, pid, s->package, sizeof(s->package))) {
			s->package[0] = '\0';
			s->next_lookup = now + CTND_RETRY_MS;
			return;
		}
		result = ops->lookup(ops->ctx, s->package, now, &cfg);
		cfg.result = result;
		if (result == CTND_ERROR || result == CTND_UNSUPPORTED) {
			s->cfg_ready = false;
			s->next_lookup = result == CTND_ERROR ? now + CTND_RETRY_MS :
				(s->recheck_ms ? now + s->recheck_ms : UINT64_MAX);
			return;
		}
		s->cfg = cfg;
		s->cfg_ready = true;
		s->next_lookup = s->recheck_ms ? now + s->recheck_ms : UINT64_MAX;
		changed = true;
	}
	if (changed || now >= s->next_action) {
		(void)reconcile(s, ops);
		s->next_action = now + CTND_RETRY_MS;
	}
}
