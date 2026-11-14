# ctn_patch 模块包（Magisk / KernelSU / APatch）

本模块身份保持 `ctn_patch`、`v3.0`、`versionCode=30`。这个 README 是安装包说明；仓库根 README 是开发文档，两者不是需要同步的副本。

## 包布局与兼容性

- 根目录：`module.prop`、`customize.sh`、`service.sh`、`action.sh`、`uninstall.sh`、`verify.sh`、本说明。
- `lib/common.sh`：所有入口共用的选择器、进程身份及启停逻辑。
- `bin/ctnd`：守护进程。
- `kernels/6.1.141/` 与 `kernels/6.1.157/`：各自的 `ctn_patch.ko`、`manifest.json`、`metadata.prop`。安装后保留包内所有 KO，下次启动重新选择，不能在 OTA 后沿用上次选择。
- 不包含用户配置、源码、LICENSE、测试线程程序。源码仓库保留 LICENSE；`out/user/fakethreads` 是独立测试产物，不是模块运行依赖。

安装和启动均先检查 `uname -r` 精确白名单、`aarch64` 与 4096 字节页，然后校验真实 `/vendor/lib/modules/oplus_bsp_game_opt.ko` 或 `/vendor_dlkm/lib/modules/oplus_bsp_game_opt.ko` 的完整 SHA256。所有可读候选都必须匹配，出现不同的 vendor 文件即拒绝。此保守 identity gate 绑定已核验的完整 vermagic、符号 CRC、KCFI、布局等 ABI 信息，不靠前缀猜测，也不以 `insmod` 试错。还会校验目标 KO、daemon、manifest 的 SHA256。

运行 shell 只读取白名单 `metadata.prop` 键，不 `source`/`eval` 元数据，不依赖 JSON 工具。列表以 `|` 分隔，`manifest.json` 保留 profile 与逐项核验记录。未知内核、未知 vendor、损坏文件或来源不明的原生节点全部拒绝；目前没有获准的原生 daemon-only profile。

## 安装、禁用与运行

管理器从 ZIP 安装。安装检查不加载 KO、不写内核节点、不停止现有进程。已有旧 KO 并不证明新文件兼容，更不代表新文件已经运行。升级时保留 `disable`/`remove`；脚本不会替用户启用模块，已禁用设备保持禁用。

启用状态下，下次启动等待 `oplus_bsp_game_opt` 就绪，再选择并加载唯一匹配的 KO。仅当运行的 `version`、只读 `parameters/profile`、`parameters/build_id` 与包一致、`patch_ready` 为真且节点可用时启动 daemon。旧版缺少身份字段时保守拒绝接管，提示等待正常重启；绝不通过热卸载旧 KO 探测兼容性。

运行目录固定为 `/data/adb/ctn_patch`，不放在模块更新目录。daemon 独占 `.daemon.lock`，维护 `daemon.pid` 和内部 `restore.state`；服务独占 `.service.lock`。脚本核验 `/proc/<pid>/exe`、存活状态及启动时间，daemon 新旧文件比较已运行 exe 的 SHA256，不靠进程名、不扫全系统 fd、不杀无关 PID。`restore.state` 不是用户配置；只有 daemon 能解释和恢复它。

日志位于 `/data/adb/ctn_patch/boot.log`、`daemon.log` 和 `action.log`。卸载先停止自身服务，向精确身份的 daemon 发 SIGTERM并等待恢复、进程退出和锁释放，再 `rmmod`。恢复债务仍在、退出超时或卸载失败时保留 runtime 并返回失败，不强杀、不抹掉报错。不修改管理器禁用/删除标志。

## 自检

管理器点击 Action（操作）按钮直接运行自检，终端会显示结果和**日志位置**：
`/data/adb/ctn_patch/action.log`。模块启用且无游戏运行时检查节点写读、边界输入和恢复；禁用、待删除或游戏运行时只查文件/兼容性，不加载或启用。失败退出码原样保留。

```sh
sh /data/adb/modules/ctn_patch/action.sh             # 直接自检并保存日志
sh /data/adb/modules/ctn_patch/action.sh --read-only # 只读自检，仍保存日志
sh /data/adb/modules/ctn_patch/verify.sh             # 只读检查，输出终端，不写日志
```

输出分别标明 `FILE` 文件版本/profile/build_id/哈希与 `RUNNING` 已加载版本/profile/build_id、daemon 已运行 exe 哈希。运行 KO 的整文件 SHA256 不由内核公开，不把 build_id 冒称为 KO SHA256。直接调用默认 `verify.sh` 不创建锁/日志、不启停 daemon、不加载/卸载、不写节点；静态通过不等于功能实测通过。

手动要求节点实测时也可使用下面命令（禁用/待删除状态仍拒绝）：

```sh
sh /data/adb/modules/ctn_patch/verify.sh --exercise
```

显式实测会串行维护、暂停已验证的自身服务、等待 daemon 恢复，保存可往返的原始名单与 `ct_enable`，测试名单读写和非法输入，退出/异常时恢复原值而不是固定默认值；只卸载本次脚本自己加载的 KO，并恢复此前运行的服务。不能安全恢复的状态会拒绝测试；期间出现 `disable`/`remove` 则不再恢复服务。

不会覆盖 HAL 的 `rt_info`，不会清空 dmesg，不会自动重启。真实 PID 仅核对现存 HAL 样本，没有样本明确记为 `SKIP`，不能把跳过伪称为全项通过。

## 构建包

打包器只接受 `profiles/<版本>/profile.json`、`out/<版本>/ctn_patch.ko` 和对应 `verification.json`，以及 `out/user/ctnd`。默认必须两份完整且针对这些文件的静态、运行核验均通过；逐项检查 `vermagic`、`symbol_crcs`、`kcfi`、`module_layout`、`undefined_symbols`、`elf_sections`，校验原始 profile 字节和 KO 哈希，不回退旧目录产物，也不按文件大小猜测。

```sh
python build_zip.py                         # 正式双 profile 包，缺件/未实测即拒绝
python build_zip.py --development           # 显式开发双 profile 包，允许 runtime=not_tested
python build_zip.py --profile 6.1.157        # 仅开发单 profile 包
python build_zip.py --out /path/to/full.zip # 自定义输出位置，不会发布
```

开发包文件名、包内 README 与 metadata 明确标注 DEVELOPMENT；不代表正式实测发布。所有文本归一 LF，ZIP 条目保存 Unix 文件类型和权限，并回读核对白名单、内容 SHA256 和权限。只写完成验证的 ZIP，不自动发布、不附加源码快照。
