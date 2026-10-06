# 更新日志

## v1.5（versionCode 15）

**厂商模块闸门从「比哈希」改成「比 `struct module` 布局」——跨机型可通用了。**
内核模块和 daemon 都没动。

**问题**：一加12 / Ace3Pro / Ace5 / GT6 这些 8Gen3 机型，在**同一内核版本**
（6.1.118、6.1.141…）下官方风驰代码是**同源**的，本该通用；但检查 4 要求
设备上的 `oplus_bsp_game_opt.ko` 与构建时那份 **sha256 逐字节相同**，而不同机型
/不同构建批次编出来的 ko 哈希天然不同 —— 结果把本来能用的机型全拒了
（实测在另一台设备上刷 v1.4 就撞上这条）。

**改法**：判**布局**而不是哈希。设备上那份厂商 ko 是给那个内核编的，它的
`.gnu.linkonce.this_module` 段大小 == 那个内核的 `sizeof(struct module)`；跟
我们 `.ko` 的比一比，就知道装载器会不会越界写 —— 这是**唯一**会导致 insmod
崩机（而不是干净失败）的差异。

| 情况 | 判定 |
|---|---|
| 哈希一致 | `[OK]` 完全一致 |
| 哈希不同、段大小相同 | `[OK]` 另一份构建、布局一致 → **放行** |
| 哈希不同、段大小不同 | `[不满足]` → 拒绝（会崩机） |
| 哈希不同、量不出段大小 | `[警告]` → 放行 + 提示看 boot.log |

**新增 `elf_sec_size.sh`**：读 ELF64 某段的大小，只用 `od` + `awk`（设备上没有
`readelf`）。踩了两个坑记在注释里：GNU `od` 会把重复行折叠成 `*`（必须加 `-v`，
否则读出来的字节数不对）；`od` 输出会分多行，awk 必须先攒齐再算。

**为什么现在敢放松**：本 `.ko` 的符号 CRC 取自设备模块表，与它自己的实际布局
一致（`build.sh` 第 8 步硬校验段大小）。所以在别的内核上若 CRC 对不上，内核会
**干净地拒绝加载**（`disagrees about version of symbol`），不会崩机。之前那次
崩机是我自己关掉 BTF 导致 `.ko` 与它声明的 CRC 自相矛盾，不是设备差异造成的。

**真机验证**（一加 Ace 3 Pro）：四个分支全过 —— 同哈希放行；改过 `.comment` 的
假 ko（哈希不同、布局 1088）**放行**；把段大小改成 1024 的假 ko **拦截**；
非 ELF 文件走警告分支。安装输出仍是 9 行进度。

**校验值**
```
ctn_patch.ko  sha256 be9a686a1901931a7cd82f76df00e1f268820ed459427f86e8ee90aef2cd37f1
ctnd          sha256 7a47dc6844cc37d5933b7fbff023b1ed608a4682af7c6ba2a5d19971dd94e0f1
zip           sha256 54dc67b08cc74da41ef14abba7dd926fad4628c9feb7eadf386a09639e780e1a
```

> `ctn_patch.ko` 和 `ctnd` 的哈希与 v1.2~v1.4 相同（没动），只有脚本和文档变了。

## v1.4（versionCode 14）

**安装界面改成进度式**（用户要求：平时只要进度，被拦才显示完整明细）。
`verify.sh` 也修掉一个会误报失败的项。内核模块和 daemon 都没动。

**安装输出 40 行 → 13 行**：9 组检查一行一个（`[3/9] 目标模块 oplus_bsp_game_opt ✓`），
像进度条一样刷下去。**只有被拦截的那一组才展开完整明细** —— 不满足的原因、
对比数据、怎么处理，全在这组里给足；其它组照常一行 ✓。警告不拦截安装，
平时不展开（一行 ⚠ 带过），结尾统一列出，被拦时随该组一起展开。
真机验证过两种形态：全部通过（13 行）、厂商模块哈希不匹配被拦（只展开第 4 组）。

**修掉「daemon 日志」项的误报**：它原来扫 daemon.log 近 200 行的**全部历史**，
把升级/重载过渡期的旧记录也当失败 —— 比如「锁: 已有另一个 ctnd 在跑」。
那条其实是**自愈噪音**：停 daemon 的瞬间守护循环抢跑又拉一个，新的撞锁后
自己退出（退出码 3），守护循环也识别退出，**没有留下双实例**。现在：

- 用「行数水位」（`.verify_logmark`）只看**本次自检之后新增**的日志行，
  首次检查只立水位不评价历史；
- 「撞锁自愈」单独归为一档备注 `[--]`（打出来让人看见，不算失败）；
  真正需要人为处理的（加载失败/写入失败/缺符号/找不到云控库）才算失败。

**守护循环不再抢跑**（`service.sh`）：拉起 ctnd 前再查一遍 `.stop` 哨兵、
并确认没有 ctnd 在跑（原来 pkill 没杀干净时这里会叠一个）；轮询间隔
5 秒 → 2 秒，减小停/启过渡期的抢跑窗口。那两条「已有另一个实例」的日志
源头就是这个，修完理论上不再出现。

**校验值**
```
ctn_patch.ko  sha256 be9a686a1901931a7cd82f76df00e1f268820ed459427f86e8ee90aef2cd37f1
ctnd          sha256 7a47dc6844cc37d5933b7fbff023b1ed608a4682af7c6ba2a5d19971dd94e0f1
zip           sha256 8eb8b56c6ea7d5e8e1175d6ec3a35ff883e99aa00b891139b9113800665280ab
```

> `ctn_patch.ko` 和 `ctnd` 的哈希与 v1.2/v1.3 相同（没动），只有脚本变了。

## v1.3（versionCode 13）

**自检重做：覆盖更广、输出更短、一眼能看出问题。** 内核模块和 daemon 都没动。

**覆盖更广（9 项 → 15 项）**：原来只测内核模块，daemon 一个字都没测。现在加了
daemon 是否在跑/是否单实例/版本、daemon 的依赖（SQLite 库、COSA 云控库、
`ctn.conf`）、daemon 日志里有没有失败记录、模块文件是否齐全（**含 `ctnd` 的
可执行位** —— 这个丢过一次，装完 daemon 起不来）、厂商模块 sha256 比对。

**输出更短**：从 ~64 行压到 ~20 行。一项一行，只有失败时才在下面缩进打印细节。
三档标记：`[OK]` / `[!!]`（失败）/ `[--]`（跳过或备注）。最后一行汇总，
脚本退出码 = 失败项数，可以直接被别的脚本调用。

**修掉两个让自检「测了个寂寞」的问题**：

- `action.sh` 原来**先把模块加载好**再调 `verify.sh`，于是「insmod」和「卸载」
  两项永远显示"跳过" —— 而这两项恰恰是最该测的。现在改成先 `rmmod`，让自检
  自己 `insmod` → 测试 → `rmmod`，最后再把模块和 daemon 恢复起来。
- 自检末尾自己恢复运行状态（重新加载模块 + 拉起 daemon），不再把设备留在
  「模块没加载 / daemon 没跑」的状态。

**dmesg 扫描的漏洞**：原来如果 `/dev/kmsg` 写不进去（标记进不了 dmesg），日志段
是空的，脚本会**静默报 PASS** —— 等于什么都没查却说没问题。现在报一条失败让人
看见。

**「瞬时旧值」如实处理**：实测遇到过一次节点读回「好几轮之前的旧值」。之后用
900+ 次迭代（daemon 开/关、每轮重新加载模块、300 轮大循环）都复现不出来，模块
的双缓冲逻辑逐行看也没问题。处理办法不是掩盖：读回不一致时**立刻重读两次**，
重读对上就记一条「瞬时旧值」备注（不算失败）并把首次读到的旧值原样打印；重读
也对不上才算失败。这样自检不会抖，真出问题又能看到现场。

**新增 `stress_node.sh`**：专压双缓冲/RCU 那条路的写读压测，按需跑几千次
（`verify.sh` 只写 8 次，量太小）。

**校验值**
```
ctn_patch.ko  sha256 be9a686a1901931a7cd82f76df00e1f268820ed459427f86e8ee90aef2cd37f1
ctnd          sha256 7a47dc6844cc37d5933b7fbff023b1ed608a4682af7c6ba2a5d19971dd94e0f1
zip           sha256 e3a2186906c3a958d1bda22f1d3c44b3d7b52b91d2fd6e85f9df3e6ebdcb8ec6
```

> `ctn_patch.ko` 和 `ctnd` 的哈希与 v1.2 相同（没动），只有脚本和文档变了。

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
