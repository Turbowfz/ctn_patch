# 更新日志

## v1.2（versionCode 12）

**修掉一个会把所有升级都卡住的 bug。** 如果你装 v1.1 时被 `[不满足] 该节点已存在`
拒绝过，就是它。

**问题**：`customize.sh` 的检查 5 原来是「只要 `critical_task_name` 节点存在就
拒绝安装」。但升级的时候，KernelSU 是把新版装到 `modules_update`，**旧版的 ko
还在内存里跑着、节点还在**，于是新版一看节点存在就把自己拒了 —— 等于每升一次版
卡一次。这个检查本来是要挡「6.6 内核自带该节点」和「别的补丁已经建过」的。

**修法**：把「节点存在」拆成两种完全不同的情况，分别处理：

| 情况 | 判定 | 处理 |
|---|---|---|
| 节点在，且 `ctn_patch` 正在运行 | 节点是**我们自己**建的 → 这是升级 | **放行**，重启后新版接管 |
| 节点在，但 `ctn_patch` 没运行 | 内核自带（6.6 那种）或别的补丁建的 | 拒绝（保持原样） |

为什么「`ctn_patch` 在跑」就能断定节点是我们的：如果内核本来就有这个节点，我们
`insmod` 时 `proc_create_data` 会返回 NULL、`init` 直接 `-EEXIST` 退出，
`ctn_patch` 就不会出现在 `/proc/modules` 里。所以两者等价。

顺带修正检查 3 的 refcount 提示：升级时 refcount 是 2（旧版还钉着
`oplus_bsp_game_opt`），原来那句「装上后会 +1」在这种场景下有歧义，现在会说明
「其中 1 是本模块旧版在引用」。

**`ctn_patch.ko` 和 `ctnd` 都没动**，哈希与 v1.1 相同，只有 `customize.sh` 变了。

**真机验证**（一加 Ace 3 Pro / 6.1.141 / KernelSU）：在「模块正加载」的升级
场景下用 `ksud module install` 装本版，检查 5 正确放行、9 组检查全过、
`Module installed successfully!`。

**校验值**
```
ctn_patch.ko  sha256 be9a686a1901931a7cd82f76df00e1f268820ed459427f86e8ee90aef2cd37f1
ctnd          sha256 7a47dc6844cc37d5933b7fbff023b1ed608a4682af7c6ba2a5d19971dd94e0f1
zip           sha256 98234523fe84770f296136e7a8f5931f59d1ab6a6617f578b956b2f20245e9c5
```

> v1.1 的刷入包有上述 bug。不过 `customize.sh` 是**被安装的那个 zip 里**的脚本，
> 所以从 v1.1 升到本版没问题（用的是本版修好的脚本）。全新安装或从 v1.0 升，
> 直接用本版即可，不用管 v1.1。

## v1.1（versionCode 11）

**daemon（ctnd）自己的版本号从 1.1 起算** —— 它和内核模块是两条独立的版本线。
本版改动全在 daemon 与脚本；`ctn_patch.ko` 的功能代码没动，只把模块内的
`MODULE_VERSION` 同步成 1.1（`.ko` 里唯一变化的字节就是这个版本串）。

**性能**
- 配置缓存：同一个包只解析一次；库没变（比对 db + wal 的
  `mtime/size/inode` 指纹）就直接用缓存，不再重复拷 600KB 的库
- 轮询 500ms → 800ms，且只在 `game_pid` 真的变化时才干活

**占用**
- 只拷 db + `-wal`，不再拷 `-shm`：`-shm` 是 COSA 正在 mmap 的共享内存，
  拷它既没必要（SQLite 会自己重建），还可能拷到撕裂内容
- 缓存上限 64 条，长期运行不会无限增长

**兼容性 —— 云控注入不管明文还是加密都有路可走**
- 明文 JSON：直接取 `ctn` / `ctb`
- base64 编码的 JSON：先解码再取（自带解码器，含**无填充**残余处理）
- 加密 / 未知形态：**不硬猜** —— 日志明确告警 + 打印「本地覆盖文件该怎么写」，
  本次先写回默认值
- 新增本地覆盖文件 `/data/adb/ctn_patch/ctn.conf`（优先级高于云控库），
  首次开机自动铺一份带注释的示例；云控没覆盖的游戏、或库是加密内容时，
  手写一行就能用 —— 保证「注入」这条路始终走得通
- 新增 `--db <路径>`：云控库不在内置候选列表里（换机型/换 OPLUS 版本）时手工指定
- 新增 `--version` / `--help`

**修掉的问题**
- 打包时 zip 条目的 `create_system` 写成了 0（DOS），解包器因此无视 Unix
  权限位，`ctnd` 装到手机上变成 0644 —— 表现是「模块装上了但 `daemon.log`
  不生成」。`build_zip.py` 现在设 `create_system=3` 并回读校验，
  `customize.sh` 也补了 `set_perm` 兜底
- `get_package_name()` 取 `/proc/<pid>/cmdline` 时按第一个 NUL 截断
  （之前会把整个 cmdline 连参数一起拷进缓冲区）

**测试**
- 新增 `daemon/test.sh`：PC 上原生 gcc 跑解析逻辑单元测试（base64 各种形态、
  明文/base64 JSON 取值、加密形态识别、名字归一化、`game_pid` 解析、
  本地覆盖文件解析）。写这套用例时抓出两个真 bug：无填充 base64 尾部被丢、
  base64 形态下 `ctb` 取不到
- 新增 `daemon/device_e2e.sh` + `daemon/fakepkg.c`：真机上用合成云控库
  把 8 种配置形态 + `ct_enable` 代管 + 缓存 + 单实例锁走一遍（17 项全过）

**真机验证**（一加 Ace 3 Pro / 6.1.141 / KernelSU，v1.1）
- 内核模块：`.gnu.linkonce.this_module` = 1088 字节（与设备模块逐字相等）、
  vermagic 一致、kCFI 类型号一致；`insmod` / 读写 / 拒非法输入 / `rmmod` 全部正常，
  dmesg 无 BUG / WARNING / oops / CFI failure
- 刷入：`ksud module install` 全 9 组检查通过，`ctnd` 解包后权限 0755
- daemon：`--db` 合成库跑通 17 项；真实 COSA 库跑通 5 个包
  （`pubgmhd` → `RenderThread Thread-`、`yh.laohu`/`mingchao` →
  `GameThread RenderThread`、Unity 游戏与库外包 → 默认值）
- 真实游戏端到端：启动和平精英 → HAL 写 `game_pid` → ctnd 注入
  `RenderThread Thread-`（与游戏内实际线程名一致）→ 退出后 6 拍防抖恢复默认

**校验值**
```
ctn_patch.ko  sha256 be9a686a1901931a7cd82f76df00e1f268820ed459427f86e8ee90aef2cd37f1
ctnd          sha256 7a47dc6844cc37d5933b7fbff023b1ed608a4682af7c6ba2a5d19971dd94e0f1
zip           sha256 ce1113f96659fa4e797d9b39504dcbdd5c563a732f349f5d9e9127f8b9b6a125
```

## v1.0（versionCode 10）

首个可用版本。

**功能**
- 补回 `/proc/game_opt/task_boost/critical_task_name` 可写节点（6.6 官方接口）
- 写入格式与官方 6.6 逐字对齐：恰好两个名字，空格/TAB/换行分隔
- 读取格式 `名字:pid,名字:pid`（6.1 无 pid 数据，固定 `-1`）

**实现**
- kprobe 取 `kallsyms_lookup_name`，按 `模块名:符号名` 解析目标符号
- `vmap` 建 `.rodata` 可写别名（6.1 名单是指针数组且在只读段）
- 双缓冲 + `synchronize_rcu()` 换指针，适配 `sched_switch` tracepoint 读者
- `try_module_get()` 钉住 `oplus_bsp_game_opt`，防止符号变野指针
- 卸载完全可逆，不修改任何分区文件

**兼容性**
- 内核 `6.1.141-android14-11-o-*`
- 8Gen3 / sm8650 一加系列（Ace3Pro、Ace5、一加12、Pad2）
- Magisk / KernelSU / APatch

**刷入时自动检查 9 组条件**，不满足直接拒绝安装（内核版本、vermagic、
目标模块是否为模块、厂商模块 sha256、节点是否已存在、内核配置项、
运行时符号、页大小与 SELinux）。

**真机验证**（一加 Ace 3 Pro / 6.1.141 / KernelSU）
- `insmod` 成功，节点创建，默认值 `UnityMain:-1,UnityGfxDevice:-1`
- 写入/读回正确；单名与三名被拒；多空格与制表符接受
- 连续 8 次写读稳定（压双缓冲 / RCU 路径）
- `rmmod` 完整还原，dmesg 无 BUG / WARNING / oops / CFI failure

**校验值**
```
ctn_patch.ko  sha256 dc071abd3fd674715b5a31cc704f39ef955c5a2a0950ca347e1436dbe09292be
zip           sha256 975ebf077d155286608ed0c8ddb2a85707bb71ce4e4b55ee3a8b27581ddf086c
```
