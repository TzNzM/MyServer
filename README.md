# MyServer

**v1.0.0 最终版**：本次 TCP 多通道文件传输复现的定稿，采用串行合并与平台加速 SHA-256。

[下载 Windows 运行包](https://github.com/TzNzM/MyServer/releases/tag/v1.0.0) · [学习笔记](docs/Network项目解析.md) · [流水线实验](experiments/merge-pipeline/README.md)

基于 Qt Widgets / Qt Network 的 TCP 长连接、多通道文件传输学习工程，使用 C++17、Qt 5.14.2 / MinGW 64 位。

一个任务由一条控制连接和 N 条数据连接组成，每条数据通道有对应的发送线程、接收线程和独立临时文件。线程数与每通道发送窗口分别配置；窗口表示允许同时未确认的业务分片数。

## 当前版本

- 协议 v3、62 字节固定包头，单 SHA-256，支持业务 ACK、有限补传、重连和断点续传。
- 接收端按通道顺序串行合并，复用一个 4 MiB 缓冲区，同批数据用于写入和增量摘要；摘要一致后提交、改名和清理。
- Windows 使用 CNG，Linux 使用 OpenSSL EVP，其余平台使用 Qt 后端。
- 主工程没有合并性能计时、流水线线程或模式环境变量；网络超时计时器仍用于发送与恢复。
- 界面已接通监听、停止监听、发送、日志和进度；尚未提供取消发送按钮。

当前回归覆盖 18 项会话检查、42 组摘要及状态检查，以及真实界面调用链的本机 TCP 普通文件和小文件传输。用户已完成约 6.64 GiB 文件的本机传输实验。两机、全新外机及真实 Linux 构建尚需验收；“状态保存拒绝访问”曾出现，根因尚未定位。

## 构建与运行

在 Qt Creator 打开 `MyServer/MyServer.pro`，选择匹配的 Qt / MinGW 套件；也可使用 PowerShell：

```powershell
$env:PATH = 'D:\Qt\5.14.2\mingw73_64\bin;D:\Qt\Tools\mingw730_64\bin;' + $env:PATH
New-Item -ItemType Directory -Path 'D:\Qt test\MyServer\build-release' -Force
Set-Location 'D:\Qt test\MyServer\build-release'
qmake '../MyServer/MyServer.pro' 'CONFIG+=release' 'CONFIG-=debug'
mingw32-make -j4
```

构建程序：`build-release/release/MyServer.exe`。
完整发布目录：`dist/MyServer-win64/`；压缩包：`dist/MyServer-win64.zip`。解压整个目录后运行，保留 DLL 与 platforms 等插件目录。

默认监听 `0.0.0.0:8899`，默认目标 `127.0.0.1:8899`；两机传输时将目标改为接收电脑的实际地址。

## 目录

- `MyServer/`：当前工程源码、用户学习注释、界面与资源。
- `tests/`：当前会话、摘要和真实界面调用链回归源码。
- `build-release/`：唯一保留的当前构建目录与验证证据。
- `dist/`：当前 Windows x64 运行目录与 ZIP。
- `experiments/merge-pipeline/`：流水线 A/B 实验源码、证据和结论，不参与主工程编译。
- `experiments/legacy-validation-sources.zip`：清理旧构建目录前保存的历史辅助测试源码。
- `backups/`：已有源码备份，继续保留。

流水线实验中，同一工程的最新观测为流水线 14.00 秒、串行 9.14 秒；当前选择串行，具体数据和原因分析见实验目录。Linux 构建要求见 `Linux构建说明.md`。

主分支面向可靠 TCP 长连接。早期 UDP 与短连接代码保存在既有归档分支 `archive/unfinished-udp-tcp-short`。
