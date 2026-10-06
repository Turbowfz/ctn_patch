<!-- 由 gen_changelog.py 自动生成，请勿手改。要改内容请改 CHANGELOG.md 后重跑本脚本。 -->
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

> `ctn_patch.ko` 和 `ctnd` 的哈希与 v1.2~v1.6 相同（没动）。
