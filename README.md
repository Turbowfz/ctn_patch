# ctn_patch

给一加 8Gen3 / 6.1 内核补回 `/proc/game_opt/task_boost/critical_task_name`
可写节点，并自动按云控配置为每个游戏填入关键线程名。

作者：Turbo ｜ 许可：GPL-2.0-only

**已在真机验证**：一加 Ace 3 Pro / PJX110 / 16.0.5.701 / 内核
`6.1.141-android14-11-o-gd2a6093d5589` / KernelSU。

---

## 一、为什么需要它

一加 8Gen3 的 6.1 内核里，`oplus_bsp_game_opt.ko` 把关键线程名单**写死**成
`UnityMain` / `UnityGfxDevice`，6.6 才做成可写节点。这带来两个问题，要分两步解决：

1. **节点不存在** → 外部内核模块补出来（`ctn_patch.ko`）
2. **没人写它** → 一加 6.1 的 gameopt HAL 走的是 sched_assist 那条路
   （`GameoptIoctl` 的 ioctl + `pipeline_pids_cpus`），**从没碰过内核的
   `critical_task[]`**（HAL 二进制里 `critical_task_name` 出现 0 次）。
   所以还得有个 daemon 去写（`ctnd`）

只装内核模块的话，节点永远停在默认值 —— 这对 Unity 游戏刚好合适，
但对**虚幻引擎游戏**（`GameThread` / `RenderThread`）完全没用。

## 二、组成

| 部分 | 作用 |
|---|---|
| `ctn_patch.ko` | 外部 LKM，补出可写节点 |
| `ctnd` | 用户态 daemon，按云控配置自动写节点 |
| `ctn_patch.zip` | Magisk / KernelSU 刷入包（含上面两个） |

## 三、安装

Magisk / KernelSU / APatch → 模块 → 从存储安装 → 选 `ctn_patch.zip` → 重启。

刷入时会做 9 组兼容性检查，不满足直接拒绝安装（见第七节）。

重启后确认：

```bash
cat /data/adb/modules/ctn_patch/boot.log     # 模块加载 + daemon 启动
cat /data/adb/modules/ctn_patch/daemon.log   # daemon 干活记录
cat /proc/game_opt/task_boost/critical_task_name
```

## 四、用法

节点要求**恰好两个名字**，空格分隔（制表符、换行也行）：

```bash
echo "名字1 名字2" > /proc/game_opt/task_boost/critical_task_name
cat /proc/game_opt/task_boost/critical_task_name
# 名字1:-1,名字2:-1
```

读回格式是官方 6.6 的 `名字:pid,名字:pid`（6.1 内核没有 pid 数据，固定 `-1`）。

| 输入 | 结果 |
|---|---|
| `A B` / `A    B` / `A<TAB>B` | 接受 |
| `onlyone`（只填一个） | 拒绝 `-EINVAL`，节点不变 |
| `A B C`（三个） | 拒绝 `-EINVAL` |
| 空 | 拒绝 `-E2BIG` |
| 99 字符 | 接受（接口上限，与官方 `%99s` 一致） |

**写入是整体替换**，不保留原值。想留着 Unity 默认名就自己带上：
`echo "UnityMain MyGameMain" > ...`

**只盯一个线程**：同名写两遍（`echo "A A" > ...`）。内核里两个槽记同一线程、
同一 CPU，等价于只填一个，不会双倍加成。

**名字建议 ≤15 字符**：接口放到 99 是为了跟官方对齐，但内核比对用的是
`strncmp(task->comm, name, strlen(name))`，而 `task->comm` 只有 16 字节（含 NUL），
超过 15 字符的名字永远匹配不到（不会越界，只是白填）。

装好 daemon 后通常不用手动写 —— 它会在游戏启动时自动填。

## 五、原理

### 5.1 内核模块

6.1 的名单是 `const char* critical_task[2]`，**数组在只读段**，直接写会触发内核
写保护异常。所以：

- `kprobe` 取 `kallsyms_lookup_name`，按 `模块名:符号名` 查
  `oplus_bsp_game_opt:critical_task` 和 `...:critical_heavy_boost_dir`
- `vmap(&page, 1, VM_MAP, PAGE_KERNEL)` 建**可写别名**，只通过别名改数组内容
- **双缓冲 + `synchronize_rcu()`** 换指针：读者是 `sched_switch` tracepoint 里的
  `update_critical_task_time()`，6.1 的 `__DO_TRACE` 用
  `rcu_read_lock_sched_notrace()` 包住回调，所以 `synchronize_rcu()` 等得到它
- `find_module()` + `try_module_get()` **钉住目标模块**，防止它先被 `rmmod`
  导致手里的符号变野指针

布局守卫：6.6 的 `critical_task` 是内联 `char[2][100]`，用「上半区指针」判断
把它拒掉（`-EINVAL`），避免把新内核改坏。

### 5.2 daemon

```
轮询 /proc/game_opt/game_pid (500ms)
  │  HAL 在游戏启动时写 pid、退出写 -1
  └ pid 由 -1 变正数
      ├ 读 /proc/<pid>/cmdline 取包名
      ├ 拷 db/-wal/-shm 到临时目录，用 SQLite 查该包的 game_config
      │   库: /data/user/0/com.oplus.cosa/databases/db_game_database
      ├ 有 ctn       → 写进去
      ├ 有配置无 ctn → Unity 游戏 → 写回 UnityMain UnityGfxDevice
      ├ 库里没这个包 → OPLUS 不认识 → 写回默认值
      └ 读库失败     → 保持现状不动（宁可不动，也别写错名字）
  └ pid 连续 8 次（4 秒）为 -1 → 恢复默认名字
```

`game_config` 里的 **`ctn`** 就是官方给的关键线程名，格式与节点完全一致：

| 包 | `ctn` | 引擎 |
|---|---|---|
| `com.tencent.tmgp.pubgmhd` | `RenderThread Thread-` | 虚幻 |
| `com.hottagames.yh.laohu` / `dfm` / `mingchao` / `codev` | `GameThread RenderThread` | 虚幻 |
| `com.miHoYo.Nap` 等 | 无 `ctn` | Unity → 用内核默认值 |

Unity 游戏没有 `ctn`，因为内核默认的 `UnityMain` / `UnityGfxDevice` 正好就是
它们的线程名 —— 这也说明 `ctn` 就是为「非 Unity 游戏」准备的。

实现上的几个必要选择：

| 点 | 做法 | 原因 |
|---|---|---|
| 读 SQLite | `dlopen("/system/lib64/libsqlite.so")` | 平台私有库，NDK 没 stub；各机型路径不同，dlopen 好回退 |
| 库是 WAL 模式 | 拷 db/-wal/-shm 再读 | 直接开原库会动它的 -shm，拷贝零干扰 |
| JSON 解析 | 手写取值器 | `game_config` 是扁平 JSON，省一个依赖 |
| `ctn` 字符串 / `ctb` 数字 | 两个取值器 | 混用会取不到 `ctb` |
| 退出防抖 | 连续 8 次 -1 才算退出 | 加载期 `game_pid` 会反复抖好几秒 |
| 单实例 | `flock` 锁文件 | 防止被两条路径重复拉起 |

## 六、构建

### 6.1 内核模块

需要一份能用的内核构建产物。本机（Windows）没有工具链，实际是在 **WSL Arch** 里
用 **NDK r26b 的 clang 17.0.2** 编的 —— 这个版本和设备内核**同一个 llvm-project
提交**（`d9f89f4d…`），所以 kCFI 类型号必然一致（实测我们回调的类型号和设备
模块逐字相等）。

```bash
bash wsl_build.sh        # 一键全流程（详见脚本内注释）
```

流程：装依赖 → 下载 NDK → 克隆 OnePlusOSS 内核源码 → 铺设备真实 `.config` →
`modules_prepare` → 编 `.ko` → 校验。

**两个关键约束**：

- **`.config` 必须忠实沿用设备配置**。为省事关掉 `CONFIG_DEBUG_INFO_BTF(_MODULES)`
  会让 `sizeof(struct module)` 从 1088 掉到 1024，装载器按自己的布局越界写坏指针，
  `insmod` 时在 `mod_sysfs_setup` panic（**实测踩过**）。`build.sh` 第 8 步会硬校验
  `.gnu.linkonce.this_module` 段大小。
- **符号 CRC 取自设备**。内核开了 `CONFIG_MODVERSIONS`，整编一遍要几小时；
  这里从设备自带的 432 个 `.ko` 的 `__versions` 段抽出 4168 个符号 CRC
  （`extract_symvers.py` → `Module.symvers.device`），够用且不必整编。
  唯一缺的 `copy_from_kernel_nofault`（被 `TRIM_UNUSED_KSYMS` 裁掉）改成运行时
  用 kallsyms 取址。

### 6.2 daemon

```bash
bash daemon/build.sh     # NDK aarch64 clang，产出约 18KB 单文件
bash daemon/lint.sh      # 严格警告检查（应零输出）
```

只依赖 `libc.so` / `libdl.so`，无其他运行时依赖。

### 6.3 打包

```bash
python build_zip.py      # 产出 ctn_patch.zip
```

## 七、兼容性

| 维度 | 要求 |
|---|---|
| 内核 | `6.1.141-android14-11-o-*`（git hash 尾差容忍：内核自身 `…gd2a6093d5589`、模块 `…gdc1b6a03413f`，模块照样加载） |
| 机型 | 8Gen3 / sm8650 一加系列（Ace3Pro、Ace5、一加12、Pad2 的 6.1 `game_opt` 源码已实测同源） |
| 目标模块 | `oplus_bsp_game_opt` 必须是**模块**（内建机型不支持） |
| root | Magisk / KernelSU / APatch |
| 架构 | arm64（页大小不限，只要求那 16 字节指针数组不跨页） |

**换机型必须重编**：`CONFIG_MODVERSIONS` 的 CRC 是对结构体/函数原型定义做哈希，
而定义受 `.config` 影响。本包按 Ace3Pro 编译，含构建时对照的厂商模块 sha256。

**刷入时的闸门**（`customize.sh`，9 组检查，不满足直接 `abort`）：
内核版本 / `.ko` 与 vermagic / `oplus_bsp_game_opt` 是否模块 / 厂商模块 sha256 /
节点是否已存在 / `ctnd` 是否为有效 ELF / SQLite 库 / 云控库 / 内核配置项 /
运行时符号。分 `[不满足]`（拒绝安装）、`[警告]`、`[备注]` 三级。

**升级路径**：升到 6.6 内核 → 自带 ctn 且布局变了 → 守卫直接拒，不会重复建节点
也不会崩，此时应卸载本模块。升到更高 6.1.x → vermagic 前缀变 → 拒绝，需重编。

**与其他修改的关系**：别人也做同一件事 → `proc_create_data` 返回 NULL →
`-EEXIST` 安全退出不覆盖；别人想卸载 `game_opt` → 被我们钉住（refcount +1，
有意设计）；不碰 `oplus_bsp_sched_ext`、不碰 HAL、不依赖 Zygisk/LSPosed。

## 八、已知限制

1. **`ct_enable` 是另一道门**。名字设对了，boost 本身还归 HAL 控制 —— 它按
   `game_config.ctb` 加**游戏场景激活**（`/proc/uag/is_game_scene`）判定。实测
   某游戏配置 `ctb=1` 但运行期 `is_game_scene` 始终为 0，`ct_enable` 也就一直
   是 0。这是 OPLUS 自己的场景判定，不在本模块能控的范围。想让 daemon 绕过它：
   `ctnd -e` 会按配置的 `ctb` 直接写 `ct_enable`（代价是绕过了 HAL 的场景门控，
   可能影响功耗/温控，自己权衡）。
2. **名字超过 15 字符匹配不到**（见第四节）。
3. **读取里的 pid 固定是 -1**：6.1 没有 `critical_task_pids` / `update_ctb_pids`
   （全仓 0 命中），而 -1 正好等于 6.6 写入后的初始值，格式不受影响。
4. **本设备 6.1 的 HAL 不读这个节点**（走
   `/proc/sys/oplus_sched_ext/pid_unitymain`），所以补节点是补齐 6.6 标准接口 +
   让内核侧匹配名单可改。
5. **daemon 依赖 COSA 的云控库**。库里没有的游戏（OPLUS 不认识）只能用默认名。
6. 模块不持久化；刷 zip 则由 `service.sh` 开机自动加载。

## 九、许可

**GPL-2.0-only**，见 `LICENSE`。所有源文件头部有
`SPDX-License-Identifier: GPL-2.0-only`。

### 为什么不是 GPL-3.0

内核模块的许可不是自由选择，GPL-3.0 既不可行也不合法：

**技术上会加载失败。** 内核判定「GPL 兼容」用白名单
（`include/linux/license.h`）：

```c
return (strcmp(license, "GPL") == 0
	|| strcmp(license, "GPL v2") == 0
	|| strcmp(license, "GPL and additional rights") == 0
	|| strcmp(license, "Dual BSD/GPL") == 0
	|| strcmp(license, "Dual MIT/GPL") == 0
	|| strcmp(license, "Dual MPL/GPL") == 0);
```

`"GPL v3"` 不在其中。而本模块用到三个 **GPL-only** 符号 —— `register_kprobe`、
`unregister_kprobe`、`synchronize_rcu`（都是 `EXPORT_SYMBOL_GPL`）。
一旦被判为非 GPL，它们无法解析，`insmod` 直接失败。而 RCU 同步没有非 GPL 的
替代品，绕不过去。

**法律上互不兼容。** 内核 `COPYING` 是 `GPL-2.0 WITH Linux-syscall-note`，
正文明确是 *GNU GPL version 2 **only***。GPLv2 与 GPLv3 互相不兼容，而内核模块
是内核的衍生作品，必须 GPLv2 兼容。

> 想要 GPLv3 的专利授权/反 Tivoization 条款的话，在 Linux 内核模块这个形态下
> 拿不到 —— 这是内核的固有约束。

### 使用者须知

可自由使用、修改、再分发（含商用）；再分发**二进制**时必须同时提供完整对应源码
和 `LICENSE`（刷入包里已含）；修改后的版本也须以 GPL-2.0 兼容许可发布并保留版权声明。

### 版权

- `ctn_patch.c`、`ctnd.c` 及配套脚本：Copyright (c) Turbo
- `LICENSE`：GNU GPL v2 全文，取自 Linux 内核 `LICENSES/preferred/GPL-2.0`

本项目是**独立的外部模块**，不修改内核源码、不修改任何分区文件，只通过公开内核
API（kprobe / procfs / vmap / RCU）在运行时补一个 proc 节点。读写格式参照一加
6.6 官方源码（OnePlusOSS 开源仓，GPL-2.0）实现，项目中**不包含**任何一加的专有
代码或二进制。

## 十、云更新

`module.prop` 里有：

```
updateJson=https://gitee.com/turbowfz/ctn_patch/raw/main/update.json
```

管理器会拉这个 JSON，比对 `versionCode`，远端更大就显示「更新」按钮。

**发新版本的顺序**（`zipUrl` 里带 tag，顺序错了下载会 404）：

```bash
python bump.py 1.1        # 改版本：module.prop 与 update.json 一起更新
# 补 CHANGELOG.md
python build_zip.py
git add -A && git commit -m 'v1.1' && git push github main && git push gitee main
# 建 v1.1 的 Release，把 ctn_patch.zip 传为附件
```

`python bump.py --show` 可随时检查两个文件是否一致。

**两个注意点**：① 仓库必须公开（管理器是未登录状态拉 `update.json` 的，
私有仓库返回 403）—— **绝不能**把 token 写进 `update.json` 或 `module.prop`
来绕过，那等于把仓库写权限发给每个装模块的人。② `versionCode` 必须单调递增，
`bump.py` 已强制校验。

## 十一、排查

| 现象 | 原因 / 处理 |
|---|---|
| `boot.log` 里 `insmod 失败: disagrees about version of symbol` | CRC 不匹配，必须用同内核的 `Module.symvers` 重编 |
| `boot.log` 里 `Invalid module format` | vermagic 差太多（比如 6.6 内核刷了 6.1 的 ko） |
| `insmod` 后 `mod_sysfs_setup` panic | `struct module` 布局不符 —— 检查是不是关了 BTF，`build.sh` 第 8 步会拦 |
| 刷入时被 `[不满足]` 拒绝 | 闸门拦住了，消息里写明是哪一条 |
| `daemon.log` 里没有「游戏启动」 | `game_pid` 没变 → 这游戏不在 OPLUS 名单里，HAL 不认 |
| `sqlite: 所有候选路径都加载失败` | 该机型库名/路径不同，改 `ctnd.c` 的 `SQLITE_CANDIDATES` |
| `库: 包 xxx 不在 PackageConfigBean 里` | OPLUS 不认识这个游戏 → 用默认名 |
| 节点是 `UnityMain` 但游戏是虚幻的 | 读库失败（看日志），或该包配置里确实没有 `ctn` |
| 名字对了但没效果 | `ct_enable=0` → 见第八节第 1 条 |
| `daemon.log` 一直刷「已有另一个 ctnd 在跑」 | 有重复的守护循环，正常会自己退出；若持续刷说明守护脚本是旧版 |

## 十二、开发中踩过的坑（改代码前请读）

这几条都是实测出来的，改相关代码时容易重新踩：

1. **`struct module` 大小必须和设备一致**。关掉 `CONFIG_DEBUG_INFO_BTF(_MODULES)`
   会让它少 64 字节 → `insmod` 时 panic。而且改完 `.config` **必须重跑
   `make modules_prepare`**，否则 `autoconf.h` 还是旧的，会误判「BTF 不影响大小」。
2. **`| head -N` 会提前关管道**，给 WSL 里的构建进程发 SIGTERM（Error 143），
   看起来像编译失败。看构建输出用 `tail` 或重定向到文件。
3. **`mv "$VAR"/*` 在变量为空时展开成 `/*`**，会把整个根文件系统搬走。
   写脚本时永远先判空。
4. **一加会把上一轮 dmesg 存到** `/data/persist_log/backup/SYSTEM_LAST_KMSG.txt`
   —— pstore 是空的也不代表没崩过。
5. **设备 `/sys/kernel/btf/vmlinux` 含运行内核全部结构体布局**，
   `btf_struct_dump.py` 可以直接 dump 任意 struct 的成员偏移，比猜配置快得多。
6. **`game_pid` 在游戏加载期会在正数和 -1 之间反复抖好几秒**，退出判定必须防抖。
7. **`game_config` 里 `ctn` 是字符串、`ctb` 是数字**，JSON 取值器要分开。
