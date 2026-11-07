# MyServer 当前版本与交接

## 工作目录与目标

- 仓库：`D:\Qt test\MyServer`；工程：`MyServer/MyServer.pro`。
- 参考工程：`D:\Qt test\QtLargeFileTransfer`，与 MyServer 是独立工程。
- 用户逐步手写复现并学习 C++ / Qt，保留现有学习注释，修改限于当前任务需要。
- Qt 5.14.2 / MinGW 64 位，C++17；当前主线只有可靠 TCP 长连接。

## 当前实现

协议为 v3、62 字节包头、单 SHA-256。一条控制连接协调 N 条数据连接，每条数据通道有对应发送、接收线程及独立通道文件。发送线程数与每通道窗口分别配置。

Types、Utils、Codec、Session、独立 Store、TcpServer、ReceiverWorker、TransferIo、SendTaskState、SendLane、SenderWorker、Manager、Controller 和界面收发入口均已接通。

当前合并方式只有串行：完成请求由接收 Worker 调用 Session，在该后台线程按通道顺序读取，使用一个 4 MiB 缓冲区写入并计算摘要，校验通过后提交文件、改名和清理。没有独立摘要线程、模式环境变量、合并计时参数或性能日志。网络超时计时器继续保留。

ReliableSha256 在 Windows 使用 CNG、Linux 使用 OpenSSL EVP，其余平台使用 Qt。发送前读文件计算摘要和接收合并时的摘要均走该封装；Codec 中短会话身份字符串的 Qt 哈希仍保留。

## 模块边界

- Codec：字节表示、拆包、JSON 和字段校验。
- Session：一个文件的接收状态、分片写入、持久化与恢复、串行合并和校验。
- Store：共享 Session、连接通道占用、续传目录归属和共享停止标记。
- Worker：所属接收线程中的 socket、缓存、消息分发和回复。
- Manager：监听、线程生命周期、进度汇总和通知转发。
- SenderWorker：后台协调线程；SendLane：各数据发送线程。
- Controller 和 MainWidget：主线程中的调度入口与界面更新。

## 当前证据与边界

`tests/` 保存当前回归源码；`build-release/validation-results.json` 汇总当前构建与测试结果。覆盖会话 18 项、摘要 42 组及状态检查、真实界面调用链的本机 TCP 普通文件和小文件收发。用户已成功传输约 6.64 GiB 文件。

两台真实电脑、全新外机及真实 Linux 编译运行尚未验收。取消发送已有后台接口与检查点，但界面没有取消按钮。逐片业务超时补传仍保留，不能把它解释为 TCP 自身需要额外重传。“原子保存状态失败：拒绝访问”曾出现，当前测试未复现，不能宣称已经解决。

## 实验归档和产物

流水线及 A/B 模式源码在 `experiments/merge-pipeline/source/`，测试源码和证据在同级目录。最新同工程实测为流水线 14,003.6 ms、串行 9,138.4 ms；流水线多耗时约 53.2%，差异主要在文件操作，具体根因尚未确认。用户决定主工程采用串行，并删除合并计时。

只保留当前 `build-release/` 和 `dist/MyServer-win64/`、`dist/MyServer-win64.zip`。历史辅助测试源码已压缩保留，旧的编译和打包目录已清理，已有源码备份保留在 backups 中。不要把实验归档当成当前生产代码。

## 构建与笔记

Qt 工具：`D:\Qt\5.14.2\mingw73_64\bin`；MinGW：`D:\Qt\Tools\mingw730_64\bin`。在 build-release 中运行 qmake 和 mingw32-make，使用 windeployqt 对已有 Release EXE 收集依赖。本机此套部署工具不加 --release / --debug，避免已有 MinGW 插件识别问题。

主学习笔记：`D:\Learning Documents\WorkFinding\cpp-python-review\07-QtWork\Network项目解析.md`。它按当前实现组织知识点，保留流水线实验结论，删除过期复现进度和过程路径；外部笔记修改前的副本保存在实验 evidence 目录。
