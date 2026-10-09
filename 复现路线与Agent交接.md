# MyServer：复现路线、验收清单与 Agent 交接

## 1. 项目背景和用户目标

用户正在准备 C++ / Qt 面试，逐步手写复现参考工程 QtLargeFileTransfer，希望理解每段代码及其设计理由。
目标项目是 MyServer，主线只聚焦可靠 TCP 长连接文件传输；UDP 和 TCP 短连接原型保留在归档分支。
当前任务是学习与复现，不是把参考工程整套代码一次复制过来，也不是已经完成的传输产品。

- 本地 Git 仓库根目录：`D:\Qt test\MyServer`
- 实际 qmake 工程：`D:\Qt test\MyServer\MyServer\MyServer.pro`
- 参考工程：`D:\Qt test\QtLargeFileTransfer\src`
- GitHub 公开仓库：https://github.com/TzNzM/MyServer
- Qt 版本：5.14.2，MinGW 64 位；C++17。
- 本文是交接基线；后续 Agent 必须以最新源码和 Git 状态为准。

## 2. 当前基线（2026-10-09）

已完成：

- [x] 初始化 Git，沿用远程最初的提交历史。
- [x] 将清理前完整源码、学习注释保存为归档提交 `83f8441`。
- [x] 主线移除旧短连接通道和旧协议文件，清理 UDP、块大小及模式切换 UI。
- [x] 保留 MainWidget、NoWheelSpinBox、QSS 和资源文件。
- [x] 添加 `transferdefaults.h`，只保存发送窗口和分片大小默认值。
- [x] 主线清理提交 `703c537` 已通过 Qt 5.14.2 / MinGW 编译并上传。
- [x] 本地归档分支改名为 `archive/unfinished-udp-tcp-short`。
- [x] README 分支名更新提交为 `e28073e`。
- [x] 远程归档分支改名完成：新旧名称的提交核对一致后删除旧名称。
- [x] 本地归档 upstream 已更新为 `origin/archive/unfinished-udp-tcp-short`。
- [ ] 每次更新文档或代码后推送 main 并核对远程提交（持续维护项）。
- [ ] 可靠 TCP 协议、网络监听、文件发送、ACK、重传、续传和校验尚未实现。

MainWidget 的发送/监听按钮目前只输出“尚待复现”的日志，不执行网络传输。
“发送窗口”表示允许同时未确认的分片数量，不是并发线程数量。
当前默认监听 `0.0.0.0:8899`，目标 `127.0.0.1:8899`，分片为 60 KB。

写本文时工作区中另有 4 个用户未跟踪文件：

```text
MyServer/abstracttransferchannel.cpp
MyServer/abstracttransferchannel.h
MyServer/tcptransferchannel.cpp
MyServer/tcptransferchannel.h
```

这些文件属于用户当前工作，不在主线 qmake SOURCES / HEADERS 中。不要自动删除或通过 `git add .` 把它们带进长连接主线；处理前先明确用户意图。

## 3. 参考工程实际采用的网络方式

正式 TCP 调用链：

```text
MainWidget
  → TransferController（仅保留可靠 TCP 调度）
  → ReliableTransferManager
      → 接收：QTcpServer + 各连接的 QTcpSocket / 接收缓存 / 会话状态
      → 发送：QThread + ReliableSenderWorker::ProcessSend()
                    → 一个 QTcpSocket 贯穿本次传输
```

参考发送端在后台线程中运行 `ProcessSend()`，函数内创建局部 `QTcpSocket`。
该 socket 在整个传输循环期间持续存在，使用 `waitForConnected()`、`waitForBytesWritten()`、
`waitForReadyRead()` 进行阻塞式等待；断线时可以重新连接并恢复会话。
参考接收端通过 `newConnection`、`readyRead` 和 `disconnected` 信号处理连接。

因此，“socket 是局部变量”不等于“短连接”，应看它贯穿一次 SendPacket 还是贯穿整个文件发送过程。
“长连接/短连接”和“阻塞/异步”是两个独立维度。当前复现可先遵循参考的后台阻塞发送方式，
不必先改成全异步状态机；阻塞发送与哈希计算不能放在 UI 主线程。

`write()` / `bytesWritten` 只反映本地发送进度。业务 ACK 才能表示接收端已经接受某个分片，
最终成功须以接收端完整性校验后返回的 Result 为准。

参考可靠协议与旧协议不同：

- 固定包头为 80 字节，版本 1，魔数 `0x52544650`。
- 消息类型：Metadata(101)、ResumeState(102)、Data(103)、Ack(104)、CompleteRequest(105)、Result(106)、Error(107)。
- 用分片序号和文件偏移定位数据，不沿用旧协议的 Chunk/Segment 两级切分。
- UUID 固定编码为 RFC 4122 的 16 字节；包头字段须显式序列化，不用 sizeof(struct) 作为网络格式。
- 应用层 ACK / 续传用于追踪业务分片和恢复状态；TCP 本身已经提供有序可靠字节流，不能把 TCP 简单描述成需要应用层补救的“丢包协议”。

## 4. 分阶段复现顺序与验收清单

每阶段仅实现当前所需部分，编译、验证后提交；不要一次复制完整的大类。
Manager 与 Worker 互有依赖，可先拆出最小骨架和接口，再逐步补齐，不要求按文件整份完成。

### 阶段 1：ReliableTransferTypes（下一步）

参考：`ReliableTransferTypes.h`。

- [ ] 定义可靠协议常量、消息枚举和固定包头。
- [ ] 定义文件元信息：传输 ID、会话键、文件名、大小、分片大小、总分片数、哈希。
- [ ] 定义缺失范围、未确认分片记录。
- [ ] 能解释每个字段的单位、有效范围，以及 transferId 与持久会话身份的区别。
- [ ] 确认缺失范围端点是否包含边界，避免发送/接收端产生 off-by-one。

验收：头文件可编译；能手算包头编码长度和最后一个分片大小。

### 阶段 2：ReliablePacketCodec

参考：`ReliablePacketCodec.h/.cpp`。

- [ ] 实现包头编解码，统一字节序。
- [ ] 实现 Metadata、ResumeState、Data、ACK、CompleteRequest、Result、Error 的封包与解析。
- [ ] 实现接收缓冲拆包：不足一个包保留，多个完整包循环解析。
- [ ] 校验 magic/version/type/header length、载荷上限、序号和偏移。
- [ ] 大整数 JSON 编码避免精度丢失；解码必须检查转换是否成功。

验收：测试完整包往返、拆成多次输入的半包、多个粘连包，以及错误长度/魔数。
这些测试先不依赖 socket，便于定位协议问题。

### 阶段 3：接收端最小骨架

参考：`ReliableTransferManager.h/.cpp` 的监听和接收部分。

- [ ] 创建 QTcpServer，接入新连接。
- [ ] 每个 QTcpSocket 独立维护接收缓存，正确处理断开与对象释放。
- [ ] 收到 Metadata 后创建接收会话，并返回 ResumeState。
- [ ] 初次传输时返回全部分片缺失的范围。
- [ ] 收到 Data 后按偏移写入 .part 文件，正确处理重复分片。
- [ ] CompleteRequest 后检查所有分片是否完整，缺失时不能报告成功。

验收：接收端能监听；非法包不造成错误写入；日志能对应会话和分片。

### 阶段 4：发送端最小长连接

参考：`ReliableSenderWorker.h/.cpp`，以及 Manager 的 StartSend / QThread 生命周期。

- [ ] 在工作线程创建并使用 socket，不跨线程直接操作同一个 socket。
- [ ] 同一文件会话连接一次，完成 Metadata → ResumeState → 多个 Data → CompleteRequest → Result。
- [ ] 逐段读取文件，内存不随整个文件大小增长。
- [ ] 明确文件、socket、线程在正常结束、错误、窗口关闭时的清理流程。
- [ ] 第一版允许先用串行发送；明确哪些可靠性机制尚未接入。

验收：本机 1 MB 文件能完整传输；正常传输期间只接受一条连接；外部 SHA256 比较一致。
第一次联调双方都使用 MyServer 的同一协议版本，阶段性简化不保证与参考成品互通。

### 阶段 5：ACK，再加入发送窗口

- [ ] 窗口先设为 1：收到 ACK 才推进下一分片。
- [ ] ACK 在分片校验、写入成功后发出；解释写入成功与持久化到磁盘的区别。
- [ ] 加入未确认分片表，记录序号、最近发送时间、重试次数。
- [ ] 窗口提升到 N；在途未确认分片数始终不超过 N。
- [ ] 重复 ACK 不重复计进度，非法 ACK 不改变状态。

验收：使用多个窗口值传输；进度按已确认分片统计，CompleteRequest 不早于全部确认。

### 阶段 6：超时补传与断线恢复

- [ ] 超时只补发未确认分片，设置重试/重连次数上限。
- [ ] 超时时间使用单调计时来源，避免系统时间调整影响。
- [ ] 重连时清理旧字节缓存，重新握手，按接收端真实缺失范围恢复。
- [ ] Result 等待也有明确超时/退出条件，不无限卡住。

验收：在测试接收端故意延迟/忽略 ACK；传输中断连接；确认补传、恢复或可解释失败。
TCP 正常情况下不会向应用交付“乱序分片”；可用人工重复/异常协议包测试业务健壮性。

### 阶段 7：跨进程断点续传与最终校验

- [ ] 持久化接收位图、文件信息和 state.json，考虑状态文件的原子更新。
- [ ] 恢复时验证元信息匹配，不能仅凭文件名续传到另一个文件。
- [ ] 位图与 .part 内容保持一致，明确崩溃后恢复策略。
- [ ] 校验完整大小和 MD5 / SHA256；成功后重命名为正式文件，再回 Result。
- [ ] 文件名与路径校验，禁止写出用户指定的输出目录；处理已有文件重名。
- [ ] 无效状态文件或哈希不一致不能显示“传输成功”。

验收：接收进程重启后恢复；截断/损坏 .part 后能识别问题；最终文件哈希一致。
哈希用于完整性比对，不意味着具备加密或身份认证。

### 阶段 8：TransferController 与 MainWidget 接入

- [ ] Controller 仅转发可靠 TCP 的监听、发送、日志、进度与结果。
- [ ] 监听成功后才更新按钮；启动失败时保留正确状态。
- [ ] 发送成功/失败后恢复按钮，任务中避免重复提交。
- [ ] UI 不阻塞，发送窗口与分片单位转换有效，避免整数溢出。
- [ ] 明确空文件、多文件、取消操作是否支持，README 与实际能力一致。

验收：文件不存在、连接拒绝、端口占用、断线、写盘失败均能给出明确反馈。

### 阶段 9：面试展示与回归

- [ ] 本机与局域网传输；大文件、边界分片、中文文件名、重名文件。
- [ ] 保存测试记录：文件大小、窗口、分片大小、耗时、SHA256、故障恢复结果。
- [ ] 能说明 TCP 字节流拆包、线程归属、ACK 的业务意义与续传身份。
- [ ] README 只列已验证的功能，不把参考工程的功能当作 MyServer 已完成成果。
- [ ] 不把其他 Agent 的推测直接当结论；涉及实现方式须阅读实际源码。

## 5. GitHub 分支结构和当前同步状态

计划稳定结构：

```text
origin (https://github.com/TzNzM/MyServer.git)
├─ main
│  └─ 可靠 TCP 长连接复现主线，按阶段更新
└─ archive/unfinished-udp-tcp-short
   └─ 旧学习快照：UDP / TCP 短连接部分未完成，保留原始注释
```

远程改名已完成，`archive/unfinished-udp-tcp-short` 指向归档提交 `83f8441`，
旧远程分支 `archive/current-scaffold` 已删除，本地 upstream 已同步。
README 的分支名称更新和本交接文档在 main 中维护，后续用下列命令核对实时状态。

先检查实时状态：

```powershell
Set-Location 'D:\Qt test\MyServer'
git status --short
git branch -vv
git remote -v
git ls-remote --heads origin
```

本次远程改名已完成。以下保留改名流程供以后参考，无需再次执行已完成的步骤：

```powershell
git push -u origin archive/unfinished-udp-tcp-short
git ls-remote --heads origin archive/current-scaffold archive/unfinished-udp-tcp-short
```

确认命令成功且两个远程分支指向同一提交（初始归档为
`83f8441be7fc6a545a12eaf986a689cad4cda32d`），再执行：

```powershell
git push origin --delete archive/current-scaffold
git push -u origin main
git fetch --prune origin
git branch -vv
```

若新分支推送失败、旧分支已经有新增提交或两者不一致，先保留旧分支并核对差异。
已删除的只是旧分支名称，归档源码仍在新分支和本地提交中。

## 6. 日常提交和上传更新

在 main 学习并实现一个小阶段后，先构建、检查差异，再只暂存本次需要的文件：

```powershell
Set-Location 'D:\Qt test\MyServer'
git status --short
git branch --show-current
git diff
# 示例：完成协议定义后
git add -- MyServer/reliabletransfertypes.h MyServer/MyServer.pro
git diff --cached
git commit -m "feat: define reliable TCP transfer protocol"
git push origin main
```

上传本交接文档时：

```powershell
git add -- 复现路线与Agent交接.md
git commit -m "docs: add reproduction roadmap and agent handoff"
git push origin main
```

建议提交主题：协议编解码、接收会话、长连接发送、ACK 窗口、超时恢复、状态持久化、UI 接入。
每次增加 .h/.cpp 后同步更新 .pro；每次更新功能后同步修改本文复现清单与 README。

查看归档代码无需切换主工作区：

```powershell
git show archive/unfinished-udp-tcp-short:MyServer/tcptransferchannel.cpp
```

切分支前先检查未提交和未跟踪文件；旧分支有同名文件时，切换可能被阻止。不要强制覆盖。
不执行 `git reset --hard`、`git clean -fd` 或强制推送来解决正常同步问题。

GitHub 登录由用户在自己的终端/浏览器完成，不在聊天中索取密码或 Token。
HTTPS 认证优先通过凭据管理器；若终端要求 Password，不使用 GitHub 账户密码，应按 GitHub 的认证方式处理。

## 7. 构建与验证

在 Qt Creator 打开 `MyServer/MyServer.pro`，选择 Qt 5.14.2 MinGW 64 位。
命令行可用：

```powershell
Set-Location 'D:\Qt test\MyServer'
$env:PATH = 'D:\Qt\Tools\mingw730_64\bin;D:\Qt\5.14.2\mingw73_64\bin;' + $env:PATH
New-Item -ItemType Directory -Path build-tcp-main -Force | Out-Null
Set-Location build-tcp-main
qmake ../MyServer/MyServer.pro "CONFIG+=debug"
# qmake 成功后再执行
mingw32-make -j4
```

`build-tcp-main`、旧构建目录、.exe、.o、`*.pro.user` 已由 .gitignore 排除。
编译通过只证明构建成立，不证明 ACK / 续传等行为成立；按各阶段验收单独验证。

## 8. 给后续 Agent 的工作要求

1. 先阅读本文、README、最新源码、Git 状态及适用 AGENTS.md，再判断当前进度。
2. 用户偏好逐步手写、逐段解释。只回答问题时不要改文件；要求落盘时遵守指定修改范围。
3. 未经请求不要一次实现剩余全部模块。先说明下一小阶段及其验收目标，再按请求推进。
4. 主线保持可靠 TCP 范围；用户新写的短连接文件先保留，不自动混入构建或提交。
5. 不覆盖用户现有修改，不删除构建目录或未跟踪文件，不把源码快照当可运行产品。
6. 检查实际代码后区分：长连接生命周期、后台阻塞发送、接收端事件驱动。
7. 正常发送结束、异常、重连、程序退出都要核对线程与对象生命周期。
8. 不宣称功能完成、编译通过或上传成功，除非实际执行并取得结果。
9. 当前执行环境曾出现隔离进程启动失败；允许使用产品提供的审批机制重试，不能假称命令已经运行。
10. 更新本文的进度和同步状态，保留可供用户面试复述的设计理由及测试证据。
