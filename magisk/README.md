# ctn_patch 模块包（Magisk / KernelSU / APatch）

本模块身份保持 `ctn_patch`、`v3.1`、`versionCode=31`。本说明是安装包文档，仓库根 README 是开发文档。
CTN 只从 COSA 数据库自动读取，不创建、读取或支持手动 `ctn.conf`；旧版遗留文件不会生效。

## 包布局与兼容性

- 根目录：`module.prop`、管理器脚本、`verify.sh`、`changelog.txt`、`oplus.png` 和本说明。
- `banner=oplus.png` 使用模块内相对文件名；支持横幅的管理器显示，不支持者忽略。
- `changelog.txt` 完整保留全部版本的短更新日志，每版最多四条，安装只显示当前版。
- `lib/common.sh`：所有入口共用的选择器、进程身份及启停逻辑。
- `bin/ctnd`：守护进程。
- `kernels/6.1.141/` 与 `kernels/6.1.157/`：各自的 `ctn_patch.ko`、`manifest.json`、`metadata.prop`。安装后保留包内所有 KO，下次启动重新选择，不能在 OTA 后沿用上次选择。
- 不包含用户配置、源码、LICENSE、测试线程程序。源码仓库保留 LICENSE；`out/user/fakethreads` 是独立测试产物，不是模块运行依赖。

安装和启动均按 `uname -r` 的 6.1.141 / 6.1.157 数字版本选择 KO，检查 `aarch64`、4096 字节页与厂商 game_opt 模块存在可读。完整内核版本与厂商模块字节身份记录在清单中但不作安装门槛；不在实测名单的固件仅显示提示。KO 加载兼容由内核 MODVERSIONS CRC 及模块自身的厂商符号、大小、分区和模块状态校验把关，不以 `insmod` 试错或轮换 KO。还会校验目标 KO、daemon、manifest 的 SHA256。

运行 shell 只读取白名单 `metadata.prop` 键，不 `source`/`eval` 元数据，不依赖 JSON 工具。列表以 `|` 分隔，`manifest.json` 保留 profile 与逐项核验记录。不支持的内核版本、损坏文件或来源不明的原生节点全部拒绝；目前没有获准的原生 daemon-only profile。

## 安装、禁用与运行

管理器从 ZIP 安装。安装检查不加载 KO、不写内核节点、不停止现有进程。已有旧 KO 并不证明新文件兼容，更不代表新文件已经运行。升级时保留 `disable`/`remove`；脚本不会替用户启用模块，已禁用设备保持禁用。

启用状态下，下次启动等待 `oplus_bsp_game_opt` 就绪，再选择并加载唯一匹配的 KO。仅当运行的 `version`、只读 `parameters/profile`、`parameters/build_id` 与包一致、`patch_ready` 为真且节点可用时启动 daemon。旧版缺少身份字段时保守拒绝接管，提示等待正常重启；绝不通过热卸载旧 KO 探测兼容性。

运行目录固定为 `/data/adb/ctn_patch`，不放在模块更新目录。daemon 独占 `.daemon.lock`，维护 `daemon.pid` 和内部 `restore.state`；服务独占 `.service.lock`。脚本核验 `/proc/<pid>/exe`、存活状态及启动时间，daemon 新旧文件比较已运行 exe 的 SHA256，不靠进程名、不扫全系统 fd、不杀无关 PID。`restore.state` 不是用户配置；只有 daemon 能解释和恢复它。

日志位于 `/data/adb/ctn_patch/boot.log`、`daemon.log`、`action.log` 和 `action-detail.log`。
安装输出由 Root 管理器保存，本模块不另外生成安装日志。卸载先停止自身服务，向精确
身份的 daemon 发 SIGTERM 并等原值恢复、退出和锁释放，再 `rmmod`。恢复债务仍在、
超时或卸载失败时保留 runtime 并返回失败，不强杀、不抹掉报错，不改管理器状态。

## 自检

管理器点击 Action（操作）按钮运行自检，默认只显示简短中文状态：适配是否匹配、
当前版本是否运行、数据库是否可读、演练与恢复是否完成。模块启用且空闲时检查节点
写读；禁用、待删除或游戏运行时只读，不加载或启用。失败或中断明确显示，保留退出码。

- 摘要日志：`/data/adb/ctn_patch/action.log`
- 完整技术诊断：`/data/adb/ctn_patch/action-detail.log`

```sh
sh /data/adb/modules/ctn_patch/action.sh             # 简短中文结果，两份日志分开保存
sh /data/adb/modules/ctn_patch/action.sh --read-only # 不做写入演练，仍保存日志
sh /data/adb/modules/ctn_patch/verify.sh             # 只读中文自检，不写日志
sh /data/adb/modules/ctn_patch/verify.sh --details   # 只读完整技术诊断，不写日志
```

详细模式分别报告磁盘 `FILE` 与正在运行的 `RUNNING` 身份；默认不打印完整哈希。
直接调用只读 `verify.sh` 不创建锁／日志、不启停 daemon、不加载／卸载、不写节点。
文件匹配不等于已经加载，静态检查通过不等于游戏效果已验证。

手动要求节点实测时也可使用下面命令（禁用/待删除状态仍拒绝）：

```sh
sh /data/adb/modules/ctn_patch/verify.sh --exercise
```

显式实测仅允许游戏空闲；暂停服务后及写前再次确认空闲。它会串行维护、等待 daemon
恢复，保存可往返的原始名单与 `ct_enable`，测试名单读写和非法输入，退出／异常时
恢复原值而不是固定默认值；只卸载本次脚本自己加载的 KO，并恢复此前运行的服务。
已有停止请求、测试中出现 `disable`/`remove` 时不恢复服务，也不清除已有标记。

写前生成绑定本次 boot ID 的内部 `exercise.state`，成功还原才删除；失败／中断留下
记录时，同次开机的服务重启和新自检会拒绝。重启后不重放旧内核的名单，只在确认
boot ID 不同后清理旧记录。此文件不是配置，不交给 daemon 解释。

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
