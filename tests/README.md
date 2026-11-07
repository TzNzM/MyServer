# 当前版本回归源码

- `session_check.pro`：18 项会话检查，覆盖位图恢复与异常、通道划分、重复分片、取消与重试、合并摘要不匹配、最终清理和重复完成。
- `sha256_check.pro`：42 组标准摘要及分块一致性检查，并验证初始化、错误输入、完成缓存和重新初始化。
- `ui_transfer_check.pro`：真实 MainWidget → Controller → Manager → Worker → Session 本机 TCP 收发，检查普通文件、小文件、摘要、进度和资源回收，并确认没有合并性能计时日志。

在 `build-release/tests/` 下分别创建 session、sha256、ui 目录，进入后运行对应 `.pro` 的 qmake，再执行 mingw32-make。产物和日志只放在构建目录，源码配置通过相对路径引用当前 MyServer 工程。

界面测试需要一个新的数据目录参数；自动化运行可使用 QT_QPA_PLATFORM=offscreen。Qt 5.14 的 offscreen 消息框存在原生窗口兼容问题，测试过滤消息框 Show 事件并自动关闭对话框；此项不代表原生界面外观或外机行为已经验收。
