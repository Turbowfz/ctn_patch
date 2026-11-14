# CTNP / CTN Patch

为已核验的一加 SM8650（8 Gen 3）6.1 内核补出
`/proc/game_opt/task_boost/critical_task_name`，由 `ctnd` 从 COSA 数据库读取
`game_config.ctn` 后应用。没有本地 CTN 覆盖文件，也不读取 `from_server`。

模块 ID 保持 `ctn_patch`，当前版本 `v3.0 / versionCode 30`。v3.0 适配
Android 17 的 6.1.157 内核，保留 6.1.141 兼容；旧版 `v2.2 / versionCode 22`
仅面向 6.1.141，已停更但 Release 继续保留可下载。

## 当前验证范围

| 基线 | 设备/固件来源 | 静态验证 | 本轮真机验证 |
|---|---|---|---|
| 6.1.141 | PJX110，保存的 16.0.5.701 手机提取物 | 构建、vermagic、CRC、KCFI、布局通过 | 未测试；旧版本测试不能替代本次 KO 测试 |
| 6.1.157 | PJX110，17.0.1.201 手机提取物 | 构建、vermagic、CRC、KCFI、布局通过 | 临时 KO 加载、名字写读、输入拒绝、恢复和卸载通过；无游戏时真实 PID 样本跳过 |

“兼容 141/157”不代表所有机型和所有同版本固件都自动兼容。安装与开机选择器还会
核对完整内核 release、厂商模块文件身份和包内文件哈希。未知基线拒绝加载，不轮流
试两份 KO。当前未批准原生节点的 daemon-only 模式。

## 原始方案与重写边界

最初的内核方案来自用户提供的 `ctb_name_patch.c`：

- 旧 `critical_task` 是只读段里的两指针数组，不是新版的内联字符数组。
- 通过 `vmap` 创建同一物理页的可写别名，发布名字指针。
- 双缓冲配合 `synchronize_rcu()` 后复用；卸载先排空 proc 回调，再还原指针。

重写保留上述核心，不新增调度、频率或热控功能。补充目标模块引用保护、精确的
对象大小/内存区域校验、动态函数 KCFI 检验，以及安全的 PID 身份查询。不能核实
PID 数据源时只显示 `-1`，不裸读厂商可能失效的 task 指针。

## 输入与读取

节点接受一到两个名字，以空白分隔；单名复制到两槽。每个名字最多 99 字节。
第三个名字、全空白、嵌入 NUL、越界长度或非零写偏移拒绝。

```sh
# 以下会改节点；仅在匹配的已验证模块上手动调试时使用。
printf '%s\n' 'GameThread RenderThread' > /proc/game_opt/task_boost/critical_task_name
cat /proc/game_opt/task_boost/critical_task_name
# 例：GameThread:1234,RenderThread:1235；没有已注册匹配线程则为 -1。
```

两个槽记录同一线程不产生双倍加成。99 字节是接口上限；task 的 comm 最多
15 个名字字节，因此更长的名字不能匹配真实线程。

## 数据库和恢复行为

源数据库全程只读。默认候选是 COSA 的 `db_game_database`；`-d` 指定路径时不回退
到真实库，方便隔离测试。数据库和存在的 WAL 要完整复制到私有临时快照，前后
文件身份、尺寸、精细时间和 WAL 状态一致才使用；失败保持节点状态。

| 情况 | 行为 |
|---|---|
| 查询到合法非空 CTN | 规范化并写节点，写成功后才提交应用状态 |
| 游戏运行中 CTN 变化 | 默认每 30 秒复查，变化后下发 |
| 明确移除、空串或纯空白 CTN | 撤销本次仍拥有的应用，恢复原值 |
| 直接切换到无 CTN 游戏 | 撤销前一个游戏的应用 |
| 库、SQL 或解析失败 | 不当作删除；保持现状，按重试周期再查 |
| 写入/恢复失败 | 保留未完成状态和恢复债务，不能假报成功 |
| 正常退出、停止或同次启动崩溃后重启 | 对仍由 daemon 拥有的节点偿还恢复债务 |
| HAL/用户已另写节点 | 让出所有权，不覆盖其新值 |

轮询为 800ms，连续负 PID 约 5 秒才认定退出；正 PID 和未知读数会中断负 PID
计时。缓存指纹按包保存，避免其它游戏查询把旧缓存伪装成新缓存。

默认只管理 CTN，不额外开启 CTB。保留调试 CLI：

```sh
bin/ctnd -V
bin/ctnd -h
bin/ctnd -d /absolute/path/to/test.db -r 5
# -e 仅显式选用：独立按 ctb 管 ct_enable，并让 HAL/外部写入优先。
```

没有 `ctn.conf` 或 `ctnd.conf`。运行目录 `/data/adb/ctn_patch` 下的
`restore.state` 是程序内部的原值/恢复债务日志，绑定 boot ID，不是可编辑配置。

## 源码与安装结构

```text
ctn_patch.c / kernel/       内核源码及实际输入解析器
profiles/6.1.141/           141 独立配置、CRC 和证据描述
profiles/6.1.157/           157 独立配置、CRC 和证据描述
daemon/                    数据库、解析、状态机、运行文件及测试源码
magisk/                    安装、启停、卸载与公共选择器源码
tools/                     固定源码获取、ABI 解析、构建及核验
tests/                     安装/打包/构建工具回归
out/                       构建产物和逐项报告，不入 Git
```

安装包根目录是管理器脚本和 `module.prop`；可执行 daemon 在 `bin/ctnd`，
公共脚本在 `lib/common.sh`，两套 KO 与清单在 `kernels/<版本>/`。
运行锁、PID、日志与内部债务放在独立 runtime，避免升级目录变化。
禁用/删除标记不被自检或开机脚本解除。

包内不放测试库、构建工具、conf 或独立 LICENSE 文件。源码保留 GPL-2.0-only
许可证全文和 SPDX；内核的 `MODULE_LICENSE("GPL")` 不能去掉。

## Debian 构建与检验

使用现有 **WSL Debian 13**；无需重装发行版。编译工具优先使用已装的 Linux
NDK r26b（Clang 17.0.2），最终以产物 ABI 核验为准。构建目录每轮独立，不写源码
根目录中的旧发布二进制。

```sh
# Debian 原生编译器、真实解析/状态/SQLite/进程回归及 sanitizers。
bash daemon/test.sh
python3 -I -B tests/test_packaging.py
python3 -I -B tests/test_profile_tools.py

# 已准备好固定源码时，明确选 profile。
bash build.sh --profile 6.1.157 --srcdir /path/to/pinned/kernel \
  --work /path/to/new-work --clang /path/to/linux-ndk/bin/clang --jobs 4

# 自动取固定的 OnePlusOSS 源码与 modules，验证归档哈希，安全解包。
python3 -I tools/fetch_profile.py --profile 6.1.157 --out /path/to/new-source-download

# 全部用户态程序也在 Debian 从源码构建。
bash daemon/build.sh
bash daemon/build_tools.sh
```

141 参考源码是 OnePlusOSS `android_kernel_oneplus_sm8650` 固定提交
`3ce62e9b9534b4cac64300b91a7ead2b0deda495`；157 参考源码是官方
`android_kernel_common_oneplus_sm8650` 固定提交
`7a7ae28e849b0f9b42b8b6fc26feebe1864bf7ac`，实际 Makefile 为 6.1.157。
后者来自 SM8650 13R 分支，**不声称是 PJX110 当前 OEM 内核的精确源码**。
手机提取的配置、CRC、BTF 和厂商函数证据才是适配基线。

逐项报告 `verification.json` 绑定原始 profile 字节、KO 哈希、编译器与 build ID。
静态通过包括完整 vermagic、每个导入 CRC、回调 KCFI、真实结构布局及必要 ELF
段。动态函数在第一次间接调用前再核对现机 KCFI，失败直接拒绝。

CI 的 Debian 容器从源码构建两套 KO 和用户态程序，先跑回归测试，只输出标明
未实机测试的开发候选。更新后的 CI 本轮未推送、未云端运行，不能称云端通过。

## 打包与设备测试

```sh
python3 -I build_zip.py --development --out /path/to/ctn_patch-development.zip
# 正式双版本包：两份本次 KO 的实机核验都通过才允许。
python3 -I build_zip.py --out /path/to/ctn_patch-full.zip
```

缺件、哈希不符、静态失败或缺真实验证记录时拒绝，不回退旧产物。141 本轮未实机
验证，因此目前只能提供明确标记的开发包，不能称为全验证正式包。

Action（操作）按钮直接运行自检，保存结果到
`/data/adb/ctn_patch/action.log`，并在终端显示日志路径。模块启用且空闲时执行
受控写读/恢复；禁用、删除中或游戏运行时自动只读检查，不解除禁用。
`action.sh --read-only` 可强制只读；直接调用默认 `verify.sh` 仍只读且不写日志。
文件身份与运行身份分别报告，失败退出码原样保留。

141 继续保留旧版的一/两名写法、读取格式、数据库下发和独立 KO。历史可用性保留
为历史记录，不用新版策略替换成其它调度；仍需对应版本设备补本轮完整实测。

本轮额外临时 KO 测试是用户明确允许的独立测试，保持原先禁用标记、结束卸载。
不修改 `rt_info`、不写 `game_pid`、不清空 dmesg、不自动重启手机。

详细本轮检验状态见 [docs/重写检验报告.md](docs/重写检验报告.md)。
