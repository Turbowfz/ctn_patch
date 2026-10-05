// SPDX-License-Identifier: GPL-2.0-only
/*
 * fakepkg —— 测试用的小工具（不打包进模块 zip）
 *
 * 目的：伪造一个「进程名 = 指定包名」的进程，好让 ctnd 的
 * /proc/<pid>/cmdline 路径读到我们想要的包名。
 *
 * 做法：把自己的 argv[0] 那块内存覆写成目标名字。内核读
 * /proc/<pid>/cmdline 读的就是这块内存，所以 ps 和 ctnd 都会看到新名字。
 * 之后 pause() 挂着不动，等测试脚本 kill。
 *
 * 用法：
 *     fakepkg <要伪装的进程名>
 * 例：
 *     fakepkg com.tencent.tmgp.pubgmhd &
 *
 * 为什么不用 `sh -c 'exec -a 名字 ...'`：设备上的 toybox sh 不支持 -a。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	static char name[512];
	size_t total = 0;
	size_t want;
	int i;

	if (argc < 2) {
		fprintf(stderr, "用法: fakepkg <要伪装的进程名>\n");
		return 2;
	}

	/* argv[] 在初始栈上是连续的一整段，能用的空间就是所有参数加 NUL */
	for (i = 0; i < argc; i++)
		total += strlen(argv[i]) + 1;

	want = strlen(argv[1]);
	if (want + 1 > total) {
		fprintf(stderr, "名字太长（%zu 字节，可用 %zu）\n", want, total);
		return 2;
	}
	if (want + 1 > sizeof(name))
		return 2;

	/* 先落到本地缓冲：目标区（argv[0] 起）和源（argv[1]）可能重叠 */
	memcpy(name, argv[1], want);
	name[want] = '\0';

	memcpy(argv[0], name, want);
	argv[0][want] = '\0';

	/* 名字改好了，挂着等测试脚本收尾 */
	pause();
	return 0;
}
