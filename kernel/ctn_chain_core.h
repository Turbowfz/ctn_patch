/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CTN_CHAIN_CORE_H
#define CTN_CHAIN_CORE_H

#include "ctn_patch_internal.h"
#ifdef __KERNEL__
#include <linux/types.h>
typedef u64 ctn_chain_u64;
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t ctn_chain_u64;
#endif

/* comm 只有 15 字节可用；更长的配置名不可能命中任何真实线程。 */
#define CTN_COMM_LEN 16
#define CTN_BIND_CANDIDATES 2
#define CTN_CHAIN_U64_MAX ((ctn_chain_u64)-1)

struct ctn_chain_config {
	char names[CT_NUM][CT_NAME_LEN];
	unsigned int length[CT_NUM];
};

/*
 * 绑定里的 task 以指针身份参与热路径；内核适配层必须持引用，
 * 防止 slab 复用让同名同 PID 的新任务冒充旧绑定。
 */
struct ctn_chain_task {
	int pid;
	int tgid;
	unsigned long token;
	char comm[CTN_COMM_LEN];
};

struct ctn_chain_binding {
	ctn_chain_u64 generation;
	ctn_chain_u64 epoch;
	int tgid;
	struct ctn_chain_task tasks[CT_NUM];
};

struct ctn_chain_selection {
	struct ctn_chain_config config;
	int tgid;
	struct ctn_chain_task candidates[CT_NUM][CTN_BIND_CANDIDATES];
};

struct ctn_chain_time {
	ctn_chain_u64 running_ns;
	ctn_chain_u64 blocked_ns;
	ctn_chain_u64 end_ns;
	ctn_chain_u64 window_running_ns;
	int running;
	int cpu;
};

/* 先在临时对象解析；拒绝输入不能留下半个新配置。 */
static inline int ctn_chain_parse(const char *input, size_t count,
				  struct ctn_chain_config *out)
{
	struct ctn_chain_config next;
	unsigned int i;
	int ret;

	if (!out)
		return -EINVAL;
	memset(&next, 0, sizeof(next));
	ret = ctn_parse_names(input, count, next.names);
	if (ret)
		return ret;
	for (i = 0; i < CT_NUM; i++)
		next.length[i] = (unsigned int)strlen(next.names[i]);
	*out = next;
	return 0;
}

static inline bool ctn_chain_config_valid(const struct ctn_chain_config *config)
{
	unsigned int i;
	size_t length;

	if (!config)
		return false;
	for (i = 0; i < CT_NUM; i++) {
		for (length = 0; length < CT_NAME_LEN && config->names[i][length]; length++)
			;
		if (!length || length >= CT_NAME_LEN || length != config->length[i])
			return false;
	}
	return true;
}

static inline bool ctn_chain_task_valid(const struct ctn_chain_task *task)
{
	return task && task->pid > 0 && task->tgid > 0 && task->token &&
	       task->comm[0] && memchr(task->comm, '\0', sizeof(task->comm));
}

static inline bool ctn_chain_name_matches(const struct ctn_chain_config *config,
					  unsigned int slot,
					  const struct ctn_chain_task *task)
{
	return slot < CT_NUM && config->length[slot] > 0 &&
	       config->length[slot] < CTN_COMM_LEN &&
	       !memcmp(config->names[slot], task->comm, config->length[slot]);
}

static inline void ctn_chain_select_init(struct ctn_chain_selection *selection,
					 const struct ctn_chain_config *config,
					 int tgid)
{
	memset(selection, 0, sizeof(*selection));
	selection->config = *config;
	selection->tgid = tgid;
}

/* 候选必须来自已核对的同组登记表；这里只比较值，不解引用 token。 */
static inline void ctn_chain_select_add(struct ctn_chain_selection *selection,
					const struct ctn_chain_task *task)
{
	unsigned int slot, candidate;

	if (!ctn_chain_config_valid(&selection->config) ||
	    !ctn_chain_task_valid(task) ||
	    (selection->tgid > 0 && task->tgid != selection->tgid))
		return;
	for (slot = 0; slot < CT_NUM; slot++) {
		if (!ctn_chain_name_matches(&selection->config, slot, task))
			continue;
		for (candidate = 0; candidate < CTN_BIND_CANDIDATES; candidate++) {
			struct ctn_chain_task *saved = &selection->candidates[slot][candidate];

			if (saved->pid == task->pid && saved->token == task->token)
				break;
			if (!saved->token) {
				*saved = *task;
				break;
			}
		}
	}
}

/* 与新官方实现一致：槽 1 优先，两槽不得重复统计同一个 PID。 */
static inline void ctn_chain_select_finish(struct ctn_chain_selection *selection,
					   struct ctn_chain_binding *out)
{
	unsigned int i;

	memset(out, 0, sizeof(*out));
	out->tgid = selection->tgid > 0 ? selection->tgid :
					 selection->candidates[1][0].pid > 0 ?
					 selection->candidates[1][0].tgid : -1;
	out->tasks[1] = selection->candidates[1][0];
	for (i = 0; i < CTN_BIND_CANDIDATES; i++) {
		const struct ctn_chain_task *candidate = &selection->candidates[0][i];

		if (candidate->token && candidate->pid != out->tasks[1].pid) {
			out->tasks[0] = *candidate;
			break;
		}
	}
	if (!out->tgid || out->tgid < 0)
		out->tgid = -1;
}

static inline bool ctn_chain_binding_current(const struct ctn_chain_binding *binding,
					     ctn_chain_u64 generation,
					     ctn_chain_u64 epoch)
{
	return binding && binding->tgid > 0 &&
	       (binding->tasks[0].token || binding->tasks[1].token) &&
	       binding->generation == generation && binding->epoch == epoch;
}

/* 热路径只核对固定两槽身份与有界名称，不扫描 related_threads。 */
static inline unsigned int ctn_chain_match(const struct ctn_chain_config *config,
					  const struct ctn_chain_binding *binding,
					  const struct ctn_chain_task *task,
					  ctn_chain_u64 generation,
					  ctn_chain_u64 epoch,
					  bool enabled)
{
	unsigned int slot;

	if (!enabled || !ctn_chain_task_valid(task) ||
	    !ctn_chain_binding_current(binding, generation, epoch))
		return 0;
	for (slot = 0; slot < CT_NUM; slot++) {
		const struct ctn_chain_task *saved = &binding->tasks[slot];

		if (saved->token && saved->token == task->token &&
		    saved->pid == task->pid && task->tgid == binding->tgid &&
		    ctn_chain_name_matches(config, slot, task))
			return 1U << slot;
	}
	return 0;
}

static inline bool ctn_chain_advance(ctn_chain_u64 *counter)
{
	if (*counter == CTN_CHAIN_U64_MAX)
		return false;
	++*counter;
	return true;
}

static inline ctn_chain_u64 ctn_chain_sat_add(ctn_chain_u64 left, ctn_chain_u64 right)
{
	return right > CTN_CHAIN_U64_MAX - left ? CTN_CHAIN_U64_MAX : left + right;
}

/*
 * 官方一加13计时语义：clock 倒退时整次丢弃；首次事件只落 end；
 * running/blocked 与状态只在已有 end 时累计；窗口起点取两者较大值；
 * 全部饱和到 U64_MAX，不更改任何加速阈值。
 */
static inline bool ctn_chain_account(struct ctn_chain_time *sample,
				    ctn_chain_u64 now_ns,
				    ctn_chain_u64 window_end_ns,
				    bool is_prev, int cpu)
{
	ctn_chain_u64 delta, start;

	if (!sample || !now_ns || now_ns < sample->end_ns)
		return false;
	if (sample->end_ns) {
		delta = now_ns - sample->end_ns;
		if (is_prev) {
			sample->running_ns = ctn_chain_sat_add(sample->running_ns, delta);
			start = sample->end_ns > window_end_ns ? sample->end_ns : window_end_ns;
			if (now_ns > start)
				sample->window_running_ns = ctn_chain_sat_add(
					sample->window_running_ns, now_ns - start);
			sample->running = 0;
		} else {
			sample->blocked_ns = ctn_chain_sat_add(sample->blocked_ns, delta);
			sample->running = 1;
			sample->cpu = cpu;
		}
	}
	sample->end_ns = now_ns;
	return true;
}

#endif /* CTN_CHAIN_CORE_H */
