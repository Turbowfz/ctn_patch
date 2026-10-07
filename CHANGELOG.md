# 更新日志

## v2.3（versionCode 23）

**修掉 v2.2 留下的一个自相矛盾：自检的边界项还在按旧语义期望「单名被拒」。**
v2.2 把内核改成接受单名，却忘了同步 `verify.sh` 里的期望值，于是自检会报一条
假的 `[!!] 边界输入 单名被接受(应拒)`。（算是个好例子 —— 自检确实抓到了改动遗漏。）

现在那一项改成：
```
[OK]  边界输入   单名接受(补成两槽)/16字符 接受；三名/120字符/全空白 被拒
```
也就是**同时校验新语义**：写一个名字不但要被接受，还要确认内核真的把它补成了
两槽（`onlyone:-1,onlyone:-1`），而不只是"没报错"。

**真机验证**：装 v2.3 跑完整自检 → 3 秒、**13 项全过**。

**校验值**
```
ctn_patch.ko  sha256 2b40becca988958606447908135ad720e4c2b58e7ba04e320410dac3e82533cf
ctnd          sha256 a621a09a189c4552cdb27577f579347677a4d03043a9553b67d5a4a37798af4a
zip           sha256 98a28c43027e192e18b571bbaa0b77a995ea5c6556de23b3ce5c254843bbdb08
```

> ko 和 ctnd 与 v2.2 相同（只改了自检脚本）。**建议直接用 v2.3**。

## v2.2（versionCode 22）

**按「性能 / 速度 / 占用 / 兼容性」整体过了一遍。最大的一项：ko 剥掉调试段，
从 305KB 降到 27.8KB（-91%），刷入包 394KB → 117KB（-70%）。**

### 1. 写一个线程名也生效（兼容性）

旧版要求**恰好两个**名字（对齐官方 6.6 的 `sscanf("%99s %99s")`），只写一个会被
`-EINVAL` 拒掉。现在**一个也行**：内核把第二个槽自动填成同一个名字。

理由：内核里两个槽记同一个线程本来就**等价于一个**（`decide_boost_status` 只是对
同一个 CPU 重复 `cpumask_set_cpu`，不会双倍加成）。而且很多游戏真正关心的就是
一个渲染线程，调用方不该为了绕开限制把名字写两遍。

实测：`echo SoloThread > 节点` → `SoloThread:-1,SoloThread:-1` ✓；
云控给 `ctn="SoloThread"` 也直接生效 ✓。三个名字/全空白仍然拒绝（语义没松）。

### 2. ko 剥掉调试段：305KB → 27.8KB

这个 `.ko` 里 `.debug_info`（84KB）+ 它的重定位（136KB）+ `.debug_str`（26KB）等
调试段占了约 **270KB**，而**装载器一个都不用**。内核安装模块时
（`INSTALL_MOD_STRIP=1`）做的就是这件事，我们在构建脚本里补上了这一步
（`llvm-strip --strip-debug`）。

只剥 `.debug_*`，绝不碰 `.text` / `.modinfo` / `.gnu.linkonce.this_module` /
`__versions` / `.rela.*`。剥完**校验跑在剥完之后**，确认：段大小仍是 **1088**
（和设备模块一致）、vermagic 一致、kCFI 类型号一致。

### 3. daemon 不再每次启动游戏都重拷云控库（速度 / 占用）

旧版的缓存用「库指纹」判失效，但云控库是 WAL 模式，**`-wal` 每秒都在变**
（COSA 自己也在写）→ 指纹每次都变 → **缓存永不命中** → 每个游戏每次启动都要
重拷 600KB 库 + 重开一次 SQLite（日志里那句「内容有变，缓存失效」每次都出现）。

改成 **TTL 缓存（默认 600 秒）**：这段时间内直接用缓存，过了才重查一次。
代价是云控改了配置最多 10 分钟后生效（重启 daemon 立刻生效）。

实测同一包连开三次：第 1 次 20ms CPU（要查库）→ 第 2、3 次 10ms（命中缓存）。

### 4. 查完就把临时库删掉（占用 / 隐私）

以前查完那 600KB 的库副本会一直摊在 `/data/local/tmp/.ctnd/`。现在查完立刻删
（db + wal + shm），空闲时只剩一个空目录。既省盘，也**不把用户云控库的副本
一直留在盘上**。

### 5. 实测数据（一加 Ace 3 Pro）

| 项目 | v2.1 | v2.2 |
|---|---|---|
| `.ko` 体积 | 304,640 字节 | **27,776 字节**（-91%） |
| 刷入包体积 | 394,340 字节 | **117,084 字节**（-70%） |
| daemon 空闲内存 | 5.7MB（sqlite 常驻） | **3.7MB**（不查库就不加载 sqlite） |
| 空闲磁盘 | 600KB（临时库常驻） | **0**（只剩空目录） |
| 每次启动游戏 | 重拷 600KB + 开 SQLite | **命中缓存，10ms** |
| 自检耗时 | 3 秒 | 3 秒 |

> libsqlite 不做 dlclose：实测 **bionic 的 dlclose 不会真卸载**（Android 已知
> 行为），而且它是全系统共享库，边际成本只是页表那点 —— 所以没做无用的卸载。

**校验值**
```
ctn_patch.ko  sha256 2b40becca988958606447908135ad720e4c2b58e7ba04e320410dac3e82533cf
ctnd          sha256 a621a09a189c4552cdb27577f579347677a4d03043a9553b67d5a4a37798af4a
zip           sha256 04d25ab4dcbc5d20d1a04c3e08693c10f8651bae0799c390e5f1df2e2c2a140c
```

## v2.1（versionCode 21）

**接着 v2.0 修两个问题 —— 都直接影响后台开销（也就是耗电）。**
`ctn_patch.ko` 与 v2.0 相同（这版没动内核模块）。

### 1. daemon 存活检测：从「扫全系统 fd」改成「读 pidfile」

v1.9 为了绕开 `pgrep` 的语义差异，用「扫 `/proc/<pid>/fd` 找谁握着锁」来判断
daemon 在不在跑。**结果是对的，但极慢**：有的进程有 800+ 个 fd，全系统扫一遍是
几万次 readlink；而 `service.sh` 的守护循环**每 2 秒**就调一次 —— 等于**持续在
后台扒全系统的文件描述符**。

实测：自检卡在这一步几分钟出不来（执行轨迹刷到 6.7 万行还在扫）。**这本身就是
一笔持续的后台 CPU 开销，会实打实体现在耗电上。**

**改法**：daemon 启动时把自己的 pid 写进模块目录的 `ctnd.pid`，退出时删掉。
脚本只读这一个文件 + 看一眼 `/proc/<pid>`，O(1)。陈旧 pidfile 会再核对 `comm`，
兜底才是扫 `/proc/<pid>/comm`（只有几百个目录，很便宜）。

**实测：自检从「卡几分钟」变成 3 秒。**

### 2. 修掉一个把 `.stop` 哨兵注释掉的 bug（v1.7 引入，v1.7~v2.0 都带着）

v1.7 插那段进程探测代码时，结尾的 `# ---- ` 和下一行**粘在了一起**，把
`STOP="$MODDIR/.stop"`（`uninstall.sh` 里是 `MODDIR=${0%/*}`）**变成注释**了。

后果：守护循环永远收不到停止信号 → **它会不停地重启 daemon**。日志里反复刷的
`锁: 已有另一个 ctnd 在跑` 就是它造成的 —— **一直有后台活动在跑**。

### 3. 顺带

- `verify.sh` 在 `daemon.log` 不存在时不再喷一行 shell 错误
- daemon 的关键事件同时写 `/dev/kmsg`（v2.0 加的），配合本条修复，现在
  `dmesg | grep -E 'ctn_patch:|游戏启动|不写节点|恢复原值'` 一条命令就能看到
  「内核 + daemon」完整时间线

**校验值**
```
ctn_patch.ko  sha256 afb3c7cbe2760153299ff4fb0aeaba658cda2cb57fa9a18839e0458e124b0848
ctnd          sha256 7c1f1049d44c5b8cc801d897178adb82a523e57558514a9aefab1c89ac8008d8
zip           sha256 f135bddb8a4af31e917e71f19aeaedc0f4e0b1aafc93a8b13efb5c23a47a9185
```

> v2.0 的 daemon/脚本有上面两个问题，**建议直接用 v2.1**（v2.0 的 ko 与 v2.1 相同）。

## v2.0（versionCode 20）

**按「刷入后功耗暴增」的反馈重写。核心改动：没有配置就一个字节都不碰节点，
并且移除了 `ctn.conf`。**

**先说清楚功耗这件事的来源**：模块本身不改功耗 —— 它只是把可写节点暴露出来。
真正改变系统行为的是 **daemon 写进节点的那串线程名**：写进去之后，内核的
critical-task boost 就会针对这些线程生效（这是本模块的功能）。所以这一版把
「什么情况下会去动内核」收窄到最小。

### 1. 没有配置 → 不写节点（旧版会写）

| 云控里的情况 | 旧版（≤v1.9） | 现在（v2.0） |
|---|---|---|
| 有 `ctn` | 写进去 | **写进去**（唯一会写的情况） |
| 有配置但没 `ctn`（Unity 游戏） | 写回 `UnityMain UnityGfxDevice` | **不碰节点** |
| 库里没有这个游戏 | 写回 `UnityMain UnityGfxDevice` | **不碰节点** |
| 配置是加密/未知形态 | 写回 `UnityMain UnityGfxDevice` | **不碰节点** |

旧版那三种「写回默认值」虽然值和内核默认一样，但等于**每启动一个游戏都去动一次
内核**（每次写都要在内核里做一次 RCU 同步换指针）。现在这些情况 daemon 对内核
**零影响**。

### 2. 退出时恢复「原值」而不是写死

游戏退出时不再硬编码写回 Unity 串，而是恢复成**启动时读到的那个值** —— 万一节点
被别的工具设过，不会被覆盖。本次没动过节点的话，退出时什么都不做。

### 3. 移除 `ctn.conf`

连同 `ctn.conf.example`、开机的铺开逻辑、daemon 里的查找逻辑一起删掉。
指定关键线程名现在只有一条路：云控配置里的 `ctn`。zip 从 11 个文件降到 10 个。

### 4. dmesg 成为调试主场（「查 bug 用 dmesg」）

- **内核模块**：每次写节点打一行 —— `ctn_patch: 名单更新 [A] [B]`（频率极低，
  一个游戏会话两次：写入 + 恢复）
- **daemon**：关键事件除了写 `daemon.log`，**同时写 `/dev/kmsg`**
- 于是**内核和 daemon 在同一条时间线上**，一条命令看全：

```bash
dmesg | grep -E 'ctn_patch:|游戏启动|不写节点|恢复原值'
```

**真机实测输出**（5 个场景只有 2 个真写了节点）：

```
ctnd 2.0 启动（节点 …）
ctn_patch: 名单更新 [JsonMain] [JsonRender]            ← 有 ctn → 写
游戏启动: aaa.empty.test → 云控里没有 ctn → **不写节点**
游戏启动: aaa.missing.test → 云控库里没有这个游戏 → **不写节点**
游戏启动: aaa.opaque.test → 加密形态 → **不写节点**
游戏退出（连续 6 次 -1），恢复原值 [UnityMain UnityGfxDevice]
ctn_patch: 名单更新 [UnityMain] [UnityGfxDevice]       ← 恢复
```

### 5. 内核模块因改动重新编译

`ctn_patch.c` 加了那行写日志，所以是**新编的 .ko**（哈希变了）。已核验：
`.gnu.linkonce.this_module` 段仍是 **1088 字节**（与设备模块一致，布局没变）、
vermagic 一致、kCFI 类型号一致（`read`=0xe866e2f4 / `write`=0x9a660ea0）。

**校验值**
```
ctn_patch.ko  sha256 afb3c7cbe2760153299ff4fb0aeaba658cda2cb57fa9a18839e0458e124b0848
ctnd          sha256 9dd8f386ad838b7d45e15f10386fe9804e4d56731a39bf432c83366bea7ad779
zip           sha256 402606b9a7788a4fc39d6351e23b077b72a3063c13f427e3e3e71d0445a4b851
```

> 注意：这是**第一个 ko 与上一个版本不同的版本**（v1.2~v1.9 的 ko 都是一个文件）。

## v1.9（versionCode 19）

**精简模块：兼容性不猜了，直接试加载；失败了看 dmesg。** 内核模块和 daemon 都没动。

**删掉的东西**：`expected_vendor.txt`、`elf_sec_size.sh`，以及整套
「厂商模块 sha256 + `struct module` 布局」预检（那是我为了跨机型安全加的，
但方向错了 —— 见下）。zip 文件数 13 → 11，脚本目录里少一个文件。

**换成什么**：安装器第 4 组改成**直接让内核加载它**：

| 情况 | 处理 |
|---|---|
| 模块未加载（全新安装） | `insmod` 试装 → 成功则 `rmmod`（只是试，重启后由 service.sh 正式加载）；失败则**抓 dmesg 里 ctn_patch 的行打出来**并拒绝安装 |
| 模块已在跑（升级） | 跳过试加载 —— 内核不允许同名模块加载两次；旧版能加载本身就说明本机内核接受我们的构建 |

**为什么这样对**：符号 CRC、`struct module` 布局、kCFI、vermagic —— 这些
**内核在 insmod 时自己全都会校验**，比任何用户态启发式都硬。而且校验不过时是
**干净拒绝加载**（不会崩机），原因就写在 dmesg 里。之前那套比哈希/比布局的预检
纯属多余，还把兼容机型误拒过（v1.4 的教训）。

**verify.sh 同步精简**：删掉「厂商模块」那一项（15 → 14 项），它的作用被下面本来
就有的 insmod 测试完全覆盖；insmod 失败时改成抓 dmesg 里 ctn_patch 的行（原来是
`dmesg | tail -12`，一大片无关的）。

**顺带修一个输出顺序 bug**：新写的 dmesg 打印原来用管道子 shell 直接 `ui_print`，
绕过了分组缓冲，那几行会跑到「`[n/9]`」标题**前面**去。改用 `info`（进缓冲）+
here-doc（不用管道）后顺序正确。

**真机验证**（一加 Ace 3 Pro）：
- 升级路径（模块在跑）→ 第 4 组显示「旧版正在运行…跳过试加载」✓
- 全新路径（模块已卸）→ 真的 insmod + rmmod，dmesg 里留下
  `已为 oplus_bsp_game_opt 补上 critical_task_name` 和 `已卸载` 两条（间隔 11ms）✓
- 失败路径：把 `module_layout` 的 CRC 改坏后装 → 第 4 组拦下，明细里直接打出
  `ctn_patch: disagrees about version of symbol module_layout` ✓
- 改完跑完整自检：14 项全过、`daemon 已拉起（pid …）`✓

**校验值**
```
ctn_patch.ko  sha256 be9a686a1901931a7cd82f76df00e1f268820ed459427f86e8ee90aef2cd37f1
ctnd          sha256 7a47dc6844cc37d5933b7fbff023b1ed608a4682af7c6ba2a5d19971dd94e0f1
zip           sha256 5cdc6d3586369b5790e768813241b9476adc55b95dd89e55278b7ef46d8cd6c3
```

> `ctn_patch.ko` 和 `ctnd` 的哈希与 v1.2~v1.8 相同（没动），只有脚本变了。

## v1.8（versionCode 18）

**接着修 v1.7 没修干净的地方：daemon 检测改成「看谁握着锁」。**
内核模块和 daemon 本体都没动。

**v1.7 修了一半**：把 `pgrep -x` 换成扫 `/proc/*/comm` 之后，在 App 的 action
通道（`ksud module action`）里仍然会偶尔误报「daemon 起不来」——3 次启动全撞锁，
但收尾时同一套扫描又能找到进程。说明**单靠进程名判断不可靠**：模块升级换过文件、
`comm` 与预期对不上、以及不同 pgrep/扫描实现的差异，都会让「按名字找」漏掉。

**改成看锁**：daemon 活着的**定义**就是它持着 `.ctnd.lock`。所以新增
`lock_holders()`（扫 `/proc/*/fd` 找谁打开了这个锁文件），检测变成两级：

| 函数 | 判据 |
|---|---|
| `ctnd_alive` | 进程名是 `ctnd` **或** 有人握着 `.ctnd.lock`（任一命中即算活着） |
| `ctnd_one` | 取 pid：先按进程名，取不到就用锁持有者的 pid |
| `ctnd_kill` | 名字匹配的和持锁的**一起**杀（原来只杀名字匹配的，漏掉就杀不干净） |

这个判据与进程名、与 `pgrep`/扫描的实现**都无关** —— 锁是文件系统层面的事实。
四个脚本（verify.sh / action.sh / service.sh / uninstall.sh）统一改用这套。

**真机验证**（一加 Ace 3 Pro）：走 App 的真实通道 `ksud module action` 连跑 3 次，
**每次都是 15 项全过**、每次都正确报 `daemon 已拉起（pid …）`
（修复前是 13 过 / 2 失败，报「daemon 0 个」但实际它一直活着）。

**校验值**
```
ctn_patch.ko  sha256 be9a686a1901931a7cd82f76df00e1f268820ed459427f86e8ee90aef2cd37f1
ctnd          sha256 7a47dc6844cc37d5933b7fbff023b1ed608a4682af7c6ba2a5d19971dd94e0f1
zip           sha256 c0712aeb599e2f7057cff60423ca3cd28010a5fedc67700019a63d1e46717c3a
```

> `ctn_patch.ko` 和 `ctnd` 的哈希与 v1.2~v1.7 相同（没动），只有脚本变了。

## v1.7（versionCode 17）

**修掉一个环境相关的假故障：daemon 明明活着，自检却报「0 个 / 起不来」。**
内核模块和 daemon 本体都没动。

**现象**（用户真机 log）：自检的「恢复运行状态」里 `ctnd` 启动成功（日志有
「ctnd 1.1 启动」），随后 3 次重试全部撞锁 —— **说明那个 ctnd 一直活着、锁一直
被它握着**；可自检和 `action.sh` 里的 `pgrep -x ctnd` 全都返回 0，`pkill -x ctnd`
也杀不掉它。最后报「daemon 0 个」，但它实际干了一下午活（后续的游戏写入记录都在）。

**根因**：`pgrep`/`pkill` 的 `-x` 语义**因实现而异**。系统 toybox 的 `-x` 按
**进程名（comm）**精确匹配（能匹配上）；而 KernelSU 的 action 环境里 PATH 可能
排到别的实现（这台设备上发现了 `/data/adb/turbo/backup/toolkit/pgrep`），那种
实现的 `-x` 匹配的是**整条命令行** —— ctnd 的 cmdline 是完整路径
`/data/adb/modules/ctn_patch/ctnd`，跟 `ctnd` 永远不相等 → **永远匹配不上**。
于是「探测不到、也杀不掉」，重试自然全撞锁。

**改法**：探测/击杀不再依赖 `pgrep`/`pkill`，自己扫 `/proc/*/comm`
（`ctnd_pids` / `ctnd_count` / `ctnd_kill` 三个小函数，与 pgrep 的实现无关）。
四个脚本（verify.sh / action.sh / service.sh / uninstall.sh）里的
`pgrep -x ctnd`、`pkill -x ctnd` 全部替换，共 19 处。

**真机验证**（一加 Ace 3 Pro）：`/proc` 扫描与 toybox `pgrep -x` 结果一致；
在 PATH 前置 ksu/bin 的模拟环境里两种写法都能工作（该环境恰好没有 pgrep
applet，所以无法在本机直接复现 app 环境的失败，但新写法不依赖任何 pgrep 实现，
从根上免疫）。

**校验值**
```
ctn_patch.ko  sha256 be9a686a1901931a7cd82f76df00e1f268820ed459427f86e8ee90aef2cd37f1
ctnd          sha256 7a47dc6844cc37d5933b7fbff023b1ed608a4682af7c6ba2a5d19971dd94e0f1
zip           sha256 b19f4680b0f22c89490abc972aca88f20fb8157bb4f49c8b445057cb08098d4a
```

> `ctn_patch.ko` 和 `ctnd` 的哈希与 v1.2~v1.6 相同（没动）。

## v1.6（versionCode 16）

**修掉自检里两个误报/自相矛盾的地方。** 内核模块和 daemon 都没动。

**问题一：自检的厂商模块检查还是旧的「比哈希」逻辑。** v1.5 只改了安装器
（`customize.sh`），漏改了自检脚本（`verify.sh`）。结果在另一台设备
（PKG110 / Ace5）上：**`insmod` 明明成功了**（模块在那台机器上能正常加载），
自检却报 `[!!] 厂商模块 与构建时不是同一份 → 结构体布局可能不匹配，会崩机`。
现在 `verify.sh` 用与 `customize.sh` **同一套判据**：哈希一致 → OK；哈希不同但
`.gnu.linkonce.this_module` 段大小相同 → OK（另一份构建、布局一致）；段大小
不同 → 失败；量不出 → 备注（以下面 `insmod` 的结果为准，能加载成功就是最硬的证据）。

**问题二：自检说「daemon 已拉起」，下一项又报「ctnd 没在运行」。** 两句自相矛盾，
因为「已拉起」是无条件打印的，根本没确认。现在：

- 起 daemon 会**确认**：最多试 3 次，每次等最多 5 秒；失败就清掉残兵重来；
  真起来了才打 `daemon 已拉起（pid 12345）`；
- 还是起不来 → 把**本次尝试期间新增的 daemon.log 行**当原因打出来
  （实测能直接看到「锁: 已有另一个 ctnd 在跑」）；
- **撞锁的措辞修正**：只有确实有 `ctnd` 活着才算「自愈、无后果」；没有 `ctnd`
  在跑时，那几条撞锁正是它起不来的原因 → 判失败（原来一律写「无后果」，误导）。

**问题三（顺带）**：`action.sh` 收尾时如果 daemon 没起来，会**重跑 `service.sh`**
把守护循环和 daemon 一起恢复（只起 daemon 不恢复守护循环的话，它挂了没人重启），
并把最终状态如实报出（模块数 / daemon pid / 节点值，起不来就打 daemon.log 尾部）。

**问题四（顺带）**：`verify.sh` 的 `MODDIR` 改用 `dirname "$0"`。原来用
`${0%/*}`，在「裸文件名调用」（`sh verify.sh`）时会算成文件名本身，导致后面所有
`$MODDIR/xxx` 路径全错。

**真机验证**（一加 Ace 3 Pro）：正常流程 14 项全过、`daemon 已拉起（pid …）`；
把 `ctnd` 换成必定失败的假程序后，正确打出「起不来（试了 3 次）」+ 三次撞锁的
新增日志，且 daemon 与 daemon 日志两项都判失败。

**校验值**
```
ctn_patch.ko  sha256 be9a686a1901931a7cd82f76df00e1f268820ed459427f86e8ee90aef2cd37f1
ctnd          sha256 7a47dc6844cc37d5933b7fbff023b1ed608a4682af7c6ba2a5d19971dd94e0f1
zip           sha256 791325feebd4ceccb94252b3f36d0ffa45643201dea82d24e352525c0860eec2
```

> `ctn_patch.ko` 和 `ctnd` 的哈希与 v1.2~v1.5 相同（没动），只有脚本和文档变了。

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
