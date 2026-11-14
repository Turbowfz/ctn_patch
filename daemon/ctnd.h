/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CTND_H
#define CTND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ctnd_paths.h"

#define CTND_NAME_LEN 100
#define CTND_NAMES_LEN 200
#define CTND_PKG_LEN 256
#define CTND_RAW_MAX 65536
#define CTND_BOOT_LEN 37
#define CTND_CACHE_MAX 64

/* MISSING means a successful query with no row, never an unavailable DB. */
enum ctnd_result {
	CTND_ERROR,
	CTND_MISSING,
	CTND_EMPTY,
	CTND_VALID,
	CTND_UNSUPPORTED
};

struct ctnd_cfg {
	enum ctnd_result result;
	char names[CTND_NAMES_LEN];
	bool have_ctb;
	int ctb;
};

bool ctnd_base64(const char *in, size_t len, char *out, size_t cap, size_t *written);
bool ctnd_names(const char *in, char *out, size_t cap);
bool ctnd_node_names(const char *in, char *out, size_t cap);
bool ctnd_node_enable(const char *in, char *out, size_t cap);
bool ctnd_game_pid(const char *text, int *pid);
enum ctnd_result ctnd_parse_cfg(const char *raw, size_t len, struct ctnd_cfg *cfg);

/* Only these two fixed nodes can be written by a release build. */
enum ctnd_node { CTND_NODE_NAMES, CTND_NODE_ENABLE, CTND_NODE_COUNT };

struct ctnd_debt {
	bool owed;
	bool applied_valid;
	bool pending_valid;
	char original[CTND_NAMES_LEN];
	char applied[CTND_NAMES_LEN];
	char pending[CTND_NAMES_LEN];
};

struct ctnd_journal {
	struct ctnd_debt nodes[CTND_NODE_COUNT];
};

struct ctnd_ops {
	void *ctx;
	bool (*read_node)(void *ctx, enum ctnd_node node, char *out, size_t cap);
	bool (*write_node)(void *ctx, enum ctnd_node node, const char *value);
	bool (*save)(void *ctx, const struct ctnd_journal *journal);
	bool (*package)(void *ctx, int pid, char *out, size_t cap);
	enum ctnd_result (*lookup)(void *ctx, const char *pkg, uint64_t now,
				   struct ctnd_cfg *cfg);
	void (*event)(void *ctx, const char *message);
};

struct ctnd_state {
	struct ctnd_journal journal;
	struct ctnd_cfg cfg;
	char package[CTND_PKG_LEN];
	int pid;
	uint64_t gone_since;
	uint64_t next_lookup;
	uint64_t next_action;
	uint64_t recheck_ms;
	bool gone;
	bool cfg_ready;
	bool manage_ctb;
	bool recovering;
	bool initial_session;
	bool journal_dirty;
	bool yielded[CTND_NODE_COUNT];
	bool observed_valid[CTND_NODE_COUNT];
	char observed[CTND_NODE_COUNT][CTND_NAMES_LEN];
};

void ctnd_state_init(struct ctnd_state *state, bool manage_ctb, uint64_t recheck_ms,
		     const struct ctnd_journal *journal);
void ctnd_state_tick(struct ctnd_state *state, const struct ctnd_ops *ops,
		     bool pid_ok, int pid, uint64_t now);
bool ctnd_state_shutdown(struct ctnd_state *state, const struct ctnd_ops *ops);
bool ctnd_state_has_debt(const struct ctnd_state *state);

#endif
