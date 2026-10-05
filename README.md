# ctn_patch —— 把官方 6.6 的 `critical_task_name` 可写节点 backport 到 6.1

## 一、解决什么问题

一加 Ace 3 Pro（8Gen3，sm8650）的 **6.1 内核**里，游戏调度模块
`oplus_bsp_game_opt.ko` 管着两个「关键线程」。**名单机制本身是有的**：

```c
static const char* critical_task[2] = { "UnityMain", "UnityGfxDevice" };
```

内核每次任务切换都拿这两个名字和线程的 `comm` 比对（`strncmp(task->comm,
critical_task[i], ...)`），命中就计入关键线程运行时间、触发 critical-task
boost（关键任务提频）。

问题只有一个：**这两个名字在 6.1 里被写死**。而 6.6 内核把它做成了可写节点
`/proc/game_opt/task_boost/critical_task_name`，想填哪两个就填哪两个。
这个模块就是把 6.6 的这个可写节点原样搬回 6.1 —— **写入格式和读取格式都和
官方 6.6 源码逐字对齐**，给 6.6 写的工具/脚本原样可用。

## 二、官方 6.6 接口（对齐目标）

6.6 官方源码 `风驰6.6源码/modules_vendor_oplus_kernel_cpu/game_opt/critical_task_boost.c`：

| 项 | 6.6 官方实现 | 行号 |
|---|---|---|
| 存储 | `static char critical_task[2][100] = {"UnityMain", "UnityGfxDevice"};` | :78 |
| pid | `static pid_t critical_task_pids[2] = {-1, -1};` | :80 |
| 写入 | `sscanf(page, "%99s %99s", critical_task[0], critical_task[1])`，必须**恰好两个**空格分隔的名字，否则 `-EINVAL`；写入后把两个 pid 重置为 -1 | :602–627 |
| 读取 | `sprintf(page, "%s:%d,%s:%d\n", 名字, pid, 名字, pid)` | :631–644 |
| 节点 | `proc_create_data("critical_task_name", 0664, task_boost, ...)` | :729 |

所以默认读出来是 `UnityMain:-1,UnityGfxDevice:-1`，写入后是
`你填的名字1:-1,你填的名字2:-1`（6.1 内核没有 pid 数据，见第七节）。

## 三、为什么 6.1 不能像 6.6 那样直接写

同一个名单，两个内核的**存放方式不一样**：

```
6.6：char critical_task[2][100]   ← 名字本体在数组里，数组在可写数据段
                                     官方 sscanf 直接写进去就行

6.1：const char* critical_task[2] ← 数组里是两个「指针」，指向模块 .rodata
                                     里的字符串；数组本身也在 .rodata（只读）
```

6.1 的数组和字符串都在**只读段**，直接写会触发内核写保护异常。所以本模块用
`vmap(&page, 1, VM_MAP, PAGE_KERNEL)` 给同一块物理页建一个**可写别名**，
只通过别名把数组里的两个指针换成指向自己缓冲区的名字。效果一样，路径不同。

## 四、原理

```
                    ┌─ 模块 .rodata（只读）──────────────┐
                    │  const char *critical_task[2]      │
                    │     [0] ──► "UnityMain"            │
                    │     [1] ──► "UnityGfxDevice"       │
                    └────────────┬───────────────────────┘
                                 │ 同一块物理页
              vmap(&page,1,VM_MAP,PAGE_KERNEL) 建可写别名
                                 │
                    ┌────────────▼───────────────────────┐
                    │  critical_task_rw[2]  （可写）      │
                    └────────────┬───────────────────────┘
                                 │ 写入时换指针
        ┌────────────────────────▼───────────────────────────┐
        │ name_buf[i][0]  /  name_buf[i][1]   双缓冲          │
        └────────────────────────────────────────────────────┘
                                 │
   内核读者（sched_switch tracepoint → update_critical_task_time）
   `critical_task[i]` → `strlen` → `strncmp(task->comm, ...)`
```

三个安全机制：

1. **可写别名**：`critical_task` 在只读段，`vmap` 出的新映射指向同一物理页但
   权限是 `PAGE_KERNEL`（可写），只通过别名改数组内容。
2. **双缓冲 + RCU**：读者跑在 `sched_switch` tracepoint 里。6.1 的
   `__DO_TRACE` 用 `rcu_read_lock_sched_notrace()` 把回调包住，所以
   `synchronize_rcu()` 一定等得到这些读者退出。第 N 次写入前先
   `synchronize_rcu()`，确认第 N-2 次发布出去的那块缓冲没人用了，再覆写它。
   （依据：`03_源码重建/src_original_modules/.../critical_task_boost.c:349` 与
   `:401 register_trace_sched_switch`）
3. **模块引用**：加载时 `find_module()` + `try_module_get()` 抓住
   `oplus_bsp_game_opt`。否则对方先被 `rmmod` 的话，我们手里的符号地址、
   `task_boost` 目录指针全变野指针，读写或卸载都会炸。

## 五、编译（已备好一键脚本，只需一个 Linux 环境）

### 5.1 难点在哪，怎么绕过去

这个内核开了 `CONFIG_MODVERSIONS`，模块里每个外部符号都要带 CRC，而 CRC 只有
「编过这个内核」才拿得到。整编一遍内核要几小时 + 几十 GB。这里用两个**从设备上
抠出来的**东西替代掉整编：

| 替代品 | 从哪来 | 内容 |
|---|---|---|
| `device_kernel.config` | 内核 Image 里内嵌的 `.config`（`CONFIG_IKCONFIG=y`） | 设备**真实**的 7585 项配置，含 `CC_VERSION_TEXT`（clang 17.0.2 / r487747c）、`CFI_CLANG=y`、`MODVERSIONS=y`、`STRICT_MODULE_RWX=y`、`ARM64_4K_PAGES=y` 等 |
| `Module.symvers.device` | 设备自带 432 个 `.ko` 的 `__versions` 段 | **4168 个符号的 CRC**，同名符号在所有模块里完全一致（已校验，0 冲突） |

我们用到的符号里只有 `copy_from_kernel_nofault` 不在表里 —— 因为
`CONFIG_TRIM_UNUSED_KSYMS=y` 把它裁掉了（没有任何设备模块引用它）。
所以源码里改成**运行时用 kallsyms 取它的地址**（`CONFIG_KALLSYMS_ALL=y`，
符号一定查得到），取不到才退回 `memcpy`。其余符号（`vmap`/`vunmap`/`proc_create_data`/
`register_kprobe`/`try_module_get`/`synchronize_rcu`/`__arch_copy_from_user` 等）
全部在表里，已逐个核对。

### 5.2 一键构建

```bash
# 依赖（Ubuntu / Debian / WSL2-Ubuntu）
sudo apt-get install -y clang-17 lld-17 llvm-17 build-essential bc bison \
     flex libssl-dev libelf-dev python3 git

# 一条命令：克隆内核 → 铺配置 → modules_prepare → 塞符号表 → 编 .ko → 校验 vermagic
bash build.sh
```

脚本做的事（每一步都有日志）：

1. 找 clang（也支持 `--clang /path/to/clang`）
2. `git clone --depth 1 -b oneplus/sm8650_b_16.0.0_ace_3_pro` 克隆内核源码（约 1.5~2GB）
3. 把 `device_kernel.config` 铺成 `.config`，并**把 `LOCALVERSION` 钉成
   `-android14-11-o-gdc1b6a03413f`、关掉 `LOCALVERSION_AUTO`**，让产物 vermagic 精确命中
4. `make olddefconfig` + `make modules_prepare`（几分钟，**不整编内核**）
5. 把 `Module.symvers.device` 拷成构建目录的 `Module.symvers`
6. `make M=. modules` 编出 `ctn_patch.ko`
7. 读产物里的 `vermagic=` 和目标串比对

`--srcdir` 复用已克隆的源码、`--jobs N` 控并发、`--work` 换工作目录。

### 5.3 vermagic 为什么这么定

设备上的两个串（都是从二进制里直接读出来的）：

```
内核自身       6.1.141-android14-11-o-gd2a6093d5589 SMP preempt mod_unload modversions aarch64
432 个厂商模块 6.1.141-android14-11-o-gdc1b6a03413f SMP preempt mod_unload modversions aarch64
```

两者**只差 git hash**，而厂商模块确实加载成功（`/proc/modules` 里都是 Live）
—— 说明这个内核容忍 hash 尾差。为求确定性，脚本还是把 hash 钉成模块那一串，
让产物 vermagic 与 432 个设备模块完全一致。

### 5.4 没有 Linux 环境怎么办

**用 GitHub Actions**（`.github/workflows/build.yml` 已经写好）：

1. 把本目录推到你自己的 GitHub 仓库
2. 仓库 → Actions → 选 `build-ctn-patch` → Run workflow
3. 跑完（约 15~30 分钟）在该次运行的 **Artifacts** 里下载 `ctn_patch-ko`，
   里面就是 `ctn_patch.ko` 和打好的 `ctn_patch.zip`

工作流用的是 ubuntu-24.04 + apt 的 clang-17。如果因为编译器版本差异出现
kCFI 类型不匹配（表现为加载后间接调用报 CFI failure），换成设备同款
Android prebuilt clang `r487747c` 重编即可。

### 5.5 关于 OnePlusOSS 源码和 Android NDK

**OnePlusOSS 开源项目是必需的，而且用的就是它。** `build.sh` 克隆的是：

```
https://github.com/OnePlusOSS/android_kernel_oneplus_sm8650
分支 oneplus/sm8650_b_16.0.0_ace_3_pro
```

已核对：这个分支顶 Makefile 是 `VERSION=6 PATCHLEVEL=1 SUBLEVEL=141`，
**和设备内核 6.1.141 完全对上**（脚本里加了这道版本校验，不一致会直接退出）。
另外工作区里 `03_源码重建/src_original_kernel` 和 `src_original_modules` 就是这两个
OnePlusOSS 仓的稀疏检出（只取了 kernel/sched 等需要的目录），做源码对照用的；
它们是 `--filter=blob:none` 的部分克隆，要拿来编还得把整棵树拉下来（同样约 2GB），
所以不如让脚本干净地重新 `--depth 1` 克隆一遍。

**Android NDK 用不上（也别用）。** 三个原因：

1. **NDK 是给 Android 用户态（bionic）编 APK/so 的，不是给内核编模块的。**
   内核模块必须由内核自己的 Kbuild 系统驱动（内核的编译参数、
   `-fsanitize=kcfi`、内核头文件都在 Kbuild 里），NDK 的 `ndk-build`/CMake
   工具链管不了这些。
2. **你机器上那份 NDK 是 Windows 版**（`toolchains/llvm/prebuilt/windows-x86_64`，
   全是 .exe），WSL/Linux 里跑不了。要用 NDK 当编译器，得下 **Linux 版**
   NDK（`android-ndk-r26b-linux.zip`）。
3. **版本还偏新**：你那份是 NDK r29（clang 19），设备内核是 clang **17.0.2**。
   差两个大版本，可能撞上新版警告被内核的 `-Werror` 拦下。

所以：**WSL 里 `apt install clang-17 lld-17 llvm-17` 就行**（拿到的是 clang 17.0.6，
和设备 17.0.2 几乎同版本，最省事）。真要跟设备完全同版本，可以下 Linux 版
NDK r26b（对应 clang 17.0.2）；`build.sh` 也会自动去找
`/mnt/c/Users/*/ndk/android-ndk-*/toolchains/llvm/prebuilt/linux-x86_64/bin/clang`。

## 六、加载 / 验证 / 卸载

### 6.0 写入格式细则（实测，`test_parse.c`）

接口和官方 6.6 一样，**必须恰好两个名字**：

```bash
echo "名字1 名字2" > /proc/game_opt/task_boost/critical_task_name
```

| 输入 | 结果 |
|---|---|
| `UnityMain UnityGfxDevice` | 接受 |
| `A B` / `A    B`（多空格）/ `A<TAB>B` / `A<换行>B` | 接受（空格、制表符、换行都算分隔） |
| `ABCDEFGHIJKLMNO ABCDEFGHIJKLMNO`（15 字符） | 接受 |
| `ABCDEFGHIJKLMNOP ...`（16 字符） | 接口接受，但**匹配不到任何线程**（见下） |
| 99 字符 | 接受（接口上限，与官方 `%99s` 一致） |
| 100 字符 | 拒绝 `-ENAMETOOLONG` |
| `onlyone`（只填一个） | **拒绝 `-EINVAL`**，节点内容不变 |
| `A `（一个名字加尾空格） | 拒绝 `-EINVAL` |
| 空输入 | 拒绝 `-E2BIG` |
| `A B C`（三个） | 拒绝 `-EINVAL` |

**写入是整体替换，不保留原值。** 写 `A B` 之后 `UnityMain` / `UnityGfxDevice` 立刻
不再被匹配。想留着就自己带上：`echo "UnityMain MyGameMain" > ...`。
（内核模块里 `critical_task[]` 是 `const`，除了本模块没有任何代码写它；HAL 二进制里
`critical_task_name` 出现 0 次。所以写入的值一直有效到重启或 rmmod。）

**只想盯一个线程怎么办**：接口要求两名，把同名写两遍即可 ——
`echo "MyGameMain MyGameMain" > ...`。
两个槽位各存一套统计，但触发时 `decide_boost_status()` 只是把
`cpu_core[i]` 和 `cpu_core[other_idx]` 放进同一个 cpumask；同名两遍 →
同一线程、同一 CPU，`cpumask_set_cpu` 设两次同一个 CPU，
**结果和"只填一个"完全等价，不会双倍加成**。

**名字长度建议 ≤15 字符**：接口放到 99 是为了和官方一致，但内核比对用的是
`strncmp(task->comm, name, strlen(name))`，`task->comm` 只有 16 字节（含 NUL），
超过 15 字符的名字永远匹配不上（`strncmp` 走到 comm 的 NUL 就停，不会越界）。

**和云控配置不冲突**：`game_zone.pipeline` 里也有 Unity 名字
（如 `"7": "UnityMain"`、`"6": "UnityGfxDeviceW"`），但那是 HAL 做 CPU 亲和性
（`TaskManager::setAffinity`）用的，走的是另一条路；而且它用的是
`UnityGfxDeviceW`（带 W），和内核名单的 `UnityGfxDevice` 不是同一个字符串。

### 6.1 加载与验证

一键脚本：`verify.sh`（见同目录）。

```bash
adb push ctn_patch.ko /data/local/tmp/
adb shell su -c 'insmod /data/local/tmp/ctn_patch.ko'
```

**验证 1：节点出现**

```bash
adb shell su -c 'ls /proc/game_opt/task_boost/'
# 期望能看到 critical_task_name（原来只有 5 个节点）
```

**验证 2：默认值（应等于模块里写死的两个名字，格式与 6.6 一致）**

```bash
adb shell su -c 'cat /proc/game_opt/task_boost/critical_task_name'
# 期望输出：UnityMain:-1,UnityGfxDevice:-1
```

**验证 3：任意填两个名字、能读回**

```bash
adb shell su -c 'echo "MyGameMain MyGameGfx" > /proc/game_opt/task_boost/critical_task_name'
adb shell su -c 'cat /proc/game_opt/task_boost/critical_task_name'
# 期望输出：MyGameMain:-1,MyGameGfx:-1
```

**验证 4：反复写（压一压双缓冲/RCU 路径）**

```bash
adb shell su -c 'for i in 1 2 3 4 5; do
    echo "AAA$i BBB$i" > /proc/game_opt/task_boost/critical_task_name;
    cat /proc/game_opt/task_boost/critical_task_name;
  done'
# 期望每次都能正确读回，dmesg 无 warning/oops
```

**验证 5：内核日志无异常**

```bash
adb shell su -c 'dmesg | grep -i ctn_patch'
adb shell su -c 'dmesg | grep -iE "BUG|WARNING|Unable to handle|Call trace"'
# 期望后者为空
```

**验证 6：模块被钉住（防误卸载）**

```bash
adb shell su -c 'cat /proc/modules | grep oplus_bsp_game_opt'
# 期望 refcount 从 1 变成 2（被我们持有一份）
```

**卸载（必须先从手机上卸载本模块，再考虑动 game_opt）**

```bash
adb shell su -c 'rmmod ctn_patch'
adb shell su -c 'ls /proc/game_opt/task_boost/'
# 期望 critical_task_name 消失，其余 5 个节点还在
```

## 七、打包成 root 模块（Magisk / KernelSU 刷入包）

工程里已经带好了整个 root 模块包，`magisk/` 目录就是模块本体：

| 文件 | 干什么 |
|---|---|
| `module.prop` | 模块身份卡（管理器里显示的名字/说明） |
| `customize.sh` | 刷入时跑：查内核系列（`6.1.141-android14-11`，不符直接拒绝）→ 查 zip 里有没有 `.ko`、抽 vermagic 对比（前 4 段一致放行，GKI 容忍 git hash 尾差）→ 设权限 |
| `service.sh` | **开机自动 `insmod`**：等 `oplus_bsp_game_opt` 出现（最多 90 秒）再加载，不卡开机（后台跑），日志写 `boot.log` |
| `uninstall.sh` | 管理器里停用/删除模块时 `rmmod ctn_patch` |
| `action.sh` | 管理器「操作/Action」按钮：重载模块 + 跑完整验证，结果写 `action.log` |
| `verify.sh` | 同一份 11 步验证脚本，action.sh 会自动调用 |

**产出 zip（一条命令）**：

```bash
# 先按第五节把 ctn_patch.ko 编出来，放到本目录，然后：
python build_zip.py
# 产出 ctn_patch.zip（module.prop 在 zip 根目录，Magisk 要求的平铺格式；
# 所有 sh 文本自动归一成 LF，Windows 编辑不会带 CRLF 坑）
```

**刷入**：Magisk App（或 KernelSU App）→ 模块 → 从存储安装 → 选 zip → 重启。

**装完看**：开机日志 `/data/adb/modules/ctn_patch/boot.log`；用法见上一节。
常见报错对照表在 `magisk/README.md`（`disagrees about version of symbol` = CRC
不匹配要重编、SELinux 拦 insmod、写入不生效要先开 `ct_enable` 等）。

## 八、兼容性

> 作者：Turbo ｜ 模块版本 v1.0

### 8.1 适配范围（精确）

| 维度 | 范围 | 说明 |
|---|---|---|
| 内核版本 | `6.1.141-android14-11-o-*` | git hash 尾差容忍（内核自身 `…gd2a6093d5589`、模块 `…gdc1b6a03413f` 都能加载） |
| 机型 | 8Gen3 / sm8650 一加系列 | Ace3Pro、Ace5、一加12、Pad2 的 6.1 分支 `game_opt` 源码已实测同源（都 581 行、都无 ctn） |
| 目标模块 | `oplus_bsp_game_opt` **必须是模块** | 内建进内核的机型不支持（`find_module` 失败 → 拒绝加载） |
| 架构 | arm64（页大小不限） | 只要求那 16 字节指针数组不跨页；跨了会被检查拒掉。16K/64K 页同样可用 |
| root | Magisk / KernelSU / APatch | 都支持 Magisk 模块格式；需要 `sys_module` 能力 |

**换机型必须重编 `.ko`。** 原因：`CONFIG_MODVERSIONS` 的 CRC 是对结构体/函数原型
定义做哈希，而定义受 `.config` 影响；不同机型的 config 不同 → CRC 可能不同 →
直接拿来用会报 `disagrees about version of symbol`。重编流程：从目标机型的内核
Image 抽 `.config`、从目标机型的 ko 抽符号 CRC，然后跑 `wsl_build.sh`。
本工程里的 `device_kernel.config` 和 `Module.symvers.device` **是 Ace3Pro 的**。

### 8.2 必须满足的内核条件（不满足都是**安全失败**，不会崩）

| 条件 | 不满足时的行为 |
|---|---|
| `oplus_bsp_game_opt` 是模块 | `find_module()` 失败 → `-ENOENT`，拒绝加载 |
| `critical_task` 是**指针数组**布局（6.1 形态） | 上半区指针校验失败 → `-EINVAL`，拒绝加载 |
| `CONFIG_KPROBES=y` | kprobe 取不到 `kallsyms_lookup_name` → 报错退出 |
| `kallsyms_lookup_name` 未标 `NOKPROBE_SYMBOL` | 同上 |
| 模块内存走 vmalloc | `is_vmalloc_addr` 失败 → `-EFAULT` |
| 指针数组不跨页 | 跨页 → `-EINVAL` |
| `CONFIG_KALLSYMS_ALL=y` | 查不到 `copy_from_kernel_nofault` → **只告警**，退回 `memcpy` 读串 |
| `CONFIG_TRIM_UNUSED_KSYMS` | 无影响（符号表是设备抽的，已核对） |
| 未开 `MODULE_SIG_FORCE` | 若某 ROM 强制签名 → 未签名模块装不上（本机没开） |

### 8.3 升级路径

- **OTA 升到 6.6 内核**（一加13 那种）：新内核**自带** ctn，且 `critical_task` 变成
  `char[2][100]` → 我们的布局守卫直接拒（`-EINVAL`）→ **不会重复建节点、不会崩**。
  这种情况应该卸载本模块（6.6 原生就有）。
- **OTA 升到更高 6.1.x**（比如 6.1.145）：vermagic 前缀变了 → `insmod` 报版本不匹配 →
  拒绝。`customize.sh` 在刷入时也会先拦一道。需要按新内核重编。

### 8.4 与其他修改的相互作用

| 场景 | 行为 |
|---|---|
| 别的模块也做同一件事 | 我们的 `proc_create_data` 返回 NULL → `-EEXIST`，**安全退出不覆盖** |
| 别的模块想卸载 `game_opt` | 我们用 `try_module_get()` 钉住了它 → 卸载会失败（**有意设计**，防止我们手里的符号变野指针）。`/proc/modules` 里它的 refcount 会 +1 |
| 别的模块**替换了** `oplus_bsp_game_opt.ko` | 符号地址变了但名字没变，kallsyms 照样查得到 → 兼容 |
| 改 `oplus_bsp_sched_ext` / HAL 进程 | 互不影响（本模块只碰一个 16 字节指针数组） |
| 云控 `game_zone.pipeline` 的 Unity 名字 | 另一条路（HAL 亲和性，且用 `UnityGfxDeviceW` 带 W），不冲突 |

### 8.5 卸载可逆性

`rmmod ctn_patch` → 恢复原始指针 + 释放对 `game_opt` 的引用。**不修改任何分区文件**，
不依赖 Zygisk / LSPosed，重启即彻底还原。

### 8.6 明确不支持

- `oplus_bsp_game_opt` 内建进内核的机型
- 开了 `MODULE_SIG_FORCE` 的 ROM（未签名模块不让加载）
- 内核版本不是 `6.1.141-android14-11` 的（要重编）
- 内核没开 `CONFIG_KPROBES` 的（取不到 `kallsyms_lookup_name`）

上面这些**以及 8.2 的全部条件**，刷入时由 `customize.sh` 逐条检查，
不满足直接 `abort` 拒绝安装（详见 8.7）。

### 8.7 刷入时的兼容性闸门（customize.sh）

`customize.sh` 把 8.2 / 8.6 的条件全部前移到**安装时**检查，
避免"刷完重启才发现装不上"。判定分三级：

| 级别 | 含义 | 处理 |
|---|---|---|
| `[不满足]` | 必定装不上或会冲突 | 计入 FAIL，最终 `abort` 取消安装 |
| `[警告]` | 能装但功能可能打折 | 继续安装，刷完提示 |
| `[备注]` | 仅记录，供排障 | 继续安装 |

检查项（共 7 组）：

1. **内核版本**：`uname -r` 必须是 `6.1.141-android14-11*`
2. **模块文件**：`.ko` 存在、能抽出 `vermagic`、版本前缀与内核一致、尾标是
   ` SMP preempt mod_unload modversions aarch64`
3. **目标模块**：`oplus_bsp_game_opt` 在 `/proc/modules` 里（确认是模块而非内建）、
   `/proc/game_opt/task_boost` 目录存在
4. **重复安装**：`critical_task_name` 已存在 → 拒绝（6.6 内核自带，或已装过同类补丁）
5. **内核配置**：从 `/proc/config.gz` 读（设备 `CONFIG_IKCONFIG_PROC=y`，实测可读）——
   `KPROBES` / `MODULE_UNLOAD` / `MODULE_SIG_FORCE`（必须未开）/ `KALLSYMS_ALL` /
   `MODVERSIONS` / `CFI_CLANG` / `STRICT_MODULE_RWX`
6. **运行时符号**：`/proc/kallsyms` 里有 `kallsyms_lookup_name`、`find_module`、
   `register_kprobe`；`copy_from_kernel_nofault` 缺席只警告（会退回 memcpy）
7. **其它**：页大小、SELinux 状态、`kptr_restrict`（仅记录）

读不到 `/proc/config.gz` 时第 5 组降级为警告跳过，其余照查。
另外 `customize.sh` 用的是 `$MODPATH`（Magisk 在安装阶段给的就是它，
`MODDIR` 是运行阶段才有的变量）—— 这点比早先版本修正过。

### 8.8 真机验证状态

**已在真机通过**（2026-10-05，一加 Ace 3 Pro / PJX110 / 16.0.5.701 / 内核
`6.1.141-android14-11-o-gd2a6093d5589` / KernelSU）。当时实测结果：

| 项 | 结果 |
|---|---|
| `insmod` | rc=0 |
| `oplus_bsp_game_opt` refcount | 1 → 2（被钉住） |
| 节点 | `/proc/game_opt/task_boost/critical_task_name`，`-rw-rw-r--` |
| 默认值 | `UnityMain:-1,UnityGfxDevice:-1` |
| 写入/读回 | `MyGameMain MyGameGfx` → `MyGameMain:-1,MyGameGfx:-1` |
| 边界 | 单名/三名被拒；多空格、制表符接受 |
| 连续 8 次写读（压双缓冲/RCU） | 全部正确 |
| dmesg | 无 BUG / WARNING / oops / CFI failure |
| `rmmod` | rc=0，refcount 回 1，节点消失，原节点仍可读 |

> 开发中踩过一个会**硬崩**的坑：为省事关掉 `CONFIG_DEBUG_INFO_BTF(_MODULES)`，
> 导致 `sizeof(struct module)` 从 1088 掉到 1024，装载器按自己的布局越界写坏指针，
> `insmod` 时在 `mod_sysfs_setup` panic。**现在 `build.sh` 第 8 步会硬校验
> `.gnu.linkonce.this_module` 段大小**，低于设备值直接报错退出；
> `customize.sh` 刷入时也会比对设备厂商模块的 sha256（见 8.7）。

## 九、已知限制

1. **必须和手机内核同版本编译**（`CONFIG_MODVERSIONS=y`）。换内核版本要重编，
   并且要重新抽一次 `device_kernel.config` 和 `Module.symvers.device`。
2. 如果某个内核把 `kallsyms_lookup_name` 标了 `NOKPROBE_SYMBOL`，
   kprobe 取地址会失败，模块加载会报
   `ctn_patch: kallsyms_lookup_name 不可用: -EINVAL`。本机内核没标（6.1.141
   ACK 分支），但换机要留意。
3. **超过 15 字符的名字永远匹配不到线程**。接口层面允许到 99 字符（和官方
   6.6 的 `%99s` 一致），但内核比对用的是 `task->comm`，只有 16 字节（含 NUL），
   所以实际要让 boost 生效，名字请控制在 15 字符以内。
4. **读取里的 pid 固定是 -1**。6.6 读取能显示两个关键线程的真实 pid
   （`update_ctb_pids` 维护），6.1 内核没有这套数据
   （`critical_task_pids` / `update_ctb_pids` 全仓 0 命中），所以固定输出 -1
   —— 正好等于 6.6 写入后的初始值，格式不受影响。
5. 本设备 6.1 的 HAL **不读这个节点**，它走
   `/proc/sys/oplus_sched_ext/pid_unitymain`（从 HAL 二进制里抽出的 proc 路径
   清单可以看到）。所以这个节点是「把 6.6 的标准接口补齐、让上层工具/新 HAL
   能按统一接口读写，并直接改内核侧 boost 的匹配名单」；改完要真正生效还需
   `ct_enable` 打开，且名字和游戏线程的 `comm` 完全一致。
6. **已在真机验证通过**（2026-10-05，一加 Ace 3 Pro / 16.0.5.701 / KernelSU）：
   `insmod` 成功、节点创建、读写与边界用例正确、连续 8 次写读稳定、
   `rmmod` 完整还原，dmesg 无 BUG/WARNING/oops/CFI failure。
7. 模块不做持久化，重启后需要重新 `insmod`；刷 `ctn_patch.zip` 则由
   模块的 `service.sh` 开机自动加载。

## 十、许可（License）

本项目使用 **GPL-2.0-only**，见 `LICENSE`（取自 Linux 内核自带的
`LICENSES/preferred/GPL-2.0` 全文）。所有源文件头部都有
`SPDX-License-Identifier: GPL-2.0-only`。

### 10.1 为什么不是 GPL-3.0

内核模块的许可**不是自由选择**，GPL-3.0 对这个项目既不可行也不合法：

**技术上会直接加载失败。** 内核判定「GPL 兼容」用的是白名单
（`include/linux/license.h` 的 `license_is_gpl_compatible()`）：

```c
return (strcmp(license, "GPL") == 0
	|| strcmp(license, "GPL v2") == 0
	|| strcmp(license, "GPL and additional rights") == 0
	|| strcmp(license, "Dual BSD/GPL") == 0
	|| strcmp(license, "Dual MIT/GPL") == 0
	|| strcmp(license, "Dual MPL/GPL") == 0);
```

`"GPL v3"` 不在其中。而本模块用到三个 **GPL-only** 符号：

| 符号 | 用途 | 导出方式 |
|---|---|---|
| `register_kprobe` | 取 `kallsyms_lookup_name` 地址 | `EXPORT_SYMBOL_GPL` |
| `unregister_kprobe` | 用完注销 | `EXPORT_SYMBOL_GPL` |
| `synchronize_rcu` | 双缓冲换指针的安全前提 | `EXPORT_SYMBOL_GPL` |

一旦 `MODULE_LICENSE` 被内核判为非 GPL，这三个符号**无法解析** →
`insmod` 报 unknown symbol 直接失败。也就是说，声称 GPL-3.0 的版本
**根本加载不起来**，除非把 kprobe 和 RCU 同步全部重写掉——而 RCU 同步
没有非 GPL 的替代品。

**法律上互不兼容。** Linux 内核的 `COPYING` 明确是
`GPL-2.0 WITH Linux-syscall-note`，正文是 *GNU General Public License
version 2 only*。GPLv2 与 GPLv3 是**互相不兼容**的两个许可（自由软件基金会
自己的立场），而内核模块是内核的衍生作品，因此模块必须是 GPLv2 兼容的。
把模块声明成 GPL-3.0，等于同时违反内核的许可和 GPLv2 自身的条款。

> 想要 GPLv3 的专利授权/反 Tivoization 条款的话，那些条款在 Linux 内核模块
> 这个形态下拿不到——这是 Linux 内核的固有约束，不是本项目的选择。

### 10.2 对你（使用者/再分发者）意味着什么

- 可以自由使用、修改、再分发，包括商用；
- 再分发**二进制**（比如打包进 ROM）时，必须同时提供**完整对应源码**和
  `LICENSE`；刷入包里已包含 `LICENSE`；
- 修改后的版本也必须以 GPL-2.0 兼容许可发布，并保留版权声明。

### 10.3 版权

- `ctn_patch.c` 及配套构建/验证脚本：Copyright (c) Turbo
- `LICENSE`：GNU General Public License v2 全文，来自 Linux 内核源码树
  `LICENSES/preferred/GPL-2.0`

### 10.4 与内核、与一加官方的关系

本项目是**独立的外部模块**，不修改内核源码、不修改任何分区文件：
它只通过公开的内核 API（kprobe / procfs / vmap / RCU）在运行时补一个 proc 节点。
接口格式（写入 `"%99s %99s"`、读取 `"%s:%d,%s:%d"`）参照一加 6.6 官方源码
（OnePlusOSS 开源仓，GPL-2.0）实现，以便上层工具通用。项目中**不包含**
任何一加的专有代码或二进制。

## 十一、云更新（Gitee）

模块支持管理器内置的在线更新。`module.prop` 里有：

```
updateJson=https://gitee.com/turbowfz/ctn_patch/raw/main/update.json
```

管理器（Magisk / KernelSU）会定期拉这个 JSON，把里面的 `versionCode` 和本机
已装的比较，**远端更大就在模块列表里显示「更新」按钮**，点一下自动下载安装。

`update.json`：

```json
{
  "version": "v1.0",
  "versionCode": 10,
  "zipUrl": "https://gitee.com/turbowfz/ctn_patch/releases/download/v1.0/ctn_patch.zip",
  "changelog": "https://gitee.com/turbowfz/ctn_patch/raw/main/CHANGELOG.md"
}
```

### 11.1 发新版本的正确顺序

顺序很重要：**先定 tag 名，再改版本号，再发 Release**——因为 `zipUrl` 里带 tag，
顺序错了管理器下载会 404。

```bash
python bump.py 1.1        # 1. 改版本：module.prop 与 update.json 一起更新
                          #    （versionCode 自动 +1，必须递增）
# 2. 在 CHANGELOG.md 顶部补一段 v1.1 的说明
python build_zip.py       # 3. 重打 zip
git add -A && git commit -m 'v1.1' && git push github main && git push gitee main
# 4. 在 Gitee 建 v1.1 的 Release，把 ctn_patch.zip 传为附件
```

`bump.py` 会打印上面这套步骤并带上正确的 URL，照着做即可。
`python bump.py --show` 可以随时检查两个文件是否一致。

### 11.2 两个必须注意的点

**① 仓库必须公开。** 管理器是在**没有登录**的情况下拉 `update.json` 的，
所以：

- 私有仓库 → raw 地址返回 403，**云更新不可用**（实测：即使带 token 也是 403）
- 仓库公开 → `https://gitee.com/turbowfz/ctn_patch/raw/main/update.json` 可直接读

**绝对不能**把 access_token 写进 `update.json` 或 `module.prop` 来"绕过"这一点——
那等于把你的仓库写权限发给每一个装模块的人。

**② versionCode 必须单调递增。** 管理器只比这个整数，不比版本号字符串。
`bump.py` 已经强制校验，不允许填一个更小的值。

### 11.3 手动检查更新

不想等管理器的话，手机上直接看远端版本：

```bash
curl -s https://gitee.com/turbowfz/ctn_patch/raw/main/update.json
```

对比本机 `/data/adb/modules/ctn_patch/module.prop` 里的 `versionCode`。
