# ctn_patch root 模块（Magisk / KernelSU 通用）

## 里面是什么

| 文件 | 干什么 |
|---|---|
| `module.prop` | 模块身份卡（Magisk 管理器里显示的名字/说明） |
| `customize.sh` | **刷入时的兼容性闸门**：7 组检查（内核版本 / .ko 与 vermagic / `oplus_bsp_game_opt` 是否模块 / 是否已存在同名节点 / 内核配置项 / 运行时符号 / 页大小与 SELinux）。不满足就 `abort` 拒绝安装，不用等重启才发现。详见主 README 第 8.7 节 |
| `service.sh` | 开机自动 `insmod`。先等 `oplus_bsp_game_opt` 出现（最多 90 秒）再加载，日志写 `boot.log` |
| `uninstall.sh` | 在管理器里停用/删除模块时 `rmmod ctn_patch` |
| `action.sh` | 管理器里的「操作/Action」按钮：重载一遍模块 + 跑完整验证，结果写 `action.log` |
| `verify.sh` | 11 步验证脚本（和工程根目录的同名文件是同一份，支持传 .ko 路径） |
| `ctn_patch.ko` | **需要你自己编译后放进 zip**（见下） |

## 怎么产出可刷入的 zip

```bash
# 1. 先按上级目录 README 的路线把 ctn_patch.ko 编出来
# 2. 把 .ko 放到本工程根目录（或任何位置，脚本第二个参数也行）
# 3. 打包：
python ../build_zip.py
#    或指定 ko 路径：
python ../build_zip.py /path/to/ctn_patch.ko
# 产出 ctn_patch.zip，里面 module.prop 在 zip 根目录（Magisk 要求）
```

脚本会把所有文本文件归一成 LF（Windows 下编辑过的 sh 脚本带 CRLF 到手机上会跑不起来，这里自动处理）。

## 怎么刷

- **Magisk**：Magisk App → 模块 → 从存储安装 → 选 `ctn_patch.zip`
- **KernelSU**：KSU App → 模块 → 安装 → 选 zip（同样支持 customize.sh / service.sh / action.sh）
- 装完**重启**才加载；开机日志在 `/data/adb/modules/ctn_patch/boot.log`

## 刷完怎么用

```bash
cat /proc/game_opt/task_boost/critical_task_name
# UnityMain:-1,UnityGfxDevice:-1   ← 默认值

echo "名字1 名字2" > /proc/game_opt/task_boost/critical_task_name
cat /proc/game_opt/task_boost/critical_task_name
# 名字1:-1,名字2:-1
```

## 常见问题

| 现象 | 原因 | 处理 |
|---|---|---|
| `boot.log` 里 `insmod 失败: disagrees about version of symbol xxx` | 内核开了 MODVERSIONS，`.ko` 的符号 CRC 和手机内核对不上 | 必须用和手机内核同一份源码/`Module.symvers` 重编（见上级 README 第五节） |
| `boot.log` 里 `insmod 失败: Invalid module format` | vermagic 差太多（比如 6.6 内核刷了 6.1 的 ko） | `customize.sh` 刷入时就会拦；能刷进去说明是后来换的内核，重编 |
| 刷入时被 `[不满足]` 拒绝 | 兼容性闸门拦住了，消息里会写明是哪一条 | 按消息对照主 README 第 8.2/8.6 节；多半是内核版本不对，需要按新内核重编 |
| insmod 失败且 dmesg 有 `SELinux` 字样 | 个别 ROM 收紧了 magisk 域的 `sys_module` 权限 | 先 `setenforce 0` 试一次确认；确认是 SELinux 后自己补 supolicy，别长期关 enforcing |
| 节点在但写入不生效 | `ct_enable` 没开，或名字超过 15 字符（`task->comm` 只有 16 字节，长名字永远匹配不到） | `echo 1 > /proc/game_opt/task_boost/ct_enable`；名字控制在 15 字符内 |

## 注意

- `.ko` 必须对应内核 `6.1.141-android14-11-o-*`（GKI 容忍 git hash 尾差，
  手机自带模块就是这么加载的；customize.sh 按这个口径放行/拦截）。
- 本设备 6.1 的 HAL 不读这个节点（它走 `/proc/sys/oplus_sched_ext/pid_unitymain`），
  这个节点改的是**内核侧** critical-task boost 的匹配名单。
