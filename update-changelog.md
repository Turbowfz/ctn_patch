<!-- 由 gen_changelog.py 自动生成，请勿手改。要改内容请改 CHANGELOG.md 后重跑本脚本。 -->
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

> `ctn_patch.ko` 和 `ctnd` 的哈希与 v1.2~v1.8 相同（没动），只有脚本变了。
