<!-- 由 gen_changelog.py 自动生成，请勿手改。要改内容请改 CHANGELOG.md 后重跑本脚本。 -->
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

> 注意：这是**第一个 ko 与上一个版本不同的版本**（v1.2~v1.9 的 ko 都是一个文件）。
