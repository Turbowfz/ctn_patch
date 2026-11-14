# 最新 Action 检测结果

本次使用最终开发包的临时解包副本，**没有安装新包、没有重启手机**。
当前设备 PJX110 / 17.0.1.201，内核 `6.1.157-android14-11-o-g00d81d97d759`。

## 已通过

- 开发包 74,385 字节，SHA256：
  `1ea1c9d3a908d555bab67b0935d82bff2e63d721916914f6e29975450197a82b`。
- 包内 Action、公共选择器、自检脚本与当前源码一致。
- 匹配 157 的 KO 静态验证记录及真实厂商文件身份一致。
- Action 默认选择 `exercise`，实际临时加载新 KO。
- 单名、双名写入读回和非法输入拒绝通过。
- 原名单恢复后，测试 KO 已卸载，新增节点已消失。
- Action 日志保存、路径提示和真实退出码均正确：`ACTION_RC=0`、`SELF_CHECK_EXIT=0`。
- 本次测试窗口中的内核日志未发现 BUG、WARNING、Oops、panic、KCFI/KASAN 异常。
- 历史 `ctn.conf` 哈希未变；新代码不读取该文件。

日志位置：

```text
/data/adb/ctn_patch/action.log
```

原始证据在 `01_手机提取/ctnp-action-exercise-20261114/`。

## 状态变化与实际安装情况

测试开始和包装脚本收尾时，原模块的 `disable` 标记存在，输出
`POSTCHECK disabled=yes module_loaded=no node_present=no debt_present=no stop_present=no`。

稍后的只读复核发现 `disable` 已不存在。本次测试脚本未删除该标记；变化来源未确定，
因此没有擅自重新创建或启用模块。后续读取得到：

- 手机上仍是旧版文件布局：根目录 `ctn_patch.ko` 34,800 字节与 `ctnd` 20,456 字节。
- 没有新版 `bin/ctnd`，也没有 `modules_update/ctn_patch`。
- 当前没有 CTN KO、daemon PID 或 CTN 节点在运行。
- `game_pid=-1 child_num=0`。

**所以这次证明最新版 Action 自检可用，不代表最新开发包已经在手机正式运行。**

## 未测试

- 141 本轮真机操作（只保留旧版历史可用记录和新 KO 静态验证）。
- 真实 HAL 注册线程样本；没有为了凑 PID 测试覆盖 `rt_info`。
- 正式安装、启用、重启后服务自动加载与真实游戏长时间效果。

没有改动版本号，没有提交、推送或更换远端 Release。
