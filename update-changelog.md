<!-- 由 gen_changelog.py 自动生成，请勿手改。要改内容请改 CHANGELOG.md 后重跑本脚本。 -->
## v2.1（versionCode 21）

**把 6.6（一加13）的 pid 机制完整移植过来：节点读出来的是真实 pid，不再是写死的 -1。**
另外确认了「云控只给一个线程名」这条路端到端能走通。

### 1. 移植来源（一加13 官方源码，逐条对照）

| 6.6 原文（`vendor/oplus/kernel/cpu/game_opt/`） | 本模块怎么补 |
|---|---|
| `critical_task_boost.c:78` `char critical_task[2][100]` | 6.1 是 `const char*[2]` 且在只读段 → 仍用 `vmap` 可写别名（不变） |
| `critical_task_boost.c:80` `pid_t critical_task_pids[2]` | **6.1 没有这个数组** → 改为读节点时**现算** |
| `critical_task_boost.c:602` 写入 `sscanf("%99s %99s")` | 一致；本模块额外接受单名（见第 3 点） |
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

### 2. 单名写入端到端（db → 节点）

云控 `game_config` 的 `ctn` 只给一个名字时，现在从 db 到节点全程走通：

```
db 里 ctn="SoloThread"（单名）
  → ctnd 日志：已写入 [SoloThread]
  → 节点读回：SoloThread:6238,SoloThread:6238   ← 内核补的第二槽，pid 也是真的
  → dmesg：ctn_patch: 名单更新 [SoloThread:6238] [SoloThread:6238]
```

### 3. 自检加了「pid 机制」两项

`verify.sh` 现在会：起 `fakethreads` → 登记进 `rt_info` → 写节点 → 核对 pid 对不对，
再单独验单名两槽是否同名同 pid。**真机 15 项全过**。

### 4. insmod 失败抓 dmesg

这条 v1.9 起就在安装器里（`customize.sh` 第 4 组）：模块没加载时先试 `insmod`，
失败就把 `dmesg` 里 `ctn_patch` 的行打出来并拒绝安装。本轮复核确认仍在。
