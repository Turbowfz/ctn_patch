<!-- 由 gen_changelog.py 自动生成，请勿手改。要改内容请改 CHANGELOG.md 后重跑本脚本。 -->
## v2.1（versionCode 21）

**daemon 学会盯云控了：游戏跑着的时候云控改了 ctn（或删了 ctn），daemon 会自动跟上，不用重启游戏。**
节点读出的 pid 也从写死的 -1 换成了 6.6（一加13）官方机制的**实时真值**。
顺手把常驻开销和体积再压了一轮。

### 1. 游戏运行中复查云控（本版主功能）

旧版只在一个游戏**启动**时查一次云控：云控后来把某个游戏的 ctn 改了/删了，
节点上还挂着旧配置，得杀游戏重开才生效。现在 daemon 每隔一段时间把当前游戏的
云控配置复查一遍：

- **ctn 变了** → 重新写节点（「db 里的 ctn 参数被修改也要应用」走的就是这条）；
- **ctn 被移除/清空** → 恢复成启动时的原值（云控不再要求加速这个游戏）；
- **ctb 变了**（`-e` 模式）→ ct_enable 跟着重写；
- **没变化** → 一行日志都不出，也一个字节都不写。

复查周期默认 30 秒，`ctnd -r <秒>` 可调（最小 5，0 = 关闭恢复旧行为）。

### 2. 复查不白查：stat 指纹两层缓存

复查要是每轮都拷一遍 600KB 云控库、开一次 SQLite，常驻开销就说不过去了。
现在查询带**两层判定**：

1. TTL（600 秒）兜底 —— 云控改动最迟几分钟内生效；
2. TTL 内比对库文件的 stat 指纹（mtime + size，含 -wal）—— **库没变 = 零拷贝**
   （不拷库、不开 SQLite），变了立刻重查。指纹只在成功查库后更新，
   查询失败下次照样重试。

### 3. 其他优化

- **kmsg fd 常开**：以前每条日志都要 open/close 一次 /dev/kmsg，现在启动时开一次；
- **ko 启动顺序自愈**：ctn_patch 比游戏内核（oplus_bsp_game_opt）先加载时
  （开机竞速/刷入脚本没等够），init 不再失败退出，而是挂 10 秒一次、最多 60 次的
  延迟重试，目标模块一出现就自动补上节点；
- **体积**：daemon 改用 -Os + 按函数裁剪 + strip，21.8KB → 19.9KB；
- **e2e 扩到 11 项**：新增「运行中改库自动重应用」「运行中删配置恢复原值」
  两个用例，正好对应第 1 节的两条主路径。

### 4. 升级说明

- 从 v2.0 直升，无需卸载；daemon 参数新增 `-r`，不改任何旧参数的行为；
- 复查靠 stat 指纹，日常零感知：游戏不启动、库没变化时，daemon 每轮只做
  一次 30ms 轮询 + 一次 stat，CPU 占用测不到。

### 5. pid 机制移植（6.6 一加13 官方源码，逐条对照）

**把 6.6 的 pid 机制完整移植过来：节点读出来的是真实 pid，不再是写死的 -1。**
另外确认了「云控只给一个线程名」这条路端到端能走通。

| 6.6 原文（`vendor/oplus/kernel/cpu/game_opt/`） | 本模块怎么补 |
|---|---|
| `critical_task_boost.c:78` `char critical_task[2][100]` | 6.1 是 `const char*[2]` 且在只读段 → 仍用 `vmap` 可写别名（不变） |
| `critical_task_boost.c:80` `pid_t critical_task_pids[2]` | **6.1 没有这个数组** → 改为读节点时**现算** |
| `critical_task_boost.c:602` 写入 `sscanf("%99s %99s")` | 一致；本模块额外接受单名（见第 6 点） |
| `critical_task_boost.c:631` 读取 `"%s:%d,%s:%d
"` | 一致，但 pid 换成现算的真值 |
| `rt_info.c:492` `check_task_name()`（名字须 < `TASK_COMM_LEN`） | 照搬 |
| `rt_info.c:528` `is_matching_thread()`（两槽不重 pid） | 照搬（含 `last_pid` 去重） |
| `rt_info.c:543` `find_critical_task_pid()` | 照搬匹配逻辑，数据源换成 **6.1 自己的 `related_threads[]`** |
| `rt_info.c:568` `update_critical_task_pids()`（先槽 1、再槽 0） | 解析顺序一致 |

**数据源怎么找的**：6.1 的 `rt_info.c` 里有 `struct render_related_thread related_threads[256]`
（`pid` + `task_struct*` + `wake_count`）和 `total_num` / `have_valid_render_pid` /
`rt_info_rwlock`，全是模块里的符号 —— 用 `模块名:符号名` 通过 kallsyms 拿到地址直接读。
读取时按厂商自己的做法**拿 `rt_info_rwlock` 的读锁**（他们改这个数组是持写锁的），
拿不到锁符号就退化为不加锁 —— 那也和厂商自己的热路径 `get_critical_task_state()`
一样，不会更差。

**行为实测**（真机，用 `fakethreads` 起真线程 + 走厂商接口登记）：

```
线程 RenderThread=28728 / GameThread=28729，写进 /proc/game_opt/rt_info
写 "RenderThread GameThread" → 读回 RenderThread:28728,GameThread:28729   ✓
写 "RenderThread"（单名）   → 读回 RenderThread:28728,RenderThread:28728  ✓
写 "NoSuchThreadXyz"        → 读回 NoSuchThreadXyz:-1,NoSuchThreadXyz:-1  ✓
```

**为什么要造 `fakethreads`**：内核匹配的是 `task->comm`，而 `fakepkg` 只改 `argv[0]`
（影响 `/proc/<pid>/cmdline`）—— 名字对不上，测不出这条链。`fakethreads` 用
`prctl(PR_SET_NAME)` 开真线程改 `comm`，并随模块一起打包，自检会用它做这项验证。

### 6. 单名写入端到端（db → 节点）

云控 `game_config` 的 `ctn` 只给一个名字时，现在从 db 到节点全程走通：

```
db 里 ctn="SoloThread"（单名）
  → ctnd 日志：已写入 [SoloThread]
  → 节点读回：SoloThread:6238,SoloThread:6238   ← 内核补的第二槽，pid 也是真的
  → dmesg：ctn_patch: 名单更新 [SoloThread:6238] [SoloThread:6238]
```

### 7. 自检加了「pid 机制」两项

`verify.sh` 现在会：起 `fakethreads` → 登记进 `rt_info` → 写节点 → 核对 pid 对不对，
再单独验单名两槽是否同名同 pid。**真机 15 项全过**。

### 8. insmod 失败抓 dmesg

这条 v1.9 起就在安装器里（`customize.sh` 第 4 组）：模块没加载时先试 `insmod`，
失败就把 `dmesg` 里 `ctn_patch` 的行打出来并拒绝安装。本轮复核确认仍在。

### 9. 代码复查后的三处修正

复查 `ctn_patch.c` / `ctnd.c` 全量代码，修掉三个问题（版本号仍是 v2.1，包内
二进制已更新）：

1. **查库失败不再被当成「云控删了配置」**（重要）。`lookup_cfg()` 改成返回
   「这次查询成功了吗」：拷库失败 / SQLite 打不开是**查不动**，不是结论。
   复查路径拿到「查不动」就保持现状，不再恢复原值 —— 早期版本会把一次临时
   IO 故障当成云控下掉了 ctn，把正在加速的节点恢复成默认名，30 秒后下个周期
   又写回来，来回跳。日志现在会明确写「复查: 查库失败，保持现状」。
2. **库指纹跟着缓存条目走**。原来用一个全局指纹，会出现「B 包缓存于 F0 →
   库变成 F1 → A 包查询把全局指纹刷成 F1 → B 包再启动时指纹又"相同"了 →
   拿到改库前的旧缓存」这种串味（最多滞后一个 TTL）。现在每个包记自己的指纹。
3. **`-r` 传非数字直接报错退出**（原来是 `atol` 成 0，等于**静默关掉复查**）。

顺带把内部版本号统一了：`ctn_patch.ko` 的 modinfo `version=2.1`、
`ctnd --version` 打 `ctnd 2.1`，与模块管理器显示的 v2.1 对齐（此前 ko 写 2.2、
daemon 写 2.3，是改名合并前的内部迭代号，排查时容易误导）。
