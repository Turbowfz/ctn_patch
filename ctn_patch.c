// SPDX-License-Identifier: GPL-2.0-only
/*
 * ctn_patch - Copyright (c) Turbo
 *
 * 把官方 6.6（一加13）的 /proc/game_opt/task_boost/critical_task_name
 * 移植到 8Gen3 / 6.1 内核（一加 Ace3Pro / Ace5 / 一加12 / GT6）。
 *
 * ============================ 移植来源 ============================
 * 全部取自本工作区 6.6_一加13/官方源码仓库/…/vendor/oplus/kernel/cpu/game_opt/：
 *   critical_task_boost.c:78   static char critical_task[2][100] = {"UnityMain","UnityGfxDevice"};
 *   critical_task_boost.c:80   static pid_t critical_task_pids[2] = {-1,-1};
 *   critical_task_boost.c:602  critical_task_name_proc_write()
 *                                —— sscanf(page, "%99s %99s")；写完把两个 pid 置 -1
 *   critical_task_boost.c:631  critical_task_name_proc_read()
 *                                —— sprintf(page, "%s:%d,%s:%d\n", 名字, pid, 名字, pid)
 *   rt_info.c:492              check_task_name()          —— 名字必须 < TASK_COMM_LEN 才有 pid
 *   rt_info.c:528              is_matching_thread()      —— 两个槽不指向同一个线程
 *   rt_info.c:543              find_critical_task_pid()  —— 在 related_threads[] 里按名字找 pid
 *   rt_info.c:568              update_critical_task_pids()—— 解析流程：先槽 1、再槽 0
 *
 * ===================== 6.1 与 6.6 的差异，本模块怎么补 =====================
 * 1) 名单布局：6.1 是 `const char *critical_task[2]`，在模块 .rodata（只读）。
 *    直接写会触发内核写保护异常 —— 本模块用 vmap(&page,1,VM_MAP,PAGE_KERNEL)
 *    建同一物理页的可写别名，只通过别名改内容。
 *    （6.6 是内联 char[2][100] 在可写段，所以官方能直接 sscanf 写进去。）
 *
 * 2) 没有 critical_task_pids[]：6.1 全仓搜不到（critical_task_pids /
 *    update_ctb_pids 均 0 命中）。本模块在**读节点时现算**：
 *    去 6.1 自己的 related_threads[]（rt_info.c）里按名字找，
 *    规则与 6.6 的 update_critical_task_pids() 一致（含「两槽不重 pid」）。
 *    于是读出来的是**当前真实 pid**，而不是写死 -1。
 *    数据源取不到（内核变体不同）时自动退化成 -1，功能不受影响。
 *
 * 3) 单名写入：6.6 的 sscanf 要求恰好两个，写一个会被 -EINVAL 拒。
 *    本模块放宽为「一个也行」，第二个槽复制同一个名字 —— 内核里两槽记同一
 *    线程等价于一个（decide_boost_status 只是对同一个 CPU 重复
 *    cpumask_set_cpu，不会双倍加成）。云控 game_config 的 ctn 只给一个名字时
 *    也就直接生效。
 *
 * ============================ 安全性 ============================
 * - 换指针用「双缓冲 + synchronize_rcu()」：读者是 sched_switch tracepoint 里的
 *   update_critical_task_time()（critical_task_boost.c:349），6.1 的 __DO_TRACE
 *   用 rcu_read_lock_sched_notrace() 包住回调，所以 synchronize_rcu() 等得到它们。
 * - find_module + try_module_get 钉住 oplus_bsp_game_opt，防止它先被 rmmod
 *   导致手里的符号变野指针。
 * - related_threads 的读法与 6.1 自己的 get_critical_task_state()（rt_info.c:404）
 *   完全相同（裸读指针 + NULL 检查）—— 同样的暴露面，而且这里是冷路径
 *   （只在有人 cat 节点时跑一次），不像它是 sched_switch 热路径。
 * - 布局守卫：6.6 的 critical_task[0] 内联在数组里（低字节是 'U'），不是内核
 *   指针；用「上半区指针」一判就拒掉，避免把新内核改坏。
 *
 * 加载顺序：oplus_bsp_game_opt 先加载（开机就在）；卸载顺序相反。
 */

#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#define CT_NUM			2
/* 官方 6.6 用 char[2][100]、写入上限 "%99s"：名字最长 99 字符 + NUL。 */
#define CT_NAME_LEN		100
#define CT_INPUT_LEN		256
#define CT_PTR_ARRAY_BYTES	(CT_NUM * sizeof(const char *))

/* 6.1 game_ctrl.h:13 */
#define MAX_TID_COUNT		256

#define VICTIM_MODULE		"oplus_bsp_game_opt"
#define SYM_CRITICAL_TASK	VICTIM_MODULE ":critical_task"
#define SYM_TASK_BOOST_DIR	VICTIM_MODULE ":critical_heavy_boost_dir"
#define SYM_RELATED_THREADS	VICTIM_MODULE ":related_threads"
#define SYM_TOTAL_NUM		VICTIM_MODULE ":total_num"
#define SYM_HAVE_VALID_RP	VICTIM_MODULE ":have_valid_render_pid"
#define SYM_RT_LOCK		VICTIM_MODULE ":rt_info_rwlock"

/*
 * 6.1 rt_info.c:22 的 struct render_related_thread 镜像。
 * 那个结构体在 vendor 模块里是私有的（头文件不在开源仓里），所以这里按源码
 * 逐字复刻；arm64 布局：pid_t(4) + 4 填充 + task_struct*(8) + u32(4) + 4 尾部
 * 填充 = 24 字节。设备上的行为测试（把已知 pid 写进 rt_info 再读节点核对）
 * 就是对这个布局的实锤 —— 布局错了 pid 根本对不上。
 */
struct rrt_entry {
	pid_t pid;
	struct task_struct *task;
	u32 wake_count;
};

/* ------------------------------------------------------------------ */

static const char **critical_task;	/* 只读映射里的原数组（读当前值用） */
static const char **critical_task_rw;	/* 同一物理页的可写别名 */
static void *critical_task_vmap;
static struct proc_dir_entry *task_boost_dir;
static struct proc_dir_entry *name_entry;
static bool patched;

static const char *original_name[CT_NUM];
static char name_buf[CT_NUM][2][CT_NAME_LEN];
static u8 active_slot[CT_NUM];

static struct module *victim;

/* pid 解析用的数据源（6.6 的 critical_task_pids 在 6.1 的替代品） */
static struct rrt_entry *g_related_threads;
static int *g_total_num;
static int *g_have_valid_rp;
/*
 * 6.1 rt_info.c:32 的 static DEFINE_RWLOCK(rt_info_rwlock)。
 * 厂商自己在 rt_info_proc_write() 里改 related_threads[]（含 put_task_struct）
 * 是持 write_lock 的；我们读时也拿 read_lock，就不会读到「刚要被释放的 task」。
 * 拿不到这个符号就退回不加锁 —— 那也和厂商自己的热路径 get_critical_task_state()
 * 一样（它读 related_threads 是不加锁的），只是没比它更差而已。
 */
static rwlock_t *g_rt_lock;

static DEFINE_MUTEX(patch_lock);

static unsigned long (*lookup_name)(const char *name);
static struct module *(*find_module_fn)(const char *name);

/* ------------------------------------------------------------------ */
/* 符号解析                                                            */
/* ------------------------------------------------------------------ */

static int resolve_lookup_name(void)
{
	struct kprobe probe = {
		.symbol_name = "kallsyms_lookup_name",
	};
	int ret;

	ret = register_kprobe(&probe);
	if (ret)
		return ret;

	lookup_name = (void *)probe.addr;
	unregister_kprobe(&probe);
	return lookup_name ? 0 : -ENOENT;
}

static bool kernel_address_looks_valid(const void *ptr)
{
	/* arm64 内核/模块地址都在上半区，最高位为 1。 */
	return ptr && (long)(unsigned long)ptr < 0;
}

/*
 * copy_from_kernel_nofault 没导出给模块用：本内核 CONFIG_TRIM_UNUSED_KSYMS=y，
 * 它被裁掉了 —— 432 个设备模块的 __versions 段里都找不到它（其余我们用到的符号
 * 全都能找到，见 Module.symvers.device）。所以只能运行时用 kallsyms 取地址。
 * 取不到就退回 memcpy：这里读的地址要么是模块 .rodata 里已校验过的字符串，
 * 要么是我们自己的缓冲区，都是确定映射的，memcpy 同样安全。
 */
static long (*copy_from_kernel_nofault_fn)(void *dst, const void *src, size_t size);

/*
 * 从内核地址拷一个 NUL 结尾的字符串出来。
 * 先按 task->comm 的容量读 16 字节（正常名字到这里就有 NUL 了），
 * 没找到 NUL 再补读剩下的 —— 避免一上来就读 100 字节踩到未映射页。
 * 全程找不到 NUL 说明源串非法，报错而不是静默截断。
 */
static int copy_kernel_string(const char *src, char *dst, size_t dst_len)
{
	size_t n;

	if (!src || !dst || dst_len < 2)
		return -EINVAL;

	n = min(dst_len, (size_t)TASK_COMM_LEN);

	if (copy_from_kernel_nofault_fn) {
		if (copy_from_kernel_nofault_fn(dst, src, n))
			return -EFAULT;
	} else {
		memcpy(dst, src, n);
	}
	if (memchr(dst, '\0', n))
		return 0;
	if (n == dst_len)
		return -EFAULT;

	if (copy_from_kernel_nofault_fn) {
		if (copy_from_kernel_nofault_fn(dst + n, src + n, dst_len - n))
			return -EFAULT;
	} else {
		memcpy(dst + n, src + n, dst_len - n);
	}
	if (!memchr(dst + n, '\0', dst_len - n))
		return -EFAULT;
	return 0;
}

/* ------------------------------------------------------------------ */
/* pid 解析（6.6 rt_info.c:543 find_critical_task_pid 的 6.1 版）      */
/* ------------------------------------------------------------------ */

/*
 * 在游戏的相关线程里按名字找一个 pid。
 * 与 6.1 自己的 get_critical_task_state()（rt_info.c:404）用同一套匹配：
 * strncmp(名字, task->comm, strlen(名字)) == 0。
 * last_pid 是 6.6 is_matching_thread() 的去重规则：已经给上一个槽的 pid 跳过。
 */
static pid_t find_pid_by_name(const char *name, pid_t *last_pid)
{
	unsigned long flags;
	size_t name_len = strlen(name);
	pid_t found = -1;
	int n, j;

	/* 6.6 check_task_name()：名字 ≥ TASK_COMM_LEN 永远匹配不到任何线程 */
	if (name_len == 0 || name_len >= TASK_COMM_LEN)
		return -1;
	if (!g_related_threads || !g_total_num)
		return -1;
	/* 6.6 check_task_name() 还要求渲染名单有效 */
	if (g_have_valid_rp && !READ_ONCE(*g_have_valid_rp))
		return -1;

	if (g_rt_lock)
		read_lock_irqsave(g_rt_lock, flags);

	n = READ_ONCE(*g_total_num);
	if (n <= 0 || n > MAX_TID_COUNT)
		goto out;	/* 越界说明符号取错了，宁可不显示 */

	for (j = 0; j < n; j++) {
		const struct rrt_entry *e = &g_related_threads[j];
		struct task_struct *t = READ_ONCE(e->task);
		pid_t pid;

		if (!t)
			continue;
		if (strncmp(name, t->comm, name_len) != 0)
			continue;

		pid = e->pid;
		if (*last_pid != -1 && *last_pid == pid)
			continue;	/* 已被另一个槽占用，继续找下一个 */
		*last_pid = pid;
		found = pid;
		break;
	}
out:
	if (g_rt_lock)
		read_unlock_irqrestore(g_rt_lock, flags);
	return found;
}

/*
 * 解析两个槽的 pid。解析顺序照 6.6 update_critical_task_pids()：先槽 1、再槽 0
 * （这样去重时优先保住槽 1 的匹配）。
 * 特例：两个名字相同时（单名写入就是这种），两槽本来就指向同一个线程，
 * 如实显示同一个 pid —— 而不是像 6.6 那样把槽 0 去重成 -1。
 */
static void resolve_pids(const char *n0, const char *n1, pid_t *out)
{
	pid_t last = -1;

	out[0] = -1;
	out[1] = -1;

	if (strcmp(n0, n1) == 0) {
		out[0] = find_pid_by_name(n0, &last);
		out[1] = out[0];
		return;
	}
	out[1] = find_pid_by_name(n1, &last);
	out[0] = find_pid_by_name(n0, &last);
}

/* ------------------------------------------------------------------ */
/* proc 节点                                                           */
/* ------------------------------------------------------------------ */

static bool is_space_char(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
	       c == '\v' || c == '\f';
}

static int parse_one_name(const char **cursor, char *out)
{
	const char *start;
	const char *p = *cursor;
	size_t len;

	while (*p && is_space_char(*p))
		p++;
	if (!*p)
		return -EINVAL;
	start = p;
	while (*p && !is_space_char(*p))
		p++;
	len = p - start;
	if (!len)
		return -EINVAL;
	if (len >= CT_NAME_LEN)
		return -ENAMETOOLONG;
	memcpy(out, start, len);
	out[len] = '\0';
	*cursor = p;
	return 0;
}

static int parse_names(const char __user *ubuf, size_t count,
		       char out[CT_NUM][CT_NAME_LEN])
{
	char input[CT_INPUT_LEN];
	const char *cursor;
	const char *after_first;
	int ret;

	if (!count || count >= sizeof(input))
		return -E2BIG;
	if (copy_from_user(input, ubuf, count))
		return -EFAULT;
	if (memchr(input, '\0', count))
		return -EINVAL;
	input[count] = '\0';

	/* 第一个名字必须有 */
	cursor = input;
	ret = parse_one_name(&cursor, out[0]);
	if (ret)
		return ret;

	/*
	 * 第二个名字**可选**：只写一个时复制到第二个槽（理由见文件头 3）。
	 * 三个及以上仍然拒绝 —— 多出来的字符会被下面的检查发现。
	 */
	after_first = cursor;
	ret = parse_one_name(&cursor, out[1]);
	if (ret) {
		/* 没有第二个：复制第一个，游标退回原位，好让后面
		 * 「还有没有多余字符」的检查从正确位置继续 */
		cursor = after_first;
		strscpy(out[1], out[0], CT_NAME_LEN);
	}

	while (*cursor && is_space_char(*cursor))
		cursor++;
	if (*cursor)
		return -EINVAL;
	return 0;
}

static ssize_t critical_task_name_read(struct file *file, char __user *ubuf,
				       size_t count, loff_t *ppos)
{
	/* 官方 6.6 读取格式是 "%s:%d,%s:%d\n"；99 字符名字 * 2 + pid 展开
	 * 最长约 224 字节，256 够放（官方源码用 128 其实是会溢出的）。 */
	char page[CT_INPUT_LEN];
	char first[CT_NAME_LEN];
	char second[CT_NAME_LEN];
	pid_t pids[CT_NUM];
	int len;

	mutex_lock(&patch_lock);
	if (!critical_task ||
	    copy_kernel_string(READ_ONCE(critical_task[0]), first,
			       sizeof(first)) ||
	    copy_kernel_string(READ_ONCE(critical_task[1]), second,
			       sizeof(second))) {
		mutex_unlock(&patch_lock);
		return -EFAULT;
	}
	/*
	 * pid 现算（不是写死 -1）：数据源是 6.1 自己的 related_threads[]，
	 * 解析规则照 6.6 的 update_critical_task_pids()。没有游戏在跑、
	 * 或名字匹配不到线程时就是 -1 —— 和 6.6 写入后的初始值一致。
	 */
	resolve_pids(first, second, pids);
	len = scnprintf(page, sizeof(page), "%s:%d,%s:%d\n",
			first, (int)pids[0], second, (int)pids[1]);
	mutex_unlock(&patch_lock);

	return simple_read_from_buffer(ubuf, count, ppos, page, len);
}

static ssize_t critical_task_name_write(struct file *file,
					const char __user *ubuf, size_t count,
					loff_t *ppos)
{
	char new_name[CT_NUM][CT_NAME_LEN];
	int ret;
	int i;

	if (*ppos != 0)
		return -EINVAL;

	/* 与官方 6.6 写入语义一致：两个空格分隔的名字；本模块额外接受单名。 */
	ret = parse_names(ubuf, count, new_name);
	if (ret)
		return ret;

	mutex_lock(&patch_lock);
	if (!critical_task_rw) {
		mutex_unlock(&patch_lock);
		return -ENODEV;
	}

	/*
	 * 换指针前先等一个 RCU 宽限期，保证上一次（N-2）发布出去的那块
	 * 缓冲已经没人用了，接下来覆写它才安全。
	 */
	if (patched)
		synchronize_rcu();

	for (i = 0; i < CT_NUM; i++) {
		u8 next = active_slot[i] ^ 1;

		strscpy(name_buf[i][next], new_name[i], CT_NAME_LEN);
		/* 先填内容、再发布指针，读者才可能读到完整的新串。 */
		smp_wmb();
		WRITE_ONCE(critical_task_rw[i], name_buf[i][next]);
		active_slot[i] = next;
	}
	patched = true;
	mutex_unlock(&patch_lock);

	/*
	 * 每一次改动都留一行 dmesg（含 pid）：排查「谁在什么时候改了名单、改成了
	 * 什么、当时匹配到哪个线程」时，dmesg | grep ctn_patch 一条命令看全，
	 * 和 ctnd 写的 /dev/kmsg 同一条时间线。写入频率很低（一个游戏会话两次），
	 * 不会刷屏。
	 */
	{
		pid_t pids[CT_NUM];

		resolve_pids(new_name[0], new_name[1], pids);
		pr_info("ctn_patch: 名单更新 [%s:%d] [%s:%d]\n",
			new_name[0], (int)pids[0], new_name[1], (int)pids[1]);
	}

	*ppos = count;
	return count;
}

static const struct proc_ops critical_task_name_proc_ops = {
	.proc_read = critical_task_name_read,
	.proc_write = critical_task_name_write,
	.proc_lseek = default_llseek,
};

/* ------------------------------------------------------------------ */
/* 恢复                                                                */
/* ------------------------------------------------------------------ */

static void restore_pointers(void)
{
	int i;

	if (!patched || !critical_task_rw)
		return;

	for (i = 0; i < CT_NUM; i++)
		WRITE_ONCE(critical_task_rw[i], original_name[i]);

	/* 读者跑在 sched_switch tracepoint 的 RCU 读侧临界区里。 */
	synchronize_rcu();
	patched = false;
}

/* ------------------------------------------------------------------ */
/* init / exit                                                         */
/* ------------------------------------------------------------------ */

static void resolve_pid_source(void)
{
	unsigned long addr;

	g_related_threads = NULL;
	g_total_num = NULL;
	g_have_valid_rp = NULL;
	g_rt_lock = NULL;

	addr = lookup_name(SYM_RELATED_THREADS);
	if (addr)
		g_related_threads = (struct rrt_entry *)addr;

	addr = lookup_name(SYM_TOTAL_NUM);
	if (addr)
		g_total_num = (int *)addr;

	addr = lookup_name(SYM_HAVE_VALID_RP);
	if (addr)
		g_have_valid_rp = (int *)addr;

	addr = lookup_name(SYM_RT_LOCK);
	if (addr)
		g_rt_lock = (rwlock_t *)addr;

	if (g_related_threads && g_total_num) {
		pr_info("ctn_patch: pid 数据源已就位（related_threads + total_num）\n");
	} else {
		/* 不致命：节点功能照常，只是读出来的 pid 一直是 -1 */
		pr_warn("ctn_patch: 取不到 related_threads/total_num，pid 将显示 -1\n");
	}
}

static int __init ctn_patch_init(void)
{
	unsigned long addr;
	unsigned long offset;
	struct page *page;
	const char *name;
	int ret;
	int i;

	BUILD_BUG_ON(sizeof(struct rrt_entry) != 24);

	ret = resolve_lookup_name();
	if (ret) {
		pr_err("ctn_patch: kallsyms_lookup_name 不可用: %d\n", ret);
		return ret;
	}

	find_module_fn = (void *)lookup_name("find_module");
	if (!find_module_fn) {
		pr_err("ctn_patch: 找不到 find_module\n");
		return -ENOENT;
	}

	/* 非导出符号，只能运行时取（见 copy_kernel_string 上方说明）。 */
	copy_from_kernel_nofault_fn =
		(void *)lookup_name("copy_from_kernel_nofault");
	if (!copy_from_kernel_nofault_fn)
		pr_warn("ctn_patch: 取不到 copy_from_kernel_nofault，退回 memcpy 读串\n");

	/* 先把目标模块钉住，避免它在后面被 rmmod 造成野指针。 */
	victim = find_module_fn(VICTIM_MODULE);
	if (!victim) {
		pr_err("ctn_patch: 模块 %s 未加载，请先 insmod 它\n",
		       VICTIM_MODULE);
		return -ENOENT;
	}
	if (!try_module_get(victim)) {
		pr_err("ctn_patch: try_module_get(%s) 失败\n", VICTIM_MODULE);
		victim = NULL;
		return -ENOENT;
	}

	/* module:symbol 形式：内核 kallsyms_lookup_name 末尾会落到
	 * module_kallsyms_lookup_name()，它按 ':' 拆成模块名+符号名。 */
	addr = lookup_name(SYM_CRITICAL_TASK);
	if (!addr) {
		pr_err("ctn_patch: 找不到 %s\n", SYM_CRITICAL_TASK);
		ret = -ENOENT;
		goto fail_put;
	}
	critical_task = (const char **)addr;

	/*
	 * 布局校验：6.1 是「两个指针」，6.6 是内联 char[2][100]
	 * （6.6 源码 critical_task_boost.c:78）。
	 * 新版里 critical_task[0] 的低字节是 'U' 之类，不会是内核指针，
	 * 用上半区判断即可拒掉，避免把新内核改坏。
	 */
	name = READ_ONCE(critical_task[0]);
	if (!kernel_address_looks_valid(name)) {
		pr_err("ctn_patch: critical_task[0] 不是内核指针，可能是新布局，放弃\n");
		ret = -EINVAL;
		goto fail_put;
	}
	name = READ_ONCE(critical_task[1]);
	if (!kernel_address_looks_valid(name)) {
		pr_err("ctn_patch: critical_task[1] 不是内核指针，可能是新布局，放弃\n");
		ret = -EINVAL;
		goto fail_put;
	}

	addr = lookup_name(SYM_TASK_BOOST_DIR);
	if (!addr) {
		pr_err("ctn_patch: 找不到 %s\n", SYM_TASK_BOOST_DIR);
		ret = -ENOENT;
		goto fail_put;
	}
	task_boost_dir = READ_ONCE(*(struct proc_dir_entry **)addr);
	if (!task_boost_dir) {
		pr_err("ctn_patch: task_boost 目录尚未建立\n");
		ret = -ENOENT;
		goto fail_put;
	}

	/* 模块内存走 vmalloc；.rodata 被设成只读，所以要另建可写别名。 */
	if (!is_vmalloc_addr(critical_task)) {
		pr_err("ctn_patch: critical_task 不在 vmalloc 区\n");
		ret = -EFAULT;
		goto fail_put;
	}
	offset = (unsigned long)critical_task & ~PAGE_MASK;
	if (offset + CT_PTR_ARRAY_BYTES > PAGE_SIZE) {
		pr_err("ctn_patch: 指针数组跨页，本模块不支持\n");
		ret = -EINVAL;
		goto fail_put;
	}
	page = vmalloc_to_page(critical_task);
	if (!page) {
		ret = -EFAULT;
		goto fail_put;
	}
	critical_task_vmap = vmap(&page, 1, VM_MAP, PAGE_KERNEL);
	if (!critical_task_vmap) {
		ret = -ENOMEM;
		goto fail_put;
	}
	critical_task_rw = (const char **)((char *)critical_task_vmap + offset);

	/* 信息性检查：符号是否落在目标模块 core 区里（头文件不一致时只告警）。 */
	if ((unsigned long)critical_task < (unsigned long)victim->core_layout.base ||
	    (unsigned long)critical_task >= (unsigned long)victim->core_layout.base +
					     victim->core_layout.size)
		pr_warn("ctn_patch: critical_task 不在 %s 的 core 区，仅告警\n",
			VICTIM_MODULE);

	for (i = 0; i < CT_NUM; i++) {
		original_name[i] = READ_ONCE(critical_task[i]);
		ret = copy_kernel_string(original_name[i], name_buf[i][0],
					 sizeof(name_buf[i][0]));
		if (ret) {
			pr_err("ctn_patch: 读取原始名字失败 (%d)\n", ret);
			goto fail_unmap;
		}
	}

	resolve_pid_source();

	/* 如果这个内核本来就有这个节点，proc_create_data 会失败，正好不重复建。 */
	name_entry = proc_create_data("critical_task_name", 0664, task_boost_dir,
				      &critical_task_name_proc_ops, NULL);
	if (!name_entry) {
		pr_err("ctn_patch: 创建 critical_task_name 失败（可能已存在）\n");
		ret = -EEXIST;
		goto fail_unmap;
	}

	pr_info("ctn_patch: 已为 %s 补上 critical_task_name，当前 [%s] [%s]\n",
		VICTIM_MODULE, name_buf[0][0], name_buf[1][0]);
	return 0;

fail_unmap:
	if (critical_task_vmap) {
		vunmap(critical_task_vmap);
		critical_task_vmap = NULL;
	}
	critical_task_rw = NULL;
	critical_task = NULL;
fail_put:
	if (victim) {
		module_put(victim);
		victim = NULL;
	}
	return ret;
}

static void __exit ctn_patch_exit(void)
{
	if (name_entry) {
		proc_remove(name_entry);
		name_entry = NULL;
	}

	mutex_lock(&patch_lock);
	restore_pointers();
	mutex_unlock(&patch_lock);

	if (critical_task_vmap) {
		vunmap(critical_task_vmap);
		critical_task_vmap = NULL;
	}
	critical_task_rw = NULL;
	critical_task = NULL;

	g_related_threads = NULL;
	g_total_num = NULL;
	g_have_valid_rp = NULL;

	if (victim) {
		module_put(victim);
		victim = NULL;
	}

	pr_info("ctn_patch: 已卸载\n");
}

module_init(ctn_patch_init);
module_exit(ctn_patch_exit);

/* 本模块以 GPL-2.0-only 发布（见 SPDX 头与 LICENSE）。
 * 内核的 license_is_gpl_compatible() 把 "GPL" 认作 GPL-2.0，这是内核模块的
 * 通用写法；同时这样才拿得到 register_kprobe / synchronize_rcu 这些
 * EXPORT_SYMBOL_GPL 符号。 */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Turbo");
MODULE_DESCRIPTION("Add /proc/game_opt/task_boost/critical_task_name for old Oplus game_opt");
MODULE_VERSION("2.1");
MODULE_SOFTDEP("pre: " VICTIM_MODULE);
