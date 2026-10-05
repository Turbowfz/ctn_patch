// SPDX-License-Identifier: GPL-2.0-only
/*
 * ctn_patch - Copyright (c) Turbo
 *
 * ctn_patch - 把官方 6.6 的 /proc/game_opt/task_boost/critical_task_name
 *             可写节点 backport 到 8Gen3 / 6.1 内核
 *
 * 背景（全部来自本工作区的设备实锤，非推测）：
 *   - 6.1 的 oplus_bsp_game_opt.ko 里关键线程名单机制是有的，就是
 *       static const char* critical_task[2] = { "UnityMain", "UnityGfxDevice" };
 *     名单本身写死成这两个 Unity 名字（kallsyms 里是
 *       r critical_task [oplus_bsp_game_opt]）。
 *   - 6.1 没有把它暴露成可写节点：模块只建了 5 个 task_boost 子节点
 *       ct_enable / expire_time_percentage / target_fps / htb_strategy / htb_enable
 *     （ko 里字符串 "critical_task_name" 出现 0 次）。
 *   - 6.6 官方源码（本工作区 6.6_一加13/风驰6.6源码/.../critical_task_boost.c）
 *     把它做成了可写节点，第 78 行起：
 *       static char critical_task[2][100] = {"UnityMain", "UnityGfxDevice"};
 *       static pid_t critical_task_pids[2] = {-1, -1};
 *     写入（:602）：sscanf(page, "%99s %99s", ...)，必须恰好两个名字，否则 -EINVAL，
 *       写入后把两个 pid 重置为 -1；
 *     读取（:631）：sprintf(page, "%s:%d,%s:%d\n", 名字, pid, 名字, pid)。
 *     本模块的读写格式与这份官方实现逐字对齐，所以给 6.6 写的工具/脚本原样可用。
 *
 * 内核侧消费点（6.1 源码实锤 src_original_modules/.../critical_task_boost.c:349）：
 *   static void update_critical_task_time(struct task_struct *task, int i, bool prev)
 *   {
 *       if (task && strncmp(task->comm, critical_task[i],
 *                           strlen(critical_task[i])) == 0) { ... }
 *   }
 *   它由 register_trace_sched_switch(sched_switch_hook, NULL) 注册，
 *   即跑在 sched_switch tracepoint 里；6.1 的 __DO_TRACE 用
 *   rcu_read_lock_sched_notrace()/rcu_read_unlock_sched_notrace() 包住回调，
 *   所以 synchronize_rcu() 能等到这些读者退出 —— 这是本模块双缓冲换指针的安全前提。
 *
 * 安全性要点：
 *   1. 6.1 的 critical_task 在 .rodata（只读）。直接写会触发内核写保护异常，
 *      所以用 vmap(&page, 1, VM_MAP, PAGE_KERNEL) 建一个指向同一物理页的
 *      可写别名，只通过这个别名改数组内容（6.6 是内联 char[2][100] 在可写段，
 *      所以官方能直接 sscanf 写进去；6.1 是指针数组在只读段，必须走别名）。
 *   2. 换指针时用「双缓冲 + synchronize_rcu()」：
 *      第 N 次写入前先 synchronize_rcu()，确保第 N-2 次发布出去的那块
 *      缓冲已经没有读者，再覆写它。
 *   3. 模块加载时 try_module_get() 抓住 oplus_bsp_game_opt，
 *      防止它先被 rmmod 导致本模块手里的符号/目录指针变成野指针。
 *
 * 已知差异（与 6.6 相比）：
 *   - 6.6 读取时能显示两个关键线程的真实 pid（update_ctb_pids 维护）；
 *     6.1 内核没有这个数据（critical_task_pids/update_ctb_pids 均 0 命中），
 *     所以本模块固定显示 -1（和 6.6 写入后的初始值一致）。
 *   - 超过 15 字符的名字 6.6 也接受，但永远匹配不到任何线程
 *     （task->comm 只有 16 字节含 NUL，strncmp 走到 comm 的 NUL 就停），
 *     实际要让 boost 生效请用 15 字符以内的名字。
 *
 * 加载顺序：必须 oplus_bsp_game_opt 先加载（开机就在），
 *           卸载顺序相反（先 rmmod ctn_patch，再考虑动 game_opt）。
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#define CT_NUM			2
/* 官方 6.6 用 char[2][100]、写入上限 "%99s"：名字最长 99 字符 + NUL。 */
#define CT_NAME_LEN		100
#define CT_INPUT_LEN		256
#define CT_PTR_ARRAY_BYTES	(CT_NUM * sizeof(const char *))

#define VICTIM_MODULE		"oplus_bsp_game_opt"
#define GAME_OPT_CRITICAL_TASK	VICTIM_MODULE ":critical_task"
#define GAME_OPT_TASK_BOOST_DIR	VICTIM_MODULE ":critical_heavy_boost_dir"

static const char **critical_task;	/* 只读映射里的原数组（用于读当前值） */
static const char **critical_task_rw;	/* 同一物理页的可写别名 */
static void *critical_task_vmap;
static struct proc_dir_entry *task_boost_dir;
static struct proc_dir_entry *name_entry;
static bool patched;

static const char *original_name[CT_NUM];
static char name_buf[CT_NUM][2][CT_NAME_LEN];
static u8 active_slot[CT_NUM];

static struct module *victim;

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
	int i;

	if (!count || count >= sizeof(input))
		return -E2BIG;
	if (copy_from_user(input, ubuf, count))
		return -EFAULT;
	if (memchr(input, '\0', count))
		return -EINVAL;
	input[count] = '\0';

	cursor = input;
	for (i = 0; i < CT_NUM; i++) {
		int ret = parse_one_name(&cursor, out[i]);

		if (ret)
			return ret;
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
	 * 6.1 内核没有 critical_task_pids / update_ctb_pids（全仓 0 命中），
	 * pid 无法知道，固定输出 -1 —— 正好等于 6.6 写入后的初始值。
	 */
	len = scnprintf(page, sizeof(page), "%s:%d,%s:%d\n",
			first, -1, second, -1);
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

	/* 与官方 6.6 写入语义一致：必须恰好两个空格分隔的名字，否则 -EINVAL。 */
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

static int __init ctn_patch_init(void)
{
	unsigned long addr;
	unsigned long offset;
	struct page *page;
	const char *name;
	int ret;
	int i;

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
	addr = lookup_name(GAME_OPT_CRITICAL_TASK);
	if (!addr) {
		pr_err("ctn_patch: 找不到 %s\n", GAME_OPT_CRITICAL_TASK);
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

	addr = lookup_name(GAME_OPT_TASK_BOOST_DIR);
	if (!addr) {
		pr_err("ctn_patch: 找不到 %s\n", GAME_OPT_TASK_BOOST_DIR);
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

	/*
	 * critical_task_rw 必须在持锁内置空：write 路径在锁内判
	 * !critical_task_rw，若在锁外先 vunmap 再置空，写者可能拿到锁时
	 * 看到 rw 还非空、别名却已注销，往悬空映射上写。
	 */
	mutex_lock(&patch_lock);
	restore_pointers();
	critical_task_rw = NULL;	/* 此后 write 一律 -ENODEV */
	mutex_unlock(&patch_lock);

	if (critical_task_vmap) {
		vunmap(critical_task_vmap);
		critical_task_vmap = NULL;
	}
	critical_task = NULL;

	if (victim) {
		module_put(victim);
		victim = NULL;
	}

	pr_info("ctn_patch: 已卸载\n");
}

module_init(ctn_patch_init);
module_exit(ctn_patch_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Turbo");
MODULE_DESCRIPTION("Add /proc/game_opt/task_boost/critical_task_name for old Oplus game_opt");
MODULE_VERSION("1.0");
MODULE_SOFTDEP("pre: " VICTIM_MODULE);
