<!-- 由 gen_changelog.py 自动生成，请勿手改。要改内容请改 CHANGELOG.md 后重跑本脚本。 -->
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

> `ctn_patch.ko` 和 `ctnd` 的哈希与 v1.2~v1.7 相同（没动），只有脚本变了。
