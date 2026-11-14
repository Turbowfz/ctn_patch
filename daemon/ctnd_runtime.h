/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CTND_RUNTIME_H
#define CTND_RUNTIME_H

#include "ctnd.h"

struct ctnd_runtime {
	int dirfd;
	int lockfd;
	char boot_id[CTND_BOOT_LEN];
	bool pid_written;
};

int ctnd_runtime_open(struct ctnd_runtime *rt, const char *directory, const char *boot_path);
bool ctnd_runtime_load(struct ctnd_runtime *rt, struct ctnd_journal *journal);
bool ctnd_runtime_save(struct ctnd_runtime *rt, const struct ctnd_journal *journal);
void ctnd_runtime_close(struct ctnd_runtime *rt);
bool ctnd_runtime_read_node(enum ctnd_node node, char *out, size_t cap);
bool ctnd_runtime_write_node(enum ctnd_node node, const char *value);
bool ctnd_runtime_package(int pid, char *out, size_t cap);
uint64_t ctnd_now_ms(void);

#endif
