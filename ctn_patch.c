// SPDX-License-Identifier: GPL-2.0-only
/*
 * ctn_patch - Copyright (c) Turbo
 *
 * 把官方 6.6（一加13）的 /proc/game_opt/task_boost/critical_task_name
 * 移植到 8Gen3 / 6.1 内核（一加 Ace3Pro / Ace5 / 一加12 / GT6）。
 *
 * 移植来源：6.6 critical_task_boost.c 的两槽名字、99字节上限和
 * "%s:%d,%s:%d\n" 读取格式；单名仍复制到两槽，第三名和嵌入 NUL 拒绝。
 * 6.1 critical_task 是 .rodata 中的 const char *[2]，通过物理页的
 * vmap 可写别名发布名字指针，不修改任何调度/频率控制或热路径 hook。
 *
 * 生命周期证据（不能只凭这个参考源码给未知固件放行）：
 * 6.1 vendor/oplus/kernel/cpu/game_opt/critical_task_boost.c:349-401
 * 的 update_critical_task_time 由 sched_switch_hook 调用并注册到
 * register_trace_sched_switch；两个已提取 vendor ELF 的 critical_task
 * .text 重定位仅指向这个读者。include/linux/tracepoint.h:184-214
 * 的 __DO_TRACE 以 preempt_disable_notrace 包住调用；Linux >=5.0 的
 * synchronize_rcu 也等待 preempt-disabled 区（rcupdate.h:760-762）。
 * 因而宽限期后才能复用双缓冲/卸载；两槽各自 release 发布，不是原子成对。
 * profile 必须显式确认这些读者证据及动态函数 KCFI；缺证据拒绝启动。
 *
 * PID 仅是冷路径显示：rt_info.c:104-153 追加的任务未全持有引用，故即使
 * 拿了 rt_info_rwlock 也不能裸读 entry->task。先在 RCU 中按 pid 查找，
 * 核对指针身份及 task->pid，再 nofault 读 comm；profile/锁未证实则显示 -1。
 *
 * v3.0 起按 profile 双基线构建：6.1.141 与 6.1.157 各自核对 ABI 后出独立的
 * ko，装载时再校验本内核身份。延迟重试保留：只重试尚未出现/尚未 LIVE 的
 * 目标模块，布局、原值、符号或 proc 创建等永久失败保持 patch_ready=false。
 */

#include <linux/atomic.h>
#include <linux/build_bug.h>
#include <linux/elf.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/pid_namespace.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/utsname.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#define INCLUDE_VERMAGIC
#include <linux/vermagic.h>
#undef INCLUDE_VERMAGIC

#include "ctn_profile.h"
#include "ctn_patch_internal.h"

#if !defined(CONFIG_ARM64) || !defined(CONFIG_CFI_CLANG) || \
    !defined(CONFIG_KALLSYMS_ALL) || !defined(CONFIG_MODULE_UNLOAD)
#error "ctn_patch requires the verified arm64 KCFI/module-unload/kallsyms-all ABI"
#endif
#if !defined(CTN_PROFILE_ID) || !defined(CTN_BUILD_ID) || \
    !defined(CTN_RUNTIME_RELEASE) || !defined(CTN_MODULE_VERMAGIC) || \
    !defined(CTN_EXPECT_MODULE_SIZE) || !defined(CTN_EXPECT_TASK_COMM_OFFSET) || \
    !defined(CTN_EXPECT_TASK_PID_OFFSET) || !defined(CTN_EXPECT_RRT_SIZE) || \
    !defined(CTN_EXPECT_RT_LOCK_SIZE) || !defined(CTN_EXPECT_CRITICAL_TASK_BYTES) || \
    !defined(CTN_EXPECT_DEFAULT0) || !defined(CTN_EXPECT_DEFAULT1) || \
    !defined(CTN_PID_SOURCE_VERIFIED) || !defined(CTN_NAME_READERS_RCU_VERIFIED)
#error "ctn_profile.h must contain the complete verified profile contract"
#endif

#define VICTIM_MODULE		"oplus_bsp_game_opt"
#define MAX_TID_COUNT		256
#define CT_PTR_ARRAY_BYTES	(CT_NUM * sizeof(const char *))
#define PROBE_RETRY_MS		10000
#define PROBE_MAX_TRIES		60
#define CT_SYMBOL_NAME_LEN	128

struct rrt_entry {
	pid_t pid;
	struct task_struct *task;
	u32 wake_count;
};

static const char **critical_task;
static const char **critical_task_rw;
static void *critical_task_vmap;
static struct proc_dir_entry *name_entry;
static struct module *victim;
static struct module_layout victim_layout;
static struct mod_kallsyms victim_kallsyms;
static size_t victim_strtab_bytes;
static const char *original_name[CT_NUM];
static char name_buf[CT_NUM][2][CT_NAME_LEN];
static u8 active_slot[CT_NUM];
static bool patched;
static bool patch_ready;
static bool pid_display_available;
static bool stopping;
static DEFINE_MUTEX(patch_lock);
static struct delayed_work probe_work;
static unsigned int probe_tries;

static struct rrt_entry *g_related_threads;
static int *g_total_num;
static atomic_t *g_have_valid_rp;
static rwlock_t *g_rt_lock;
static struct mutex *module_mutex_ptr;
static unsigned long (*lookup_name)(const char *name);
static struct module *(*find_module_fn)(const char *name);
static long (*copy_from_kernel_nofault_fn)(void *dst, const void *src, size_t size);
static struct task_struct *(*find_task_by_pid_ns_fn)(pid_t pid, struct pid_namespace *ns);
static struct pid_namespace *initial_pid_namespace;

/* Reject module-load overrides too: 0444 alone does not prevent insmod args. */
static int ctn_param_reject(const char *value, const struct kernel_param *kp)
{
	return -EPERM;
}

static int ctn_param_get_string(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", (const char *)kp->arg);
}

static int ctn_param_get_bool(char *buf, const struct kernel_param *kp)
{
	const bool *value = kp->arg;

	return scnprintf(buf, PAGE_SIZE, "%c\n", smp_load_acquire(value) ? 'Y' : 'N');
}

static const struct kernel_param_ops ctn_string_param_ops = {
	.set = ctn_param_reject,
	.get = ctn_param_get_string,
};
static const struct kernel_param_ops ctn_bool_param_ops = {
	.set = ctn_param_reject,
	.get = ctn_param_get_bool,
};
module_param_cb(profile, &ctn_string_param_ops, (void *)CTN_PROFILE_ID, 0444);
module_param_cb(build_id, &ctn_string_param_ops, (void *)CTN_BUILD_ID, 0444);
module_param_cb(patch_ready, &ctn_bool_param_ops, &patch_ready, 0444);
module_param_cb(pid_display_available, &ctn_bool_param_ops,
		&pid_display_available, 0444);
MODULE_PARM_DESC(profile, "Immutable build profile identity");
MODULE_PARM_DESC(build_id, "Immutable source/profile/compiler build identity");
MODULE_PARM_DESC(patch_ready, "Validated critical_task_name node is ready");
MODULE_PARM_DESC(pid_display_available, "Verified locked PID source is available");

/* Accept any build of the same numeric kernel series (6.1.141/6.1.157):
 * the running release must start with the profile id followed by '-'. */
static bool ctn_release_matches_profile(const char *release)
{
	const size_t n = sizeof(CTN_PROFILE_ID) - 1;

	return release && !strncmp(release, CTN_PROFILE_ID, n) && release[n] == '-';
}

static int ctn_validate_profile(void)
{
	/* These are BTF-derived fields, not a guessed struct-module byte prefix. */
	BUILD_BUG_ON(sizeof(struct module) != CTN_EXPECT_MODULE_SIZE);
	BUILD_BUG_ON(offsetof(struct module, state) != CTN_EXPECT_MODULE_STATE_OFFSET);
	BUILD_BUG_ON(offsetof(struct module, name) != CTN_EXPECT_MODULE_NAME_OFFSET);
	BUILD_BUG_ON(offsetof(struct module, core_layout) !=
		     CTN_EXPECT_MODULE_CORE_LAYOUT_OFFSET);
	BUILD_BUG_ON(offsetof(struct module, core_kallsyms) !=
		     CTN_EXPECT_MODULE_CORE_KALLSYMS_OFFSET);
	BUILD_BUG_ON(sizeof(struct module_layout) != CTN_EXPECT_MODULE_LAYOUT_SIZE);
	BUILD_BUG_ON(offsetof(struct module_layout, base) !=
		     CTN_EXPECT_MODULE_LAYOUT_BASE_OFFSET);
	BUILD_BUG_ON(offsetof(struct module_layout, size) !=
		     CTN_EXPECT_MODULE_LAYOUT_SIZE_OFFSET);
	BUILD_BUG_ON(offsetof(struct module_layout, text_size) !=
		     CTN_EXPECT_MODULE_LAYOUT_TEXT_SIZE_OFFSET);
	BUILD_BUG_ON(offsetof(struct module_layout, ro_size) !=
		     CTN_EXPECT_MODULE_LAYOUT_RO_SIZE_OFFSET);
	BUILD_BUG_ON(offsetof(struct module_layout, ro_after_init_size) !=
		     CTN_EXPECT_MODULE_LAYOUT_RO_AFTER_INIT_SIZE_OFFSET);
	BUILD_BUG_ON(sizeof(struct mod_kallsyms) != CTN_EXPECT_MOD_KALLSYMS_SIZE);
	BUILD_BUG_ON(offsetof(struct mod_kallsyms, symtab) !=
		     CTN_EXPECT_MOD_KALLSYMS_SYMTAB_OFFSET);
	BUILD_BUG_ON(offsetof(struct mod_kallsyms, num_symtab) !=
		     CTN_EXPECT_MOD_KALLSYMS_NUM_SYMTAB_OFFSET);
	BUILD_BUG_ON(offsetof(struct mod_kallsyms, strtab) !=
		     CTN_EXPECT_MOD_KALLSYMS_STRTAB_OFFSET);
	BUILD_BUG_ON(offsetof(struct mod_kallsyms, typetab) !=
		     CTN_EXPECT_MOD_KALLSYMS_TYPETAB_OFFSET);
	/* task_struct may have a dynamic tail on 157; only consumed fields match. */
	BUILD_BUG_ON(offsetof(struct task_struct, comm) != CTN_EXPECT_TASK_COMM_OFFSET);
	BUILD_BUG_ON(offsetof(struct task_struct, pid) != CTN_EXPECT_TASK_PID_OFFSET);
	BUILD_BUG_ON(sizeof(struct rrt_entry) != CTN_EXPECT_RRT_SIZE);
	BUILD_BUG_ON(offsetof(struct rrt_entry, task) != 8);
	BUILD_BUG_ON(offsetof(struct rrt_entry, wake_count) != 16);
	BUILD_BUG_ON(sizeof(rwlock_t) != CTN_EXPECT_RT_LOCK_SIZE);
	BUILD_BUG_ON(sizeof(atomic_t) != 4);
	BUILD_BUG_ON(sizeof(int) != 4 || sizeof(void *) != 8 || TASK_COMM_LEN != 16);
	BUILD_BUG_ON(CT_PTR_ARRAY_BYTES != CTN_EXPECT_CRITICAL_TASK_BYTES);
	BUILD_BUG_ON(CTN_PID_SOURCE_VERIFIED != 0 && CTN_PID_SOURCE_VERIFIED != 1);
	BUILD_BUG_ON(CTN_NAME_READERS_RCU_VERIFIED != 0 &&
		     CTN_NAME_READERS_RCU_VERIFIED != 1);
	BUILD_BUG_ON(sizeof(CTN_EXPECT_DEFAULT0) > CT_NAME_LEN ||
		     sizeof(CTN_EXPECT_DEFAULT1) > CT_NAME_LEN);

	if (!CTN_PROFILE_ID[0] || !CTN_BUILD_ID[0] || !CTN_EXPECT_DEFAULT0[0] ||
	    !CTN_EXPECT_DEFAULT1[0] || !CTN_NAME_READERS_RCU_VERIFIED)
		return -EPROTONOSUPPORT;
	/*
	 * Numeric profile gate only: same-series 6.1.141/6.1.157 builds load and
	 * the kernel's own MODVERSIONS CRC checks stay the ABI gate at insmod.
	 * The vermagic self-check is build-time consistency, not a host comparison.
	 */
	if (!ctn_release_matches_profile(utsname()->release) ||
	    !ctn_release_matches_profile(init_utsname()->release) ||
	    strcmp(VERMAGIC_STRING, CTN_MODULE_VERMAGIC))
		return -ENOEXEC;
	return 0;
}

static bool ctn_kernel_address(const void *ptr)
{
	return ptr && (long)(unsigned long)ptr < 0;
}

/*
 * Non-circular bootstrap: exception-table loads, no unverified indirect call.
 * This is NOT a memcpy fallback if copy_from_kernel_nofault is unavailable.
 * Only the three KCFI type words are bootstrapped through this primitive.
 */
static int ctn_bootstrap_type(const void *fn, u32 *type)
{
	const u32 *source;
	u32 value = 0;
	int err = 0;

	if (!ctn_kernel_address(fn) || ((unsigned long)fn & 3))
		return -EINVAL;
	source = (const u32 *)((unsigned long)fn - sizeof(*type));
	pagefault_disable();
	/* Code pages have no allocation tag; keep fault fixup without MTE data helpers. */
	__raw_get_mem("ldr", value, source, err, K);
	pagefault_enable();
	if (err)
		return -EFAULT;
	*type = value;
	return 0;
}

static noinline __used unsigned long ctn_lookup_stub(const char *name)
{
	return 0;
}
static noinline __used struct module *ctn_find_module_stub(const char *name)
{
	return NULL;
}
static noinline __used long ctn_nofault_stub(void *dst, const void *src, size_t size)
{
	return -EFAULT;
}
static noinline __used struct task_struct *ctn_pid_ns_stub(pid_t pid,
						      struct pid_namespace *ns)
{
	return NULL;
}

static int ctn_validate_kcfi(const void *live, const void *local)
{
	u32 live_type, local_type;

	if (ctn_bootstrap_type(live, &live_type) ||
	    ctn_bootstrap_type(local, &local_type))
		return -EFAULT;
	if (!live_type || live_type != local_type)
		return -EPROTOTYPE;
	return 0;
}

static int ctn_probe_address(const char *symbol, void **address)
{
	struct kprobe probe = { .symbol_name = symbol };
	int ret;

	ret = register_kprobe(&probe);
	if (ret)
		return ret;
	*address = (void *)probe.addr;
	unregister_kprobe(&probe);
	return *address ? 0 : -ENOENT;
}

static int ctn_resolve_functions(void)
{
	void *lookup, *nofault, *finder;
	char probe_byte;
	int ret;

	ret = ctn_probe_address("kallsyms_lookup_name", &lookup);
	if (ret)
		return ret;
	ret = ctn_probe_address("copy_from_kernel_nofault", &nofault);
	if (ret)
		return ret;
	ret = ctn_validate_kcfi(lookup, ctn_lookup_stub);
	if (ret)
		return ret;
	ret = ctn_validate_kcfi(nofault, ctn_nofault_stub);
	if (ret)
		return ret;
	lookup_name = (void *)lookup;
	copy_from_kernel_nofault_fn = (void *)nofault;
	finder = (void *)lookup_name("find_module");
	ret = ctn_validate_kcfi(finder, ctn_find_module_stub);
	if (ret)
		return ret;
	find_module_fn = (void *)finder;
	module_mutex_ptr = (void *)lookup_name("module_mutex");
	if (!ctn_kernel_address(module_mutex_ptr) ||
	    (unsigned long)module_mutex_ptr % __alignof__(struct mutex) ||
	    copy_from_kernel_nofault_fn(&probe_byte, module_mutex_ptr, 1))
		return -EFAULT;

	/* Optional PID display uses the initial namespace, never the proc reader's. */
	finder = (void *)lookup_name("find_task_by_pid_ns");
	initial_pid_namespace = (void *)lookup_name("init_pid_ns");
	if (ctn_kernel_address(initial_pid_namespace) &&
	    (unsigned long)initial_pid_namespace % __alignof__(struct pid_namespace) == 0 &&
	    !copy_from_kernel_nofault_fn(&probe_byte, initial_pid_namespace, 1) &&
	    !ctn_validate_kcfi(finder, ctn_pid_ns_stub)) {
		find_task_by_pid_ns_fn = (void *)finder;
	} else {
		find_task_by_pid_ns_fn = NULL;
		initial_pid_namespace = NULL;
		pr_warn("ctn_patch: verified initial-namespace lookup unavailable; PID display disabled\n");
	}
	return 0;
}

static int ctn_nofault(void *dst, const void *src, size_t bytes)
{
	if (!copy_from_kernel_nofault_fn || !ctn_kernel_address(src) ||
	    !bytes || (unsigned long)src > ULONG_MAX - (bytes - 1))
		return -EFAULT;
	return copy_from_kernel_nofault_fn(dst, src, bytes) ? -EFAULT : 0;
}

/* Read exactly one byte at a time: never touch bytes beyond the first NUL. */
static int ctn_nofault_string(const char *src, char *dst, size_t capacity,
			      size_t available)
{
	size_t i;

	for (i = 0; i < capacity && i < available; i++) {
		if (ctn_nofault(dst + i, src + i, 1))
			return -EFAULT;
		if (!dst[i])
			return 0;
	}
	return -ENAMETOOLONG;
}

static bool ctn_in_range(const void *ptr, size_t bytes,
			 unsigned long base, size_t length)
{
	unsigned long address = (unsigned long)ptr;

	return bytes && length <= ULONG_MAX - base && address >= base &&
	       address - base < length && bytes <= length - (address - base);
}

static bool ctn_core_range(const void *ptr, size_t bytes)
{
	return ctn_in_range(ptr, bytes, (unsigned long)victim_layout.base,
			    victim_layout.size);
}

static bool ctn_rodata_range(const void *ptr, size_t bytes)
{
	return ctn_in_range(ptr, bytes, (unsigned long)victim_layout.base +
			    victim_layout.text_size,
			    victim_layout.ro_size - victim_layout.text_size);
}

static bool ctn_data_range(const void *ptr, size_t bytes)
{
	return ctn_in_range(ptr, bytes, (unsigned long)victim_layout.base +
			    victim_layout.ro_after_init_size,
			    victim_layout.size - victim_layout.ro_after_init_size);
}

static bool ctn_module_mapping(const void *ptr)
{
	unsigned long address = (unsigned long)ptr;

#ifdef MODULES_VADDR
	if (address >= MODULES_VADDR && address < MODULES_END)
		return true;
#endif
	return is_vmalloc_addr(ptr);
}

/* find and try_module_get form one transaction under the real module_mutex. */
static int ctn_get_victim(void)
{
	struct module *mod;
	enum module_state state;
	char name[MODULE_NAME_LEN];
	int ret = -EAGAIN;

	mutex_lock(module_mutex_ptr);
	mod = find_module_fn(VICTIM_MODULE);
	if (!mod)
		goto out;
	if (ctn_nofault(&state, &mod->state, sizeof(state)) ||
	    ctn_nofault_string(mod->name, name, sizeof(name), sizeof(mod->name))) {
		ret = -EFAULT;
		goto out;
	}
	if (strcmp(name, VICTIM_MODULE)) {
		ret = -ENOEXEC;
		goto out;
	}
	if (state != MODULE_STATE_LIVE)
		goto out;
	if (!try_module_get(mod))
		goto out;
	victim = mod;
	ret = 0;
out:
	mutex_unlock(module_mutex_ptr);
	return ret;
}

static int ctn_snapshot_layout(void)
{
	unsigned long base, strtab, typetab;
	size_t symtab_bytes;

	if (ctn_nofault(&victim_layout, &victim->core_layout, sizeof(victim_layout)))
		return -EFAULT;
	base = (unsigned long)victim_layout.base;
	if (!ctn_kernel_address(victim_layout.base) || (base & ~PAGE_MASK) ||
	    !victim_layout.text_size ||
	    victim_layout.text_size > victim_layout.ro_size ||
	    victim_layout.ro_size > victim_layout.ro_after_init_size ||
	    victim_layout.ro_after_init_size > victim_layout.size ||
	    victim_layout.size > ULONG_MAX - base ||
	    !ctn_module_mapping(victim_layout.base) ||
	    !ctn_module_mapping((void *)(base + victim_layout.size - 1)))
		return -ENOEXEC;
	/* core_kallsyms is permanent and was populated before vendor module_init. */
	if (ctn_nofault(&victim_kallsyms, &victim->core_kallsyms,
			sizeof(victim_kallsyms)))
		return -EFAULT;
	if (!victim_kallsyms.num_symtab ||
	    victim_kallsyms.num_symtab > victim_layout.size / sizeof(Elf_Sym))
		return -ENOEXEC;
	symtab_bytes = (size_t)victim_kallsyms.num_symtab * sizeof(Elf_Sym);
	strtab = (unsigned long)victim_kallsyms.strtab;
	typetab = (unsigned long)victim_kallsyms.typetab;
	/* add_kallsyms/layout_symtab place typetab directly after core strtab. */
	if (typetab <= strtab || !ctn_core_range(victim_kallsyms.symtab, symtab_bytes) ||
	    (unsigned long)victim_kallsyms.symtab % __alignof__(Elf_Sym) ||
	    !ctn_core_range(victim_kallsyms.strtab, typetab - strtab) ||
	    !ctn_core_range(victim_kallsyms.typetab, victim_kallsyms.num_symtab))
		return -ENOEXEC;
	victim_strtab_bytes = typetab - strtab;
	return 0;
}

/* Exact ELF st_size/type, not kallsyms' distance to the next symbol. */
static int ctn_object_symbol(const char *wanted, size_t expected_size,
			     bool rodata, void **out)
{
	Elf_Sym symbol;
	char symbol_name[CT_SYMBOL_NAME_LEN], type;
	unsigned int i;
	void *found = NULL;
	int ret;

	for (i = 1; i < victim_kallsyms.num_symtab; i++) {
		ret = ctn_nofault(&symbol, victim_kallsyms.symtab + i, sizeof(symbol));
		if (ret)
			return ret;
		if (!symbol.st_name || symbol.st_shndx == SHN_UNDEF)
			continue;
		if (symbol.st_name >= victim_strtab_bytes)
			return -ENOEXEC;
		ret = ctn_nofault_string(victim_kallsyms.strtab + symbol.st_name,
					 symbol_name, sizeof(symbol_name),
					 victim_strtab_bytes - symbol.st_name);
		/* Long unrelated symbol names are legitimate, but cannot match wanted. */
		if (ret == -ENAMETOOLONG)
			continue;
		if (ret)
			return ret;
		if (strcmp(symbol_name, wanted))
			continue;
		if (found || symbol.st_size != expected_size ||
		    ELF_ST_TYPE(symbol.st_info) != STT_OBJECT ||
		    symbol.st_shndx >= SHN_LORESERVE)
			return -ENOEXEC;
		found = (void *)kallsyms_symbol_value(&symbol);
		if (!found || ctn_nofault(&type, victim_kallsyms.typetab + i, 1))
			return -EFAULT;
		if (rodata ? (!ctn_rodata_range(found, expected_size) || type != 'r') :
		    (!ctn_data_range(found, expected_size) || (type != 'b' && type != 'd')))
			return -ENOEXEC;
	}
	if (!found)
		return -ENOENT; /* Missing symbol in a LIVE module is permanent. */
	*out = found;
	return 0;
}

static void ctn_clear_pid_source(void)
{
	smp_store_release(&pid_display_available, false);
	g_related_threads = NULL;
	g_total_num = NULL;
	g_have_valid_rp = NULL;
	g_rt_lock = NULL;
}

static void ctn_resolve_pid_source(void)
{
	void *related, *total, *valid, *lock;
	rwlock_t lock_snapshot;

	ctn_clear_pid_source();
	if (!CTN_PID_SOURCE_VERIFIED || !find_task_by_pid_ns_fn ||
	    !initial_pid_namespace)
		return;
	if (ctn_object_symbol("related_threads", MAX_TID_COUNT * CTN_EXPECT_RRT_SIZE,
			      false, &related) ||
	    ctn_object_symbol("total_num", sizeof(int), false, &total) ||
	    ctn_object_symbol("have_valid_render_pid", sizeof(atomic_t), false, &valid) ||
	    ctn_object_symbol("rt_info_rwlock", CTN_EXPECT_RT_LOCK_SIZE, false, &lock) ||
	    (unsigned long)related % __alignof__(struct rrt_entry) ||
	    (unsigned long)total % __alignof__(int) ||
	    (unsigned long)valid % __alignof__(atomic_t) ||
	    (unsigned long)lock % __alignof__(rwlock_t) ||
	    ctn_nofault(&lock_snapshot, lock, sizeof(lock_snapshot))) {
		pr_warn("ctn_patch: PID source unavailable; locked display disabled\n");
		return;
	}
	g_related_threads = related;
	g_total_num = total;
	g_have_valid_rp = valid;
	g_rt_lock = lock;
	smp_store_release(&pid_display_available, true);
}

/* Called with patch_lock. The rwlock protects entries; RCU protects task lookup. */
static pid_t ctn_find_pid(const char *name, pid_t excluded)
{
	struct rrt_entry entry;
	struct task_struct *task;
	unsigned long flags;
	char comm[TASK_COMM_LEN];
	size_t length = strlen(name);
	pid_t found = -1, task_pid;
	int n, valid, i;

	if (!smp_load_acquire(&pid_display_available) || !length ||
	    length >= TASK_COMM_LEN)
		return -1;
	read_lock_irqsave(g_rt_lock, flags);
	rcu_read_lock();
	if (ctn_nofault(&n, g_total_num, sizeof(n)) || n <= 0 || n > MAX_TID_COUNT ||
	    ctn_nofault(&valid, &g_have_valid_rp->counter, sizeof(valid)) || !valid)
		goto out;
	for (i = 0; i < n; i++) {
		if (ctn_nofault(&entry, g_related_threads + i, sizeof(entry)))
			break;
		if (entry.pid <= 0 || entry.pid == excluded || !entry.task)
			continue;
		task = find_task_by_pid_ns_fn(entry.pid, initial_pid_namespace);
		/* Never dereference the potentially unreferenced vendor entry.task. */
		if (!task || task != entry.task ||
		    ctn_nofault(&task_pid, &task->pid, sizeof(task_pid)) ||
		    task_pid != entry.pid ||
		    ctn_nofault_string(task->comm, comm, sizeof(comm), sizeof(task->comm)))
			continue;
		if (!strncmp(name, comm, length)) {
			found = entry.pid;
			break;
		}
	}
out:
	rcu_read_unlock();
	read_unlock_irqrestore(g_rt_lock, flags);
	return found;
}

static void ctn_resolve_pids(const char names[CT_NUM][CT_NAME_LEN], pid_t pids[CT_NUM])
{
	pids[1] = ctn_find_pid(names[1], -1);
	pids[0] = !strcmp(names[0], names[1]) ? pids[1] :
		  ctn_find_pid(names[0], pids[1]);
}

/* Verify current ownership before reading/publishing. Unexpected writers fail closed. */
static int ctn_check_slots(const char *slots[CT_NUM])
{
	int i;

	if (ctn_nofault(slots, critical_task, CT_PTR_ARRAY_BYTES))
		goto stale;
	for (i = 0; i < CT_NUM; i++) {
		const char *expected = patched ? name_buf[i][active_slot[i]] : original_name[i];

		if (slots[i] != expected)
			goto stale;
	}
	return 0;
stale:
	smp_store_release(&patch_ready, false);
	return -ESTALE;
}

static ssize_t ctn_read(struct file *file, char __user *ubuf,
			size_t count, loff_t *ppos)
{
	char page[CT_INPUT_LEN], names[CT_NUM][CT_NAME_LEN];
	const char *slots[CT_NUM];
	pid_t pids[CT_NUM];
	size_t available;
	int ret, i, len;

	mutex_lock(&patch_lock);
	if (!smp_load_acquire(&patch_ready) || READ_ONCE(stopping)) {
		ret = -ENODEV;
		goto out;
	}
	ret = ctn_check_slots(slots);
	if (ret)
		goto out;
	for (i = 0; i < CT_NUM; i++) {
		available = patched ? CT_NAME_LEN :
			(unsigned long)victim_layout.base + victim_layout.ro_size -
			(unsigned long)slots[i];
		ret = ctn_nofault_string(slots[i], names[i], sizeof(names[i]), available);
		if (ret)
			goto out;
	}
	ctn_resolve_pids(names, pids);
	len = scnprintf(page, sizeof(page), "%s:%d,%s:%d\n",
			names[0], (int)pids[0], names[1], (int)pids[1]);
	mutex_unlock(&patch_lock);
	return simple_read_from_buffer(ubuf, count, ppos, page, len);
out:
	mutex_unlock(&patch_lock);
	return ret;
}

static ssize_t ctn_write(struct file *file, const char __user *ubuf,
			 size_t count, loff_t *ppos)
{
	char input[CT_INPUT_LEN], names[CT_NUM][CT_NAME_LEN];
	const char *slots[CT_NUM];
	pid_t pids[CT_NUM];
	u8 next[CT_NUM];
	int ret, i;

	if (*ppos != 0)
		return -EINVAL;
	if (!count || count >= sizeof(input))
		return -E2BIG;
	if (copy_from_user(input, ubuf, count))
		return -EFAULT;
	ret = ctn_parse_names(input, count, names);
	if (ret)
		return ret;

	mutex_lock(&patch_lock);
	if (!smp_load_acquire(&patch_ready) || READ_ONCE(stopping)) {
		ret = -ENODEV;
		goto out;
	}
	ret = ctn_check_slots(slots);
	if (ret)
		goto out;
	/* Previous inactive buffers may still be referenced by sched_switch readers. */
	if (patched)
		synchronize_rcu();
	ret = ctn_check_slots(slots);
	if (ret)
		goto out;
	for (i = 0; i < CT_NUM; i++) {
		next[i] = active_slot[i] ^ 1;
		strscpy(name_buf[i][next[i]], names[i], CT_NAME_LEN);
	}
	for (i = 0; i < CT_NUM; i++) {
		/* Independent per-slot release stores, never an atomic pair claim. */
		smp_store_release(&critical_task_rw[i], name_buf[i][next[i]]);
		active_slot[i] = next[i];
	}
	patched = true;
	ctn_resolve_pids(names, pids);
	pr_info_ratelimited("ctn_patch: names [%s:%d] [%s:%d]\n",
			names[0], (int)pids[0], names[1], (int)pids[1]);
	*ppos = count;
	ret = count;
out:
	mutex_unlock(&patch_lock);
	return ret;
}

static const struct proc_ops ctn_proc_ops = {
	.proc_read = ctn_read,
	.proc_write = ctn_write,
	.proc_lseek = default_llseek,
};

static void ctn_release_state(void)
{
	if (critical_task_vmap)
		vunmap(critical_task_vmap);
	critical_task_vmap = NULL;
	critical_task_rw = NULL;
	critical_task = NULL;
	ctn_clear_pid_source();
	if (victim)
		module_put(victim);
	victim = NULL;
	memset(&victim_layout, 0, sizeof(victim_layout));
	memset(&victim_kallsyms, 0, sizeof(victim_kallsyms));
	victim_strtab_bytes = 0;
}

static int ctn_try_patch(void)
{
	struct proc_dir_entry *parent;
	struct page *page;
	void *critical, *parent_symbol;
	unsigned long offset;
	const char *defaults[CT_NUM] = { CTN_EXPECT_DEFAULT0, CTN_EXPECT_DEFAULT1 };
	char original[CT_NAME_LEN];
	int ret, i;

	mutex_lock(&patch_lock);
	if (READ_ONCE(stopping)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	ret = ctn_get_victim();
	if (ret)
		goto out;
	ret = ctn_snapshot_layout();
	if (ret)
		goto fail;
	ret = ctn_object_symbol("critical_task", CTN_EXPECT_CRITICAL_TASK_BYTES,
				true, &critical);
	if (ret)
		goto fail;
	ret = ctn_object_symbol("critical_heavy_boost_dir", sizeof(parent),
				false, &parent_symbol);
	if (ret)
		goto fail;
	if ((unsigned long)critical % __alignof__(const char *) ||
	    (unsigned long)parent_symbol % __alignof__(struct proc_dir_entry *) ||
	    ctn_nofault(&parent, parent_symbol, sizeof(parent)) ||
	    !ctn_kernel_address(parent)) {
		ret = -ENODEV;
		goto fail;
	}
	critical_task = critical;
	ret = ctn_nofault(original_name, critical_task, CT_PTR_ARRAY_BYTES);
	if (ret)
		goto fail;
	for (i = 0; i < CT_NUM; i++) {
		/* Original strings must belong to the pinned module's non-text RO range. */
		if (!ctn_rodata_range(original_name[i], 1)) {
			ret = -ENOEXEC;
			goto fail;
		}
		ret = ctn_nofault_string(original_name[i], original, sizeof(original),
			(unsigned long)victim_layout.base + victim_layout.ro_size -
			(unsigned long)original_name[i]);
		if (ret)
			goto fail;
		if (strcmp(original, defaults[i])) {
			ret = -ESTALE;
			goto fail;
		}
		strscpy(name_buf[i][0], original, CT_NAME_LEN);
		active_slot[i] = 0;
	}
	offset = (unsigned long)critical_task & ~PAGE_MASK;
	if (!ctn_module_mapping(critical_task) ||
	    offset > PAGE_SIZE - CT_PTR_ARRAY_BYTES) {
		ret = -ENOEXEC;
		goto fail;
	}
	page = vmalloc_to_page(critical_task);
	if (!page) {
		ret = -EFAULT;
		goto fail;
	}
	critical_task_vmap = vmap(&page, 1, VM_MAP, PAGE_KERNEL);
	if (!critical_task_vmap) {
		ret = -ENOMEM;
		goto fail;
	}
	critical_task_rw = (const char **)((char *)critical_task_vmap + offset);
	ctn_resolve_pid_source();
	name_entry = proc_create_data("critical_task_name", 0664, parent,
				      &ctn_proc_ops, NULL);
	if (!name_entry) {
		ret = -EEXIST;
		goto fail;
	}
	/* All state is initialized before the node is advertised to userspace. */
	smp_store_release(&patch_ready, !READ_ONCE(stopping));
	pr_info("ctn_patch: profile=%s build_id=%s ready; PID display %s\n",
		CTN_PROFILE_ID, CTN_BUILD_ID,
		smp_load_acquire(&pid_display_available) ? "available" : "disabled");
	ret = 0;
	goto out;
fail:
	ctn_release_state();
out:
	mutex_unlock(&patch_lock);
	return ret;
}

static void ctn_probe_work(struct work_struct *work)
{
	int ret;

	if (READ_ONCE(stopping))
		return;
	probe_tries++;
	ret = ctn_try_patch();
	if (!ret)
		return;
	if (ret == -EAGAIN && probe_tries < PROBE_MAX_TRIES && !READ_ONCE(stopping)) {
		schedule_delayed_work(&probe_work, msecs_to_jiffies(PROBE_RETRY_MS));
		return;
	}
	pr_err("ctn_patch: not ready, retry stopped ret=%d attempts=%u\n", ret, probe_tries);
}

static int __init ctn_patch_init(void)
{
	int ret;

	ret = ctn_validate_profile();
	if (ret) {
		pr_err("ctn_patch: profile/version/layout evidence rejected: %d\n", ret);
		return ret;
	}
	ret = ctn_resolve_functions();
	if (ret) {
		pr_err("ctn_patch: dynamic symbols/nofault/KCFI rejected: %d\n", ret);
		return ret;
	}
	INIT_DELAYED_WORK(&probe_work, ctn_probe_work);
	probe_tries = 1;
	ret = ctn_try_patch();
	if (ret == -EAGAIN) {
		schedule_delayed_work(&probe_work, msecs_to_jiffies(PROBE_RETRY_MS));
		pr_info("ctn_patch: waiting for LIVE %s; patch_ready=false\n", VICTIM_MODULE);
		return 0;
	}
	return ret;
}

static void __exit ctn_patch_exit(void)
{
	int i;

	WRITE_ONCE(stopping, true);
	smp_store_release(&patch_ready, false);
	cancel_delayed_work_sync(&probe_work);
	/* Never hold patch_lock across proc_remove: it drains blocked callbacks. */
	if (name_entry) {
		proc_remove(name_entry);
		name_entry = NULL;
	}
	mutex_lock(&patch_lock);
	if (patched) {
		for (i = 0; i < CT_NUM; i++) {
			/* The private vmap alias remains mapped and the vendor stays pinned. */
			const char *slot = READ_ONCE(critical_task_rw[i]);

			if (slot == name_buf[i][0] || slot == name_buf[i][1])
				smp_store_release(&critical_task_rw[i], original_name[i]);
			else if (slot != original_name[i])
				pr_err("ctn_patch: slot %d owned externally; not overwritten\n", i);
		}
		/* Both slots no longer point into this module before its buffers disappear. */
		synchronize_rcu();
		patched = false;
	}
	ctn_release_state();
	mutex_unlock(&patch_lock);
	pr_info("ctn_patch: unloaded\n");
}

module_init(ctn_patch_init);
module_exit(ctn_patch_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Turbo");
MODULE_DESCRIPTION("Add /proc/game_opt/task_boost/critical_task_name for verified Oplus 6.1 game_opt");
MODULE_VERSION("3.2");
MODULE_INFO(ctn_profile, CTN_PROFILE_ID);
MODULE_INFO(ctn_build_id, CTN_BUILD_ID);
MODULE_SOFTDEP("pre: " VICTIM_MODULE);
