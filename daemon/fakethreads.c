// SPDX-License-Identifier: GPL-2.0-only
/*
 * fakethreads —— 测试用的小工具（不打包进模块 zip）
 *
 * 造一个进程，里面开 N 个线程、每个线程的 comm 改成给定的名字，然后全体挂着。
 * 用来验证「内核按线程名匹配」那条链（related_threads / critical_task）。
 *
 * 为什么不用 fakepkg：那个只改 argv[0]（影响 /proc/<pid>/cmdline），
 * 而内核匹配用的是 task->comm —— 必须用 prctl(PR_SET_NAME) 改。
 *
 * 用法：
 *     fakethreads RenderThread GameThread
 * 输出（每行一个，方便脚本解析）：
 *     TGID <进程pid>
 *     THREAD <名字> <tid>
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static char names[8][32];

static void *worker(void *arg)
{
	const char *name = arg;
	long tid = syscall(SYS_gettid);

	prctl(PR_SET_NAME, name, 0, 0, 0);
	printf("THREAD %s %ld\n", name, tid);
	fflush(stdout);
	for (;;)
		pause();
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t th[8];
	int n = argc - 1;
	int i;

	if (n < 1 || n > 8) {
		fprintf(stderr, "用法: fakethreads <名字1> [名字2 ...]（最多 8 个）\n");
		return 2;
	}

	printf("TGID %d\n", (int)getpid());
	fflush(stdout);

	for (i = 0; i < n; i++) {
		snprintf(names[i], sizeof(names[i]), "%s", argv[i + 1]);
		if (pthread_create(&th[i], NULL, worker, names[i]) != 0) {
			fprintf(stderr, "建线程失败: %s\n", names[i]);
			return 1;
		}
		usleep(100000);		/* 让每个线程先把自己的名字改好 */
	}

	for (;;)
		pause();
	return 0;
}
