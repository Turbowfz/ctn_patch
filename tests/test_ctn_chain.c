/* SPDX-License-Identifier: GPL-2.0-only */
/* CTN 链路核心主机回归：直接包含生产实现，验证真实算法而非源码字符串。 */
#include "ctn_chain_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect(int yes, const char *what)
{
	if (!yes) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

/* ---- 选择/匹配/计时（纯函数） ---- */

static struct ctn_chain_task make_task(int pid, const char *comm)
{
	struct ctn_chain_task task;

	memset(&task, 0, sizeof(task));
	task.pid = pid;
	task.tgid = 900;
	task.token = (unsigned long)(100 + (unsigned long)pid);
	strncpy(task.comm, comm, CTN_COMM_LEN - 1);
	return task;
}

static struct ctn_chain_config make_config(const char *first, const char *second)
{
	struct ctn_chain_config config;

	memset(&config, 0, sizeof(config));
	strncpy(config.names[0], first, CT_NAME_LEN - 1);
	strncpy(config.names[1], second, CT_NAME_LEN - 1);
	config.length[0] = (unsigned int)strlen(first);
	config.length[1] = (unsigned int)strlen(second);
	return config;
}

static void test_parse_and_config(void)
{
	struct ctn_chain_config config;
	char input[CT_INPUT_LEN];

	memset(input, 'a', sizeof(input));
	expect(ctn_chain_parse("GameThread RenderThread", 23, &config) == 0,
	       "parse two names");
	expect(!strcmp(config.names[0], "GameThread") &&
	       !strcmp(config.names[1], "RenderThread"), "parse slot order");
	expect(config.length[0] == 10 && config.length[1] == 12, "parse lengths");
	expect(ctn_chain_parse("Solo", 4, &config) == 0 &&
	       !strcmp(config.names[0], "Solo") &&
	       !strcmp(config.names[1], "Solo"), "single name copies");
	expect(ctn_chain_parse("A B C", 5, &config) == -EINVAL, "third name rejected");
	expect(ctn_chain_parse("", 0, &config) == -E2BIG, "empty write rejected");
	expect(ctn_chain_parse(input, CT_INPUT_LEN - 1, &config) == -ENAMETOOLONG,
	       "oversized write rejected");
	expect(ctn_chain_parse(NULL, 4, &config) == -EINVAL, "null input rejected");
	memset(&config, 0, sizeof(config));
	expect(!ctn_chain_config_valid(&config), "empty config invalid");
	expect(ctn_chain_config_valid(NULL) == false, "null config invalid");
}

static void test_select_slot_priority(void)
{
	struct ctn_chain_selection selection;
	struct ctn_chain_binding out;
	struct ctn_chain_config config = make_config("Thread", "Thread");
	struct ctn_chain_task a = make_task(11, "Thread");
	struct ctn_chain_task b = make_task(22, "Thread");
	struct ctn_chain_task group = make_task(1, "worker");

	ctn_chain_select_init(&selection, &config, group.tgid);
	ctn_chain_select_add(&selection, &a);
	ctn_chain_select_add(&selection, &a);
	ctn_chain_select_add(&selection, &b);
	ctn_chain_select_finish(&selection, &out);
	expect(out.tasks[1].pid == 11, "slot1 takes the first candidate");
	expect(out.tasks[0].pid == 22, "slot0 excludes slot1 pid");
	expect(out.tgid == group.tgid, "binding keeps registry tgid");

	/* 官方刷新顺序：先登记的旧窗口在 clean 后只剩同名候选时同样成立。 */
	ctn_chain_select_init(&selection, &config, group.tgid);
	ctn_chain_select_add(&selection, &b);
	ctn_chain_select_add(&selection, &a);
	ctn_chain_select_finish(&selection, &out);
	expect(out.tasks[1].pid == 22 && out.tasks[0].pid == 11,
	       "candidate order decides slot assignment");
}

static void test_select_rejects(void)
{
	struct ctn_chain_selection selection;
	struct ctn_chain_binding out;
	struct ctn_chain_config config = make_config("GameThread", "RenderThread");
	struct ctn_chain_task other = make_task(7, "worker");
	struct ctn_chain_task mismatch = make_task(9, "GameThread");

	ctn_chain_select_init(&selection, &config, other.tgid);
	ctn_chain_select_add(&selection, &other);
	ctn_chain_select_finish(&selection, &out);
	expect(!out.tasks[0].token && !out.tasks[1].token,
	       "no matching candidate publishes empty slots");

	ctn_chain_select_init(&selection, &config, other.tgid);
	mismatch.tgid = 999;
	ctn_chain_select_add(&selection, &mismatch);
	ctn_chain_select_finish(&selection, &out);
	expect(!out.tasks[0].token, "cross-tgid candidate rejected");

	ctn_chain_select_init(&selection, &config, -1);
	mismatch = make_task(9, "GameThread");
	mismatch.tgid = -5;
	ctn_chain_select_add(&selection, &mismatch);
	ctn_chain_select_finish(&selection, &out);
	expect(out.tgid == -1, "invalid group falls back to -1");
}

static void test_match_identity(void)
{
	struct ctn_chain_config config = make_config("GameThread", "RenderThread");
	struct ctn_chain_binding binding;
	struct ctn_chain_task t1 = make_task(33, "GameThread");
	struct ctn_chain_task view;

	memset(&binding, 0, sizeof(binding));
	binding.tgid = t1.tgid;
	binding.generation = 2;
	binding.epoch = 3;
	binding.tasks[0] = t1;
	expect(ctn_chain_match(&config, &binding, &t1, 2, 3, true) == 1u,
	       "matching slot0 identity");
	expect(ctn_chain_match(&config, &binding, &t1, 3, 3, true) == 0,
	       "stale generation rejected");
	expect(ctn_chain_match(&config, &binding, &t1, 2, 4, true) == 0,
	       "stale epoch rejected");
	expect(ctn_chain_match(&config, &binding, &t1, 2, 3, false) == 0,
	       "disabled matches nothing");
	view = t1;
	view.pid = 34;
	expect(ctn_chain_match(&config, &binding, &view, 2, 3, true) == 0,
	       "pid mismatch rejected");
	view = t1;
	view.token = 1;
	expect(ctn_chain_match(&config, &binding, &view, 2, 3, true) == 0,
	       "token mismatch rejected");
	view = t1;
	strcpy(view.comm, "RenderThread");
	expect(ctn_chain_match(&config, &binding, &view, 2, 3, true) == 0,
	       "name prefix mismatch rejected");
	view = t1;
	view.tgid = 1;
	expect(ctn_chain_match(&config, &binding, &view, 2, 3, true) == 0,
	       "tgid mismatch rejected");
	expect(ctn_chain_match(&config, &binding, &t1, 2, 3, true) == 1u,
	       "slot1 never claims slot0-only binding");
}

static void test_account_official_semantics(void)
{
	struct ctn_chain_time sample;

	memset(&sample, 0, sizeof(sample));
	/* 首次事件：官方语义只落 end，不累计、不改状态。 */
	expect(ctn_chain_account(&sample, 100, 0, true, 2) == true, "first prev accepted");
	expect(sample.end_ns == 100 && sample.running_ns == 0 && sample.running == 0,
	       "first prev only stamps end");
	/* 正常 prev：running 自上次 end 起累计，窗口起点取两者较大值。 */
	expect(ctn_chain_account(&sample, 150, 120, true, 2) == true, "second prev");
	expect(sample.running_ns == 50, "running accumulates from end");
	expect(sample.window_running_ns == 30, "window uses max(end,window)");
	expect(sample.running == 0, "prev marks not running");
	/* next：blocked 累计并记录 CPU。 */
	expect(ctn_chain_account(&sample, 180, 120, false, 5) == true, "next event");
	expect(sample.blocked_ns == 30 && sample.running == 1 && sample.cpu == 5,
	       "next accumulates blocked and cpu");
	/* clock 倒退：整次丢弃。 */
	expect(ctn_chain_account(&sample, 100, 0, true, 0) == false, "clock rollback rejected");
	expect(sample.end_ns == 180, "rollback leaves end unchanged");
	/* 饱和。 */
	sample.running_ns = CTN_CHAIN_U64_MAX - 5;
	sample.window_running_ns = CTN_CHAIN_U64_MAX - 1;
	ctn_chain_account(&sample, 200, 0, true, 0);
	expect(sample.running_ns == CTN_CHAIN_U64_MAX, "running saturates");
	expect(sample.window_running_ns == CTN_CHAIN_U64_MAX, "window saturates");
	sample.end_ns = 0;
	sample.running_ns = 0;
	expect(ctn_chain_account(&sample, 10, 0, true, 0) == true, "fresh end accepted");
	expect(sample.running_ns == 0 && sample.end_ns == 10,
	       "fresh end still skips accumulation");
	expect(ctn_chain_account(NULL, 1, 0, true, 0) == false, "null sample rejected");
}

static void test_advance_wrap(void)
{
	ctn_chain_u64 counter = CTN_CHAIN_U64_MAX;

	expect(ctn_chain_advance(&counter) == false, "wrap fails closed");
	expect(counter == CTN_CHAIN_U64_MAX, "wrap keeps value");
	counter = 9;
	expect(ctn_chain_advance(&counter) == true && counter == 10, "advance works");
}

/*
 * 发布/恢复生命周期位于内核适配层（依赖真实任务引用与 RCU），主机回归只覆盖
 * 纯算法核心；内核侧由双基线静态检验与后续受控实机测试覆盖。
 */

int main(void)
{
	test_parse_and_config();
	test_select_slot_priority();
	test_select_rejects();
	test_match_identity();
	test_account_official_semantics();
	test_advance_wrap();
	if (failures) {
		printf("FAILURES: %d\n", failures);
		return 1;
	}
	printf("PASS: ctn chain core host tests\n");
	return 0;
}
