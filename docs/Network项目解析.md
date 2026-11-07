# 项目范围与当前版本

> 当前主工程为 MyServer：TCP 长连接、多通道收发、单 SHA-256、串行合并。合并性能计时和串行/流水线切换已从生产代码移除，流水线源码与实验资料独立保存在 `D:/Qt test/MyServer/experiments/merge-pipeline/`。

一个文件任务使用一条控制连接和 N 条数据连接，每条数据通道对应一个发送线程、一个接收线程和独立临时文件。发送窗口限制每条数据连接允许同时未确认的分片数，与线程数量分别配置。

```text
MainWidget → TransferController → ReliableTransferManager
                                  ├─ 接收：TcpServer → ReceiverWorker → Store → Session
                                  └─ 发送：Sender 协调线程 → N 个 SendLane 数据线程
```

## 当前实现与验证边界

| 部分 | 当前状态 |
| --- | --- |
| 协议 | v3、62 字节固定包头、七类消息、统一拆包与字段校验；只使用 SHA-256 |
| 接收 | 多连接调度、共享会话、分片写入、批量保存位图、恢复、串行合并与校验 |
| 合并 | 在处理完成请求的接收 Worker 线程中，按通道顺序读取；复用一个 4 MiB 缓冲区，写入同批数据并增量计算 SHA-256，校验通过后 commit、改名和清理 |
| 发送 | 后台协调线程、每通道窗口与业务 ACK、有限补传和重连、最终 Result 确认 |
| 摘要 | Windows CNG、Linux OpenSSL EVP，其余平台 Qt；协议中的算法仍是标准 SHA-256 |
| 界面 | 监听、停止监听、发送、线程数、窗口、分片与目录配置，以及日志和结果显示；尚无取消发送按钮 |
| 当前回归 | 会话 18 项、摘要 42 组及状态检查；真实界面调用链的本机 TCP 普通文件和小文件收发、最终文件摘要与资源回收检查 |
| 构建部署 | Windows x64 Release；运行包使用 windeployqt 收集依赖，隔离开发环境路径检查启动 |
| 后续验收 | 两台真实电脑、全新外机、真实 Linux 构建，以及多任务和异常恢复的系统性验收；“状态保存拒绝访问”曾出现，根因尚未定位 |

用户已在本机完成约 6.64 GiB 文件收发并取得合并日志；这一结果不代替两机或 Linux 验收。自动化界面测试在 offscreen 平台中过滤 QMessageBox 的原生 Show 处理，不代表原生对话框外观已经验收。

当前仅保留 `build-release/` 构建目录和 `dist/MyServer-win64/` 发布目录及其 ZIP。测试源码在 `tests/`，当前验证证据在 `build-release/validation-results.json`。旧构建与发布产物已清理，历史辅助测试源码另存为 `experiments/legacy-validation-sources.zip`。

# 各个文件的作用（长连接 TCP 部分）

| 文件 | 当前职责 |
| --- | --- |
| `ReliableTransferTypes.h` | 协议常量、消息类型、包头、元信息、缺失范围和在途分片记录 |
| `ReliableTransferUtils.h` | 全局通道范围、分片偏移及长度、总分片数计算，供发送与接收共用 |
| `ReliablePacketCodec.h/.cpp` | 包头与 JSON 编解码、半包/粘包拆分、字段校验及 SessionKey 生成 |
| `ReliableReceiveSession.h/.cpp` | 一个文件的接收状态、写入、持久化与恢复；串行合并、SHA-256 校验和最终文件生成 |
| `ReliableReceiveSessionStore.h/.cpp` | 查找或创建 Session、登记通道占用、管理续传目录归属和共享停止标记 |
| `ReliableTcpServer.h/.cpp` | 接受连接，转交描述符，让接收 Worker 在所属线程创建 socket |
| `ReliableReceiverWorker.h/.cpp` | socket 与缓存、拆包、三类接收请求、调用 Session、回复 ACK 和 Result |
| `ReliableTransferManager.h/.cpp` | 启停监听与发送，管理线程生命周期，过滤旧轮次通知并转发日志、进度和结果 |
| `ReliableSenderWorker.h/.cpp` | 后台准备元信息，协调控制握手、数据线程、进度、最终结果及有限恢复 |
| `ReliableSendLane.h/.cpp` | 单条数据连接的文件句柄、序号区间、窗口、ACK、补传和重连 |
| `ReliableSendTaskState.h` | 发送线程共享的确认位图、计数、首个错误、日志和取消标记 |
| `ReliableTransferIo.h/.cpp` | 后台发送线程共用的写包、读包及连接握手工具 |
| `ReliableSha256.h/.cpp` | Initialize / AddData / Finish 跨平台摘要接口，管理原生摘要资源和错误 |
| `TransferController.h/.cpp` | 对界面提供启停和发送接口，转发 Manager 通知，自身不创建额外线程 |
| `mainwidget.h/.cpp` | 配置与按钮响应，显示日志、进度和结果；后台线程不直接操作控件 |

Codec 管字节格式，Session 管文件接收状态，Store 管连接共享的会话，Manager 管线程和生命周期。流水线实验类不属于当前主工程。

---

# `ReliableTransferTypes.h`

> 当前类型定义对应协议 v3、62 字节固定包头和单 SHA-256，发送端与接收端均使用这些定义。

它主要包含四个结构体：

| 结构体 | 描述什么 |
| --- | --- |
| `SReliablePacketHeader` | 当前这条协议消息的信息 |
| `SReliablePacketRange` | 一段连续缺失的分片序号 |
| `SReliableFileMetadata` | 整个待传文件的信息 |
| `SReliablePendingPacket` | 已发送、还没收到 ACK 的某个分片 |

## 全局协议参数

这里说的“包”，是应用自己定义的协议消息。TCP 提供字节流，一次 `readyRead` 可能只收到半个协议包，也可能收到多个。

| 常量 | 值 | 含义 |
| --- | ---: | --- |
| `g_nReliablePacketMagic` | `0x52544650` | 协议识别标记，收到包头后检查它，判断是否符合本协议 |
| `g_nReliablePacketVersion` | `3` | 协议版本，供双方判断协议格式是否兼容；v3 不兼容旧版 |
| `g_nReliablePacketHeaderLength` | `62` | 显式编码后的固定包头长度，单位是字节，不是 `sizeof(struct)` |
| `g_nReliableDefaultWindowSize` | `16` | 每条数据连接默认允许同时存在的未确认分片数，不是线程数 |
| `g_nReliableDefaultPacketTimeoutMs` | `1500` | 分片等待 ACK 的超时阈值，单位是毫秒（这里的ACK是我们应用层的ACK） |
| `g_nReliableDefaultMaxRetryCount` | `8` | 单个分片最多补传 8 次，不包含初次发送 |
| `g_nReliableDefaultReconnectCount` | `3` | 断线后的重连尝试次数上限 |
| `g_nReliableControlWaitTimeoutMs` | `3000` | 连接、写入等待、握手等操作使用的等待超时 |
| `g_nReliableAckPollIntervalMs` | `40` | 发送循环每次等待可读数据的时间，随后继续检查 ACK、超时等状态 |
| `g_nReliableDefaultSendThreadCount` | `4` | 默认数据发送线程数，接收数据线程与其一一对应；不包含额外的协调/控制线程 |
| `g_nReliableMaxThreadCount` | `32` | 每个任务的数据通道数上限，即最多 32 对发送/接收数据线程 |
| `g_nReliableMaxWindowSize` | `128` | 每条数据连接的发送窗口上限，单位为未确认分片数 |
| `g_nReliableMaxSegmentSize` | `4 * 1024 * 1024` | 单个文件分片最大 4 MiB，单位为字节 |
| `g_nReliableMaxPayloadSize` | `16 * 1024 * 1024` | 协议载荷总上限为 16 MiB；Data 还要满足更严格的分片大小上限 |
| `g_nReliableResultWaitTimeoutMs` | `300000` | 等待接收端合并、校验并返回 Result 的上限，单位毫秒，即 5 分钟 |
| `g_nReliableReceiverCapacityMultiplier` | `2` | 接收线程容量倍数，可按资源情况调整；不是严格的并发文件数限制 |
| `g_nReliableMaxReceiverThreadCount` | `66` | `容量倍数 × (最大数据通道数 + 1)`，限制整个接收端的连接线程总数 |

`g_nReliableAckPollIntervalMs` 是发送端轮询等待的间隔，不是接收端发送 ACK 的间隔。例如实际 4 条数据连接、每连接窗口 16，业务上最多允许 64 个未确认分片；这不表示有 64 个线程。

## `EReliableMessageType`：这条消息要做什么

| 枚举项 | 值 | 方向和作用 |
| --- | ---: | --- |
| `MetadataPacket` | 101 | 发送端 → 接收端：介绍待传文件及本连接的通道身份，发起握手 |
| `ResumeStatePacket` | 102 | 接收端 → 发送端：告知已有分片和缺失范围 |
| `DataPacket` | 103 | 发送端 → 接收端：携带一个文件分片 |
| `AckPacket` | 104 | 接收端 → 发送端：确认某个分片已接受 |
| `CompleteRequestPacket` | 105 | 发送端控制连接 → 接收端：请求检查是否完整、顺序合并各临时文件并执行 SHA-256 校验 |
| `ResultPacket` | 106 | 接收端 → 发送端：返回最终结果或仍缺失的范围 |
| `ErrorPacket` | 107 | 反馈错误原因 |

## `SReliablePacketHeader`：每条消息都带的固定包头

| 字段 | 类型 / 编码字节数 | 含义 |
| --- | --- | --- |
| `m_nMagic` | `quint32` / 4 | 协议魔数 |
| `m_nVersion` | `quint16` / 2 | 协议版本 |
| `m_nMessageType` | `quint16` / 2 | 消息类型，如 Data、ACK |
| `m_nHeaderLength` | `quint16` / 2 | 包头长度，当前固定为 62 字节 |
| `m_oTransferId` | `QUuid` / 16 | 标识消息所属的文件传输任务；控制连接和所有数据通道共用同一个 UUID |
| `m_nPacketSequence` | `quint64` / 8 | 文件分片序号；在 ACK 中表示确认的分片序号 |
| `m_nPayloadOffset` | `quint64` / 8 | 当前分片内容在整个源文件中的起始位置，单位是字节；写通道临时文件时还需换算成局部偏移 |
| `m_nPayloadSize` | `quint32` / 4 | 当前消息的载荷长度，单位是字节，不包含包头 |
| `m_nTotalPacketCount` | `quint32` / 4 | 整个文件的数据分片总数 |
| `m_nFileSize` | `quint64` / 8 | 整个文件的大小，单位是字节 |
| `m_nWindowSize` | `quint32` / 4 | 发送端声明的每条数据连接的业务发送窗口，单位为分片数 |

### 包头长度核对

```text
4 + 2 + 2 + 2 + 16 + 8 + 8 + 4 + 4 + 8 + 4 = 62 字节
```

这 62 字节按表中顺序显式序列化。UUID 以 RFC 4122 的 16 个原始字节编码，结构体在内存中的对齐和大小不决定网络包头长度。包头不包含 flags 或预留字段，修改协议时必须同步调整写入、读取和载荷起点。

## `SReliablePacketRange` 连续缺失的分片范围

| 字段 | 含义 |
| --- | --- |
| `m_nStartSequence` | 缺失范围的第一个分片序号 |
| `m_nEndSequence` | 缺失范围的最后一个分片序号 |

范围是闭区间 `[start, end]`，两端都包含。例如 `[3, 5]` 表示缺失分片 3、4、5；默认 `[0, 0]` 表示分片 0，不是空范围。它与数据通道分配采用的左闭右开区间 `[first, end)` 要区分。

## `SReliableFileMetadata`：整个文件的信息

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `m_oTransferId` | `QUuid` | 文件传输任务的 ID，与包头对应；新任务重新生成，同一任务内重连保持不变 |
| `m_strSessionKey` | `QString` | 用于定位持久化续传状态的会话键 |
| `m_strFileName` | `QString` | 文件名，例如 `video.mp4`，不包含发送端目录 |
| `m_nFileSize` | `qint64` | 文件总大小，单位是字节 |
| `m_nSegmentSize` | `int` | 标准分片大小，单位是字节 |
| `m_nTotalPacketCount` | `quint32` | 文件总分片数 |
| `m_nLaneCount` | `int` | 实际数据通道数；每个通道对应一个发送数据线程、一个接收数据线程和一个独立临时文件 |
| `m_strSha256` | `QString` | 整个文件的 SHA-256，用 64 个小写十六进制字符保存，用于完整性比较 |

文件内容摘要统一保留 SHA-256，`m_strMd5` 已从类型中删除。两种算法都能用于完整性比较，但算法和强度并不相同；这里选择 SHA-256 简化流程。其他模块中的旧 MD5 代码仍需后续迁移。

### `m_strSessionKey` 具体解释

新版参考工程的发送端把下面的信息拼起来：

```text
v3 | 文件名 | 文件大小 | 分片大小 | 实际数据通道数 | 文件SHA256
```

然后对 UTF-8 字节计算 SHA-256，转换成十六进制字符串。

因此，在版本、文件名、大小、内容、分片大小和实际通道数都相同的情况下，可以重新计算出相同的 `sessionKey`。即使程序重启，也能定位磁盘上的历史传输状态，从而实现断点续传。

分片大小和通道数会影响临时文件的分区边界，所以它们也属于续传身份。修改这些设置会生成不同的会话键，不能直接按新边界使用旧通道文件。

`m_oTransferId` 关联**本次运行中的任务及所有连接**；`m_strSessionKey` 用于**定位跨任务、跨进程保存的历史状态**。头文件只声明这些字段，会话键生成规则已在 Codec 的 BuildSessionKey() 中实现；当前发送端 BuildMetadata 已调用该规则。

### 新增的 `m_nLaneCount`

```text
实际数据通道数 = min(用户请求的发送线程数, 总分片数)
```

例如请求 8 个发送数据线程，但文件只有 3 个分片，实际使用 3 个通道。每个通道对应一对发送/接收数据线程及一个独立临时文件；额外的任务协调/控制线程不计入该值。构造函数默认设为 1，真正发送前还要根据任务配置赋值。

通道总数 `laneCount` 及本连接的 `laneIndex` 将由新版 Metadata JSON 携带。`laneIndex = -1` 表示控制连接，`0..N-1` 表示数据通道；本连接的编号不需要增加到公共包头或文件元信息结构体中。

## `SReliablePendingPacket`：等待 ACK 的分片记录

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `m_nPacketSequence` | `quint64` | 当前等待确认的分片序号 |
| `m_nPayloadOffset` | `qint64` | 该分片在源文件中的起始偏移，单位是字节 |
| `m_nPayloadSize` | `int` | 该分片实际的数据长度，单位是字节 |
| `m_nRetryCount` | `int` | 已经补传的次数，初次发送不计入 |
| `m_nLastSendTimestampMs` | `qint64` | 最近一次发送时单调计时器的已流逝毫秒数，用于计算超时，不是 Unix 纪元时间 |

新版参考实现使用同一个 `QElapsedTimer` 的 `elapsed()` 记录和比较时间，不把它与系统日历时间相减。修改系统时间不会因此改变分片的超时判断；当前 MyServer 的 SendLane 已使用该单调计时方式。

## JSON 辅助函数与位图计数

| 函数 | 作用 |
| --- | --- |
| `BuildRangeJsonArray()` | 把缺失范围列表转换成 JSON 数组 |
| `ParseRangeJsonArray()` | 从 JSON 数组还原缺失范围列表；当前 Codec 已改用严格解析；这个旧辅助函数暂时保留，尚未删除 |

接收端恢复计数和发送端共享位图初始化统一使用 `oBitArray.count(true)`；该操作不会修改位图。

原函数中的学习点仍保留：`testBit(nIndex)` 读取位图第 nIndex 位，该位为 1 返回 true，为 0 返回 false，只读取、不修改；`setBit()` 才修改指定位置。区别是 `testBit()` 查询一个分片，`count(true)` 统计整个图中已确认的分片数量，不能互相替代。

```cpp
//恢复接收端计数。
m_stState.m_nReceivedPacketCount =
    static_cast<quint32>(oReceivedPacketMap.count(true));

//恢复发送端共享确认计数。
pTaskState->m_nConfirmedCount =
    pTaskState->m_oAckedPacketMap.count(true);
```

批量统计用于创建或恢复状态，不是每收到一个 ACK 都重新遍历整张图；正常收片和确认仍在状态变化时增减计数。

### `inline` 的作用

`inline` 是 C++ 的关键字，通常叫内联说明符。你当前代码里，最需要理解的是：它允许函数定义放在头文件中，被多个 .cpp 包含，而不会因此产生重复定义错误。

---

# `ReliableTransferUtils.h`：发送、接收共用的计算

> 本头文件集中保存发送与接收共用的计算，避免通道边界、分片偏移和尾片长度出现多份实现。

## 1. 为什么从 Session 中移出来？

之前的 GetLaneRange 是 ReliableReceiveSession 的私有成员函数，内部读取 `m_stState.m_stMetadata`；发送端没有接收会话对象，不能直接调用它。现在把元信息显式作为参数传入，函数不再依赖 Session 对象，放在公共头文件的全局作用域。

| 公共函数 | 返回结果 | 当前调用位置 |
| --- | --- | --- |
| `GetReliableLaneRange(stMetadata, nLaneIndex)` | SReliableLaneRange：通道分片区间、全局起始偏移、实际字节数 | Session 的创建/恢复文件、收片归属检查、最终合并；SenderWorker 的通道区间分配 |
| `GetReliablePacketLayout(stMetadata, nSequence)` | SReliablePacketLayout：一个分片的全局偏移及实际长度 | SendLane 的 SendDataPacket；Session 的 AcceptDataPacket |
| `GetReliablePacketCount(nFileSize, nSegmentSize)` | qint64 总分片数，除法向上取整 | SenderWorker 的 BuildMetadata；Codec 的 ValidateMetadata |

原来的私有 SLaneRange 已移为全局 SReliableLaneRange，字段含义和注释保留；Session 的旧 GetLaneRange 声明与实现已删除。发送与接收的规则现在来自同一份实现，避免两端分别修改公式而产生偏差。

## 2. 如何调用与保证合法性？

```cpp
#include "ReliableTransferUtils.h"

const SReliableLaneRange stLaneRange =
    GetReliableLaneRange(stMetadata, nLaneIndex);

const SReliablePacketLayout stLayout =
    GetReliablePacketLayout(stMetadata, nSequence);

const qint64 nPacketCount = GetReliablePacketCount(
    stMetadata.m_nFileSize,
    stMetadata.m_nSegmentSize);
```

这些是全局函数，不需要类名、Session 实例或额外对象；其他源文件需要包含头文件。定义写在头文件中，因此使用 inline，允许多个翻译单元包含；这不保证编译器一定展开调用。

调用方仍负责检查：总数计算要求文件大小和分片大小大于 0；通道和单片计算要求已校验的元信息、合法通道编号或分片序号。接收端原有的序号、通道、包头与长度检查全部保留，不能直接拿网络包提供的偏移替代期望值。返回总数为 qint64，超过位图容量的检查仍留在原调用方。

函数不修改共享状态，参数在计算期间不变即可，不在内部加锁。这不表示接收位图、计数和完成标记不需要锁，也不表示 const 自动提供线程安全。

## 3. 当前调用位置

Codec 校验元信息，SenderWorker 准备总分片数，SendLane 计算通道区间和单片位置，Session 创建、写入和合并通道文件时均调用公共计算。位图计数统一使用 Qt 的 count(true)，不另存一份手写遍历。

---

# `ReliablePacketCodec`

> Codec 负责字节格式、拆包和字段校验，不负责创建线程、建立连接或写文件；已由当前发送端和接收端调用。

Codec 使用七类消息、JSON 载荷和 UUID 原始字节编码，并统一处理拆包、字段校验和多通道握手信息。

## 1. 核心函数与建议阅读顺序

下表按职责列出核心函数，便于对照当前源码阅读。

| 顺序 | 函数 | 职责类别 | 阅读重点 |
| ---: | --- | --- | --- |
| 1 | [BuildPacket()](<../MyServer/ReliablePacketCodec.cpp>) | 封包 | 按固定顺序编码 62 字节包头 |
| 2 | [ParsePacketHeader()](<../MyServer/ReliablePacketCodec.cpp>) | 解析 | 解析包头并检查版本、类型、UUID 和载荷长度 |
| 3 | [TryTakePacket()](<../MyServer/ReliablePacketCodec.cpp>) | 拆包 | 半包保留，完整时只取一个包，后续粘连包继续保留 |
| 4 | [BuildSessionKey()](<../MyServer/ReliablePacketCodec.cpp>) | 身份 | 根据文件信息、分片大小、实际通道数和文件 SHA-256 生成续传身份 |
| 5 | [ValidateMetadata()](<../MyServer/ReliablePacketCodec.cpp>) | 校验 | 检查文件名、大小、分片数、通道数、摘要和会话键 |
| 6 | [ParseMissingRanges()](<../MyServer/ReliablePacketCodec.cpp>) | 解析 | 严格解析缺失分片范围，检查转换失败、顺序、重叠和通用边界 |
| 7 | [BuildMetadataPacket()](<../MyServer/ReliablePacketCodec.cpp>) | 封包 | 编码单 SHA-256、laneCount 和 laneIndex |
| 8 | [ParseMetadataPayload()](<../MyServer/ReliablePacketCodec.cpp>) | 解析 | 解析通道信息、检查数值转换，再调用元信息校验 |
| 9 | [ParseResumeStatePayload()](<../MyServer/ReliablePacketCodec.cpp>) | 解析 | 检查已接收数量，改用严格缺失范围解析 |
| 10 | [ParseResultPayload()](<../MyServer/ReliablePacketCodec.cpp>) | 解析 | 检查 success、message 的 JSON 类型，严格解析缺失范围 |

## 2. 公共包头的编码与解析

### `GetHeaderLength()` 与 `BuildPacket()`

```text
一个完整协议包 = 62 字节固定包头 + m_nPayloadSize 字节载荷
```

`GetHeaderLength()` 返回 Types 中的 `g_nReliablePacketHeaderLength`，当前为 62。

`BuildPacket()` 先创建并初始化包头字节数组，通过 `QDataStream` 按大端顺序写入各字段，再拼接载荷。它不直接把整个结构体内存发出去，也不使用 `sizeof(struct)` 作为网络格式。

```cpp
arrHeaderData.fill('\0', GetHeaderLength());
oDataStream.setByteOrder(QDataStream::BigEndian);
// 按 Types 中的字段顺序显式写入……
return arrHeaderData + arrPayload;
```

v3 已不写入 `m_nFlags`、`m_nReserved`、`m_nReserved64`，也不保留旧版多余的尾部填充。上层 Metadata、Data 等封包函数会设置载荷长度；直接调用 `BuildPacket()` 时，调用者也要保证头中的长度与实际载荷一致。

### `ParsePacketHeader()`

它只解析固定包头，不读取文件内容或 JSON 业务字段，主要检查：

- 输入字节数和头中的 `headerLength` 都是 62。
- 魔数正确、数据流读取成功、版本等于 3。
- 消息类型为 101～107，传输 UUID 非空。
- 载荷总上限为 16 MiB；Data 的载荷还不能超过 4 MiB。
- ACK、CompleteRequest 没有载荷，载荷长度必须为 0。

这里检查的是公共协议格式。具体分片是否属于某个通道、偏移和长度是否符合文件信息，需要后续接收业务层继续检查。

## 3. `TryTakePacket()`：统一处理半包和粘包

发送端和接收端以后都可以调用这个函数，避免各自再写一套拆包逻辑。每条 TCP 连接仍应有自己的 `QByteArray` 缓存。

```text
缓存不足 62 字节
    → 包头不完整，保留缓存
包头完整
    → 先解析并检查包头
缓存不足“包头 + 载荷”
    → 载荷不完整，保留缓存
完整包已到达
    → mid() 取载荷，remove() 移除当前包，后面的字节保留
```

函数返回值与 `bReady` 要一起看：

| 返回值 | bReady | 含义 |
| --- | --- | --- |
| `false` | `false` | 包头非法，调用方处理错误 |
| `true` | `false` | 目前字节不够，等待后续数据；不能使用本次输出当作完整包 |
| `true` | `true` | 已成功取出一个完整协议包，可以处理包头和载荷 |

因此，“返回 true”不等于“收到完整包”。例如只收到前 20 字节时，它返回 true，但 `bReady` 仍为 false。

```cpp
const qint64 nLength = GetHeaderLength()
    + static_cast<qint64>(stHeader.m_nPayloadSize);

arrPayload = arrBuffer.mid(GetHeaderLength(),
                          static_cast<int>(stHeader.m_nPayloadSize));
arrBuffer.remove(0, static_cast<int>(nLength));
```

`mid(pos, len)` 中，pos 是从 0 开始的截取位置，len 是截取的字节数。只有确认包完整后才执行 `remove()`；例如缓存为 `[包A][包B][包C的一部分]`，取出 A 后，B 和 C 的部分字节仍保留，调用方可以继续循环拆包。

## 4. `BuildSessionKey()`：生成稳定续传身份

```text
v3 | 文件名 | 文件大小 | 分片大小 | 实际数据通道数 | 文件SHA256
    ↓ UTF-8
SHA-256
    ↓ 十六进制字符串
sessionKey
```

相同的身份材料能重复生成相同的会话键，用于定位磁盘上保存的续传状态。分片大小和通道数会影响分区边界，因此也参与生成；UUID 不参与，新的发送任务可以通过相同会话键找到旧状态。

它计算的是“身份材料的摘要”，不会读取文件、计算文件内容摘要，也不会在这里生成任务 UUID。文件 SHA-256 必须由发送端事先计算好。

## 5. `ValidateMetadata()`：集中检查文件元信息

主要检查：

- 文件名非空，不是 `.`、`..`，不含 `/`、`\`、冒号或 NUL，结尾不是点或空格。
- 文件大小大于 0；当前仍不支持空文件。
- 分片大小在 8 KiB～4 MiB。
- 实际数据通道数在 1～32，且不超过总分片数。
- 文件 SHA-256 恰好是 64 个小写十六进制字符。
- 根据文件大小和分片大小重新计算总分片数，检查声明值一致且不超过 `int` 上限。
- 重新生成会话键，检查是否与声明的 `sessionKey` 一致。

分片数采用下面的等价向上取整方式，避免“文件大小 + 分片大小 - 1”的加法溢出：

```cpp
fileSize / segmentSize + (fileSize % segmentSize != 0)
```

它不检查 UUID，因为 UUID 来自公共包头，不在 Metadata JSON 中。调用方仍需将包头身份和解析出的元信息关联起来。

## 6. Metadata 握手：`BuildMetadataPacket()` 与 `ParseMetadataPayload()`

Metadata 的 JSON 内容如下：

| 字段 | 编码方式 | 作用 |
| --- | --- | --- |
| `sessionKey` | 字符串 | 定位历史续传状态 |
| `fileName` | 字符串 | 待传文件名 |
| `fileSize` | 十进制字符串 | 文件字节数，通过字符串保存整数值 |
| `segmentSize` | 整数 | 普通分片大小 |
| `totalPacketCount` | 整数 | 全部数据分片数 |
| `laneCount` | 整数 | 实际数据通道总数 |
| `laneIndex` | 整数 | 当前这条连接的通道身份 |
| `sha256` | 字符串 | 整个文件内容的 SHA-256 |

旧的 `md5` 字段已移除。UUID、文件大小、总分片数和窗口等还会按协议写入相应包头字段；窗口 `nWindowSize` 位于 Metadata 的包头中。

`ParseMetadataPayload()` 先解析 JSON 对象，检查文件大小字符串转整数是否成功，读取通道信息，再调用 `ValidateMetadata()`。校验成功后，才通过可选的 `pLaneIndex` 输出通道编号。

```cpp
static QByteArray BuildMetadataPacket(
    const SReliableFileMetadata &stMetadata,
    int nWindowSize,
    int nLaneIndex = -1);

static bool ParseMetadataPayload(
    const QByteArray &arrPayload,
    SReliableFileMetadata &stMetadata,
    QString &strErrorMessage,
    int *pLaneIndex = nullptr);
```

`nullptr` 表示调用方不需要接收这个额外输出值，不表示跳过对 laneIndex 的解析或合法性检查。通道号小于 -1 或不小于 laneCount 时会被拒绝；缺失字段用 -2 作为非法默认值，避免误认为控制连接。

## 7. `laneIndex` 是什么意思

`laneIndex` 是**数据通道编号**，用来说明：**当前这条 TCP 连接属于哪个通道。**

新版的设计是：

```text
一个发送数据线程
    ↕ 一条 TCP 长连接
一个接收数据线程
    ↓
一个独立临时文件
```

这一组就叫一个 lane（通道）。Codec 只把这个身份写进握手消息；创建对应线程、绑定通道及写文件，需要后续 Worker、Manager 等模块配合。

例如实际有 3 个数据通道：

| laneIndex | 含义 | 接收端写入位置 |
| ---: | --- | --- |
| `0` | 第 0 个数据通道 | `lane_0000.part` |
| `1` | 第 1 个数据通道 | `lane_0001.part` |
| `2` | 第 2 个数据通道 | `lane_0002.part` |
| `-1` | 控制连接，负责协调和请求最终合并 | 不写数据分片 |

这里要区分：

```cpp
laneCount = 3; // 一共有几个数据通道
laneIndex = 1; // 当前连接是哪个数据通道
```

所以：

```cpp
ReliablePacketCodec::BuildMetadataPacket(stMetadata, 16, 1);
```

表示：“这条连接属于第 1 号数据通道，每连接发送窗口为 16。”

它在 Metadata 握手时告诉接收端。接收端记住编号后，就知道该连接的数据应该写哪个临时文件。它是自己定义的业务编号，不是操作系统的线程 ID，也不是分片序号。

由于默认参数是 `nLaneIndex = -1`，省略第三个参数会构造控制连接的握手信息。数据连接应明确传入自己的编号。编号放在握手 JSON 中，不必加入每个数据包的公共包头或 `SReliableFileMetadata` 成员。

## 8. 缺失范围：`ParseMissingRanges()`

它是 `.cpp` 内部的静态辅助函数，由 ResumeState 和 Result 的解析函数共同调用。

```json
[["0", "2"], ["5", "7"]]
```

表示缺失分片 0、1、2、5、6、7，每个区间都是闭区间。端点用十进制字符串编码，与 `BuildRangeJsonArray()` 对应。

新解析会检查：输入必须为数组，每项必须恰好有两个可成功转换的字符串端点，起点不大于终点，区间按序排列且不重叠，终点小于 `int` 最大值。

```cpp
bool bStartOk = false;
const quint64 nStart = oRange.at(0).toString().toULongLong(&bStartOk);
```

这里通过 `bStartOk` 区分“合法的序号 0”和“转换失败后得到 0”。不能把非法端点默默当作分片 0。

它检查的是通用格式和边界；某个范围是否超出当前文件总分片数，还需要上层结合会话元信息核对。

当前 Codec 已不再调用 Types 中的旧 `ParseRangeJsonArray()`；该旧函数暂时保留，尚未删除。

## 9. ResumeState 与 Result 的解析

### `ParseResumeStatePayload()`

读取 `receivedPacketCount`，拒绝缺失、负数等非法计数，再调用 `ParseMissingRanges()` 解析 `missingRanges`。上层还要检查“已接收数量 + 缺失数量”与文件总分片数是否一致。

### `ParseResultPayload()`

要求 `success` 是 JSON 布尔值，`message` 是字符串，再严格解析 `missingRanges`。例如 `"success": "true"` 是字符串，不是合法的布尔结果。

解析成功只表示结果包格式合法，还要读取输出的 `bSuccess` 才能知道业务成功或失败；不能把该函数返回 true 直接当作文件传输成功。

## 10. 其他封包函数的作用

| 函数 | 作用 |
| --- | --- |
| `BuildResumeStatePacket()` | 将接收数量和缺失范围封装成恢复状态包 |
| `BuildDataPacket()` | 携带全局分片序号、全局文件偏移及分片内容 |
| `BuildAckPacket()` | 确认某个分片，载荷为空；ACK 不保证物理介质持久化或整个文件成功 |
| `BuildCompleteRequestPacket()` | 控制连接请求最终合并校验，载荷为空 |
| `BuildResultPacket()` | 编码 success、message、missingRanges |
| `BuildErrorPacket()` | 将错误文本转成 UTF-8 载荷 |
| `BuildJsonPacketPayload()` | 将 JSON 对象编码成紧凑字节数组 |
| `ParseJsonPacketPayload()` | 解析 JSON 并要求顶层为对象 |

这些函数负责构造或解析协议字节，实际是否发送 ACK、何时请求合并和如何处理失败，属于调用它们的业务层。

## 写入的细节点1：

`DataStream` 在序列化 `QByteArray` 时，不只写内容，还会先写长度,所以需要这样写，确保只写入的是内容

```cpp
oDataStream.writeRawData(arrTransferId.constData(),arrTransferId.size());
```

这个原有学习点仍然成立：UUID 固定编码为 16 个原始字节。解码时也使用 `readRawData()` 读取相同的 16 字节，再用 `QUuid::fromRfc4122()` 还原。

---

# `ReliableReceiveSession`

> Session 管理一个文件的共享接收状态，负责创建与恢复、分片写入、持久化、串行合并和校验；由 ReceiverWorker 调用。

## 1. 一个文件对应一个共享会话

多个数据连接接收同一个文件时，共享同一份元信息、位图和计数，但各通道写自己的临时文件。Session 不持有 `QTcpSocket`，也不负责监听或创建线程。

| 方法 | 职责 |
| --- | --- |
| 构造函数、`GetMetadata()` | 保存并提供创建后不再修改的文件元信息 |
| `GetLaneFilePath()` | 生成 `lane_0000.part` 等通道文件路径 |
| `LoadOrCreateSession()` | 首次创建状态，或校验并恢复历史接收记录 |
| `AcceptDataPacket()` | 校验分片、跳过重复数据、写入通道文件并更新进度 |
| `GetReceivedPacketCount()` | 加锁读取已接收的不同分片数 |
| `BuildMissingRangesLocked()` | 根据位图，把连续缺失分片压缩成闭区间 |
| `BuildResumeStatePacket()` | 在同一把锁内取得缺失范围和计数，再交给 Codec 封包 |
| `PersistSessionState()` / `PersistSessionStateLocked()` | 保存文件信息与接收位图 |
| `VerifyAndFinalizeSession()` | 检查缺失、顺序合并、校验摘要、生成正式文件 |
| `BuildAvailableFilePath()` | 选择未被占用的最终路径，候选耗尽返回空字符串 |

## 2. 状态和临时文件各管什么

`SReceiverSessionState` 保存这个文件的接收状态：

| 成员 | 含义 |
| --- | --- |
| `m_stMetadata` | 文件名、大小、分片规格、通道数、SHA-256、任务 ID 和续传会话键 |
| `m_strSessionDirectory` | 当前文件的临时会话目录 |
| `m_strPartFilePath` | 合并输出 `assembled.part` 的路径，不再是接收分片的单一文件 |
| `m_strStateFilePath` | `state.json` 的路径 |
| `m_strFinalFilePath` | 正式文件的候选路径；最终生成成功后记录实际路径 |
| `m_oReceivedPacketMap` | 全局接收位图，每一位对应一个分片 |
| `m_nReceivedPacketCount` | 已接收的不同分片数 |
| `m_nUnsavedPacketCount` | 新收到、但接收记录尚未保存的分片数 |
| `m_bCompleted` | 正式文件是否已经成功生成 |

```text
输出目录/
├─ session_完整会话键/
│  ├─ lane_0000.part    第 0 个数据通道接收的内容
│  ├─ lane_0001.part    第 1 个数据通道接收的内容
│  ├─ state.json        文件信息和全局接收位图
│  └─ assembled.part    最终合并校验通过、commit() 成功后生成
└─ video.mp4            assembled.part 改名后的正式文件
```

这里不再使用旧版的单个 `payload.part`。首次创建通道文件时，`resize()` 设置的是文件的**逻辑大小**，不代表分片已收到，也不保证物理磁盘空间已全部分配；完成情况仍以位图为准。

## 3. 通道范围：调用全局 `GetReliableLaneRange()`

原来创建通道文件、检查分片归属、最终合并中重复的公式，先提取成了 Session 私有 GetLaneRange，现已进一步移到 ReliableTransferUtils.h 的全局 `GetReliableLaneRange(stMetadata, nLaneIndex)`，供发送端也复用。返回的 `SReliableLaneRange` 包含四个值：

| 成员 | 含义 |
| --- | --- |
| `m_nFirstSequence` | 第一个全局分片序号，包含它 |
| `m_nEndSequence` | 分片结束边界，不包含它 |
| `m_nStartOffset` | 通道起点在整个文件中的字节偏移 |
| `m_nByteSize` | 当前通道文件的实际字节数 |

```text
first = 总分片数 × 通道编号 ÷ 通道数
end   = 总分片数 × (通道编号 + 1) ÷ 通道数
startOffset = first × 分片大小
byteSize = min(end × 分片大小, 文件大小) - startOffset
```

例如总分片数为 11、通道数为 3：

| 通道 | 分片区间 | 负责的序号 |
| ---: | --- | --- |
| 0 | `[0, 3)` | 0、1、2 |
| 1 | `[3, 7)` | 3、4、5、6 |
| 2 | `[7, 11)` | 7、8、9、10 |

整数除法舍去小数，但相邻区间共用同一个边界，因而没有重叠或遗漏。先转成 64 位再乘，避免中间按 `int` 乘法溢出；结束字节位置用 `qMin()` 限制在文件末尾，以适配不足一片的最后一个分片。

```cpp
const SReliableLaneRange stLaneRange =
    GetReliableLaneRange(stMetadata, nLaneIndex);
const qint64 nLocalOffset =
    nExpectedOffset - stLaneRange.m_nStartOffset;
```

例如通道 1 从第 3 片开始、每片 8 KiB，第 3 片的全局偏移是 24 KiB，但在 `lane_0001.part` 中要从偏移 0 写入。注意：通道区间是 `[first, end)`，协议中的缺失范围仍是 `[start, end]` 闭区间。

## 4. `LoadOrCreateSession()`：创建或恢复

首次接收时，先调用 Codec 校验元信息，再用**完整 `sessionKey`** 生成目录；初始化全为 `false` 的位图，为各通道创建对应大小的文件，选择正式文件候选路径，保存初始状态。`assembled.part` 留到最终合并阶段生成。

如果已经存在 `state.json`，则先检查状态版本（当前为 3）、文件信息、通道数、SHA-256 和会话键是否匹配，再恢复位图：

```text
JSON 中的 Base64 字符串
    ↓ 解码
QDataStream 二进制数据
    ↓ 按 Qt_5_12 格式反序列化
QBitArray
    ↓ oReceivedPacketMap.count(true)
重新统计已接收数量
```

保存和读取都固定使用 `QDataStream::Qt_5_12`；这是序列化格式，不是要求运行环境必须为 Qt 5.12。恢复前先核对位数，恢复后检查流状态、剩余字节、位图长度和 Base64 表示，避免接受损坏或格式不符的数据；这些格式检查不等于对位图做内容摘要校验。

恢复时还要求**所有通道文件存在且大小正确**。不匹配就返回错误，保留原状态和通道文件，不通过新建、resize 或删除旧文件来掩盖问题。

正式路径始终根据当前输出目录和文件名重新选择，不直接信任 JSON 中保存的绝对路径。这样也能避开传输中断期间新出现的同名文件。

## 5. `AcceptDataPacket()`：接收一个分片

```text
检查任务、序号、通道归属、偏移和长度
    ↓
锁内检查位图：已收到则允许重复确认，不重复写入和计数
    ↓
锁外按局部偏移写入该通道文件，flush() 并关闭文件
    ↓
锁内设置位图、增加计数，必要时保存状态
```

写文件使用 `ReadWrite | ExistingOnly`，文件必须已经由初始化阶段创建；`seek()` 定位的是**通道内部的局部偏移**。写入完整长度且 `flush()` 成功，才提交内存中的接收记录。

| 情况 | `bAccepted`：可以回复 ACK | `bNewPacket`：新增进度 |
| --- | --- | --- |
| 新分片写入成功 | `true` | `true` |
| 分片之前已收到 | `true` | `false` |
| 校验或写入失败 | `false` | `false` |

这个区别可去ReliableReceiverWorker::HandlePacket里看明白原因

Session 只返回处理结果，真正通过 socket 发送 ACK 由后续连接处理模块负责。锁外写文件的前提是**同一个通道同一时刻只有一个接收线程拥有**，后续 Store 将负责登记和保证这一点，不能任意并发调用同一通道的写入。

### 为什么 TCP 已有 ACK，还要业务 ACK？

TCP ACK 确认字节到达对端 TCP 接收端；`AckPacket` 确认接收程序已接受指定分片，用来推进业务发送窗口和统计进度。TCP 不知道“第 5 个文件分片是否已成功写入”。逐片 ACK 是本协议的选择，不是所有 TCP 文件传输都必须采用的设计。

重复分片仍可确认，因为发送端可能没处理到上一次 ACK。业务 ACK 不等于 `state.json` 已保存，也不等于整个文件已成功；最终成功以合并校验后的 `Result` 为准。`flush()` 把 Qt 的缓冲写入提交给操作系统，不保证断电后物理介质上的数据一定保留。

## 6. 持久化：内容及时写，进度批量存

文件内容由各通道文件保存；接收进度和身份信息由 `state.json` 保存。状态 JSON 包括 `stateVersion`、文件名、大小、分片规格、通道数、SHA-256、会话键、最终候选路径、计数及位图；位图以 `QDataStream → Base64` 编码。

```cpp
QMutexLocker oLocker(&m_oMutex);
return PersistSessionStateLocked(strErrorMessage);
```

外部调用 `PersistSessionState()` 获取锁；已经持锁的内部代码调用 `PersistSessionStateLocked()`，避免重复加同一把非递归锁。保存采用 `QSaveFile`，检查打开、写入长度和 `commit()`，成功后才把未保存计数清零，不再使用旧版的直接 Truncate 重写状态文件。

### 为什么累计 `>= 32` 个新分片才保存？

每收一片都立即更新内存位图和计数，但批量保存可以减少“整个位图序列化、写 JSON、提交状态文件”的频率，也减少持锁保存的次数。

如果每次保存都成功，异常退出时最多有 31 个新分片的接收记录尚未写入状态文件；恢复后这些分片会被认为缺失并重新发送。例如实际收到了 35 片、状态保存到了第 32 片，就可能补传其余 3 片。保存失败时积压数量可能超过 31。

条件用 `>=` 而不是 `==`：保存失败不会清零，下一片使计数变成 33 时仍应重试。保存失败作为 `strWarningMessage` 返回，不撤销已经成功写入的分片；调用方负责显示警告。

Session 本身在创建/恢复结束时、累计分片达到阈值时保存；完成检查发现缺失时也会尝试保存。Store 的 DetachSession() 已在最后一个通道退出时尝试保存；网络模块触发解绑以及停止监听时的调用接入仍待实现。状态原子替换也不意味着通道数据与位图具备严格的断电一致性。

## 7. `VerifyAndFinalizeSession()`：合并时同步校验

只有缺失范围为空时才开始合并。分片在成功写入并关闭通道文件之后才置位，所以正在写入、尚未提交位图的分片仍会阻止提前合并。

```text
lane_0000.part → lane_0001.part → …… → 最后一个通道文件
    ↓ 按编号顺序读取，复用 4 MiB 缓冲区
同一批字节：写入 QSaveFile 内部临时文件 + ReliableSha256::AddData()
    ↓ 大小和摘要均匹配
commit() → assembled.part
    ↓ 再次选择可用路径，rename()
正式文件 → 标记完成 → 尝试清理已知通道文件和 state.json
```

通道按连续区间划分，编号顺序就是文件顺序，不需要额外排序。合并复杂度为 `O(文件字节数)`，缓冲区不随文件大小增长。`AddData()` 保留累计计算状态，得到的是**整个文件的 SHA-256**，不是每块单独一个摘要；合并和哈希共用一次通道文件读取，避免再通读一遍合并文件。

循环中 `read()` 最多读取“缓冲区容量”和“当前通道剩余字节数”中的较小值。`nLaneReadSize` 记录当前通道已读字节，换通道时归零；`nMergedSize` 记录整个文件累计合并字节。

合并不直接写正式文件，避免未完成或未通过校验的内容被当作成功结果。失败时保留通道文件和已有状态供检查或恢复；QSaveFile 尚未提交的临时写入会被放弃。清理目录使用 `rmdir()`，只删除空目录，不递归删除未知文件。

### 重复完成请求为什么可以直接成功？

接收端可能已生成正式文件，但成功回复未被发送端处理到，随后收到重复完成请求。同一个 Session 中 `m_bCompleted` 为 `true` 时返回已有正式路径，不再尝试合并已清理的通道文件。这叫幂等处理；它依赖**原 Session 对象仍然存在**，不是跨进程的完成记录。会话释放或程序重启后的完成识别，需要另外设计。

### 路径选择的一个小细节

`BuildAvailableFilePath()` 在重名时依次尝试 `video_1.mp4`、`video_2.mp4`，候选耗尽返回空字符串，调用方检查失败。它只是选择路径，不会提前占用文件名，最终仍须检查 `rename()` 的结果。

当前使用 `QFileInfo::completeBaseName()` 搭配 `suffix()`。例如 `archive.tar.gz` 对应 `archive.tar` 和 `gz`，重名候选为 `archive.tar_1.gz`；若把 `completeBaseName()` 与 `completeSuffix()` 直接拼接，会重复中间扩展名。`absolutePath()` 返回文件所在目录，不包含文件名。

## 8. 会话锁究竟保护什么

锁不会自动冻结整个 `m_stState`。所有线程必须主动遵守同一把锁的约定，才能保护可变共享状态。

| 成员 | 需要保护的原因 |
| --- | --- |
| 接收位图、已接收计数、未保存计数 | 多线程读写，且三个值需要一致地更新 |
| `m_bCompleted`、最终路径 | 完成请求会读取或修改，重复请求必须看到一致结果 |

元信息、会话目录、通道路径和状态路径在初始化成功、交给其他线程使用后不再修改，因此可以并发读取。公共 `GetReliableLaneRange()` 显式接收元信息，只读参数并计算局部变量，不要求加锁；`const` 本身不是线程安全保证，依据是这些数据之后确实不再被修改。

`Locked` 后缀表示**调用方必须已经持有会话锁**，如 `BuildMissingRangesLocked()`、`PersistSessionStateLocked()`；不表示函数内部会自动加锁。初始化和最终合并持锁范围较大，日常分片接收则把通道文件写入放在锁外。

## 9. 停止标记：`loadAcquire()` 不是加锁

```cpp
QSharedPointer<QAtomicInt> m_pStopFlag;

//后续停止管理逻辑发出请求：
m_pStopFlag->storeRelease(1);

//合并循环在每一块读取前检查：
if (m_pStopFlag->loadAcquire())
{
    return false;
}
```

约定 `0` 表示继续，非 0 表示请求停止。`QSharedPointer` 管生命周期，`QAtomicInt` 提供原子读写；`loadAcquire()` 不获取 `m_oMutex`，所以即使合并一直持有会话锁，其他线程仍能发出停止请求。

这是合作退出：不会强行中断正在进行的磁盘读写，而是在下一次检查时响应。如果 acquire 读取观察到 release 写入的值，发布前的相关内存写入可对读取线程可见。当前 Session 已具备检查点，Store 的 RequestStop() 已实现原子写入停止请求，Manager::StopListening 和界面停止按钮已接通。

---

# `ReliableReceiveSessionStore`

> Store 独立存放于 ReliableReceiveSessionStore.h/.cpp，负责 AttachSession、DetachSession、RequestStop 和 IsStopping；Manager 每轮监听创建一份，接收 Worker 共享使用。

## 1. 引入共享指针控制

```cpp
QSharedPointer<QAtomicInt> m_pStopFlag;
```

停止标记能安全跨线程读写，是因为它本身是 QAtomicInt

**`m_pStopFlag` 是一个“请求停止接收任务”的共享开关。** Store 和它管理的各个 Session 都能访问同一个开关。

这里的数值是我们自己约定的：

| 值 | 含义 |
| --- | --- |
| `0` | 尚未请求停止，可以继续工作 |
| `1` | 已请求停止，应尽快退出工作 |

构造时：

```cpp
m_pStopFlag(new QAtomicInt(0))
```

初始值为 `0`，所以刚启动时允许工作。

### `RequestStop()`：发出停止请求

```cpp
void ReliableReceiveSessionStore::RequestStop()
{
    m_pStopFlag->storeRelease(1);
}
```

`storeRelease(1)` 把这个原子整数设置为 `1`。

业务含义就是：

```text
通知所有共享这个标记的会话：准备停止了。
```

它**不会直接关闭 socket，也不会强行结束线程**，只是设置开关。工作代码必须检查这个开关，才能响应。

例如之前的合并循环：

```cpp
if (m_pStopFlag->loadAcquire())
{
    strErrorMessage = QStringLiteral("接收端正在停止，取消合并。");
    return false;
}
```

读到 `1` 后，主动返回，停止合并。

### `IsStopping()`：查询是否已经请求停止

```cpp
bool ReliableReceiveSessionStore::IsStopping() const
{
    return m_pStopFlag->loadAcquire() != 0;
}
```

`loadAcquire()` 原子读取当前值，随后判断它是否非零：

```cpp
//读到 0：
return 0 != 0; //false，尚未请求停止

//读到 1：
return 1 != 0; //true，已请求停止
```

后续加入会话时就可以写：

```cpp
if (IsStopping())
{
    strErrorMessage = QStringLiteral("接收任务正在停止，不再接收新会话。");
    return {};
}
```

两者配合起来是：

```text
启动：flag = 0
    ↓
正常接收、允许加入会话
    ↓
调用 RequestStop()：flag = 1
    ↓
IsStopping() 返回 true，拒绝新的加入请求
已有合并循环读到 1，主动退出
```

注意，**`1` 表示“已经请求停止”，不表示“所有线程已经停止完成”**。真正完成停止，还需要后续关闭连接、等待线程退出。

### 为啥不用信号槽机制来做这个同步

可以用信号槽来**发出停止通知**，但它不能让正在执行的接收或合并代码“随时被打断”。

先纠正一个概念：这里不是共享指针在做同步。

```cpp
QSharedPointer<QAtomicInt> m_pStopFlag;
```

两部分的职责不同：

- `QSharedPointer`：让 Store 和各个 Session 共享这个对象，管理生命周期。
- `QAtomicInt`：让多个线程可以安全地读写停止标记。

**信号槽和原子标记，解决问题的方式不同。**

跨线程使用默认的信号槽连接时，通常采用队列连接：

```text
主线程发出停止信号
    ↓
停止请求进入工作线程的事件队列
    ↓
工作线程有机会处理事件时，执行停止槽函数
```

问题就在最后一步：**如果工作线程正在执行一个很长的合并循环，它暂时没有返回事件循环，停止槽就可能迟迟执行不了。**

例如：

```text
工作线程正在合并一个大文件
    ↓
主线程发出停止信号
    ↓
停止槽排队等待
    ↓
合并结束，工作线程才处理停止槽
```

这样就失去了“合并期间主动停止”的意义。原子标记则不需要工作线程先处理事件，其他线程直接设置它，合并循环下一次检查时就可以退出。

你可能会想到用 `Qt::DirectConnection`：它确实不会排队，但**槽函数会在发出信号的线程执行**，并不会抢占或中断接收线程。槽里如果只是设置原子停止标记，可以做到；如果直接操作另一个线程的 socket 或共享状态，就需要考虑线程安全。

所以，两种机制可以配合：

```text
用户点击停止
    ↓
信号槽调用停止入口
    ↓
设置原子停止标记
    ↓
各工作线程在检查点主动退出
```

另外，当前 Store 是普通 C++ 类，若要直接作为 Qt 槽的接收对象，还需要调整为 `QObject`，或者通过其他 `QObject` 的槽或 lambda 转接。

**你的想法适合用来组织“停止请求怎么传递”，原子标记适合让正在运行的循环及时察觉请求。** 无论哪一种，都不是强行中断磁盘读写；停止速度仍取决于检查点的频率，以及当前操作什么时候返回。

## 2. UUID 和 SessionKey的区别？整个传输的创建流程？

我们用一次具体传输，把这三个问题连起来看。

**先区分两个身份：UUID 标识“这一次任务”，SessionKey 标识“可以使用哪份续传数据”。**

假设你发送：

```text
文件：video.mp4
文件大小：100 MiB
分片大小：64 KiB
数据通道数：3
文件 SHA-256：H
```

Codec 根据这些材料生成会话键：

```text
v3 | video.mp4 | 100 MiB | 64 KiB | 3 | H
    ↓ 计算 SHA-256
SessionKey = K
```

为了方便说明，下面用 `A、B` 表示不同 UUID，用 `K、L` 表示不同 SessionKey。

| 身份 | 例子 | 用途 |
| --- | --- | --- |
| `m_oTransferId`，UUID | 本次任务为 `A` | 把这次任务的控制连接、数据连接关联起来 |
| `m_strSessionKey` | 文件及分片配置对应 `K` | 定位磁盘上的 `session_K` 续传目录 |

**同一任务内的所有连接使用相同 UUID；重新发起一个任务时使用新 UUID。相同文件和相同配置则可以重新算出相同 SessionKey。**

---

### 1．为什么返回同一个 Session，而不复制对象？

这次任务 `A` 有这些连接：

```text
控制连接：UUID=A，SessionKey=K，laneIndex=-1
数据连接0：UUID=A，SessionKey=K，laneIndex=0
数据连接1：UUID=A，SessionKey=K，laneIndex=1
数据连接2：UUID=A，SessionKey=K，laneIndex=2
```

它们是在**共同接收同一个文件**，因此必须共享：

```text
同一份接收位图
同一个已接收计数
同一套通道文件路径
同一把会话锁
同一个完成标记
```

所以，返回的是：

```cpp
return oEntry->m_pSession;
```

这会复制一个 `QSharedPointer`，让调用者共同持有**同一个 Session 对象**：

```text
Store 中的共享指针 ──┐
控制连接的共享指针 ──┤
数据连接0的共享指针 ─┼──→ 同一个 Session A
数据连接1的共享指针 ─┤
数据连接2的共享指针 ─┘
```

例如，通道 0 接收成功后：

```cpp
m_oReceivedPacketMap.setBit(0, true);
```

其他连接访问的也是这份位图，所以控制连接检查完成情况时能看到这个更新。

**如果每个连接都拥有一份独立 Session，它们就各自维护一份位图，无法直接汇总整个文件的接收状态。**

---

### 2．首次连接：创建一个 Session

假设控制连接首先到达：

```text
UUID=A，SessionKey=K，laneIndex=-1
```

此时两个登记表都是空的：

```text
m_mapSessions：没有 A
m_mapSessionKeys：没有 K
```

因此执行：

```cpp
QSharedPointer<ReliableReceiveSession> pSession(
    new ReliableReceiveSession(...));

pSession->LoadOrCreateSession(strErrorMessage);
```

初始化成功后登记：

```text
m_mapSessions：
    A → Session A，已占用通道 {-1}

m_mapSessionKeys：
    K → A
```

第一张表用来找对象，第二张表说明：**续传目录 K 当前由任务 A 使用。**

谁先到达并不影响原理；如果数据连接 0 先到，也可以由它首次创建会话。

#### 疑问：`stConnection` 没有 Session，就一定要创建吗？

**不一定。** Worker 中的 `stConnection.m_pSession.isNull()` 只表示“这条连接还没有加入会话”，不是“这个任务在整个接收端没有会话”。随后由 `AttachSession()` 查询 Store 的全局登记：

```text
当前连接未加入会话 → AttachSession()
    → UUID 已登记：检查身份与通道占用，加入同一个 Session
    → UUID 未登记：检查 SessionKey 占用，创建对象并新建或恢复磁盘状态
```

当前 AttachSession 没有要求 `nLaneIndex == -1` 才能首次创建会话；合法数据连接也可以先创建。编号范围由 Worker 调用的 `ParseMetadataPayload()` 检查。发送端安排“控制连接先握手、再启动数据连接”，是当前发送流程的顺序，接收端没有强制这个先后条件。

---

### 3．同一任务的其他连接到达：加入已有 Session

数据连接 0 到达：

```text
UUID=A，SessionKey=K，laneIndex=0
```

函数发现：

```text
A 已存在
SessionKey 仍然是 K
通道 0 尚未占用
```

于是登记通道并返回：

```cpp
oEntry->m_setActiveLanes.insert(0);
return oEntry->m_pSession;
```

登记项变成：

```text
A → 同一个 Session A，已占用通道 {-1, 0}
```

随后通道 1、2 到达，集合逐渐变成：

```text
{-1, 0, 1, 2}
```

**这里没有再创建 Session，也没有重新初始化通道文件。**

---

### 4．为什么有那些拒绝加入的判断？

| 情况 | 具体例子 | 为什么拒绝 |
| --- | --- | --- |
| 已请求停止 | 点击停止后，又来了新的连接 | 接收任务正在退出，不再接受新的加入请求 |
| 同 UUID，不同 SessionKey | 已登记 `A/K`，另一个连接却带来 `A/L` | 同一个任务的文件信息或分片配置不一致，可能是发送端关联错了任务 |
| 同一通道重复占用 | `A/K/通道0` 仍连接着，又来一条连接申请通道0 | 防止两个接收线程同时写同一个通道文件 |
| 不同 UUID，相同 SessionKey，且旧任务仍占用 | `A/K` 正在传，用户又启动任务 `B/K` | 两个独立任务会操作同一个 `session_K` 目录，必须避免冲突 |

第三种情况在重连时也可能短暂出现：新连接到了，但旧连接还没有完成解除登记。旧通道释放后，才可以重新申请。

至于不同 UUID、不同 SessionKey：

```text
任务 A → SessionKey K
任务 B → SessionKey L
```

它们对应不同会话目录，可以分别创建 Session，同时接收。

---

### 5．那相同 SessionKey 为什么又能用于断点续传？

关键是：**旧任务是否还在占用。**

假设任务 A 中断，所有连接退出。后续的 `DetachSession()` 会尝试保存状态，并移除登记：

```text
内存登记：
    删除 A → Session A
    删除 K → A

磁盘：
    session_K/
        lane_0000.part
        lane_0001.part
        lane_0002.part
        state.json
```

过一会儿，你重新点击发送：

```text
新任务 UUID=B
相同文件和配置 → SessionKey 仍然是 K
```

这时：

```text
m_mapSessions：没有 B
m_mapSessionKeys：K 已经没有任务占用
```

因此可以创建一个新的 Session B。它调用：

```cpp
LoadOrCreateSession(...)
```

通过相同的 `K` 找到磁盘上的 `session_K`，恢复旧位图和通道文件，继续补传。

如果恢复时发现状态损坏、文件不匹配或通道文件缺失，初始化就失败，不会把这个 Session 登记进去。

所以两个标识分别回答的是：

```text
UUID：
这些连接是不是属于同一次传输任务？

SessionKey：
这次任务能不能使用磁盘上保存的那份续传数据？
```

## 3. 整个传输的退出流程

用刚才的任务 A 举例，最开始登记的是：

```text
UUID=A
已占用通道：{-1, 0, 1, 2}
```

各连接依次退出：

| 退出的连接 | 剩余通道 | 本次操作 |
| --- | --- | --- |
| 数据通道 0 | {-1, 1, 2} | 只移除编号 0 |
| 数据通道 1 | {-1, 2} | 只移除编号 1 |
| 数据通道 2 | {-1} | 控制连接还在，保留会话 |
| 控制连接 | {} | 保存状态，移除两个表中的登记 |

退出顺序可以不同，判断依据是集合是否为空，不是控制连接是否退出。

这里再注意三个细节。

### 第一，解除登记不等于删除临时文件。

```cpp
m_mapSessionKeys.remove(...);
m_mapSessions.erase(...);
```

操作的是内存里的哈希表，磁盘上的通道文件和 state.json 保留，用于后续恢复。正式接收成功后的文件清理，已经由 Session 的完成函数负责。

### 第二，移除共享指针，不等于立即销毁 Session。

Store 不再持有之后，当前连接仍可能持有：

```cpp
QSharedPointer<ReliableReceiveSession>
```

等最后一个持有者释放，Session 才自动销毁。后面的连接清理代码还需要清空自己的会话指针。

### 第三，调用前必须先结束当前连接对通道文件的处理。

应该是：

```text
当前连接停止接收和写入
    ↓
DetachSession() 归还通道
    ↓
清空该连接持有的 Session 指针
```

不能一边继续写通道文件，一边把通道归还给其他连接；同一条连接也应只执行一次有效解绑。

保存期间仍持有 Store 锁，是为了避免新任务在旧任务尚未完成状态保存时，就重新占用同一个续传目录。保存失败会输出警告，但仍解除登记；最近未保存的进度之后可能需要重新补传。

## 4. 现在这个设计是不是接收端可以同时处理，两个发送端发送来的两个文件？

**是的，按照现在的 Store + Session 设计，接收端可以同时接收两个发送端传来的两个文件。** ReceiverWorker 和 Manager 负责连接与线程调度；实际同时收发多个文件仍需网络联调，Store 层支持并发不等于已经通过整体验收。

例如：

```text
发送端 A：发送 video.mp4
    UUID=A，SessionKey=K
        ↓
    Session A
    独立的位图、进度、会话锁和通道文件

发送端 B：发送 archive.zip
    UUID=B，SessionKey=L
        ↓
    Session B
    独立的位图、进度、会话锁和通道文件
```

Store 同时登记：

```text
m_mapSessions：
    A → Session A
    B → Session B

m_mapSessionKeys：
    K → A
    L → B
```

**两个 Session 各有自己的锁**，因此 A 更新位图或合并文件时，不会因为持有 A 的会话锁而锁住 B。Store 的公共锁主要用于加入、退出和登记管理，日常分片写入不需要持有它。

但有一种情况会拒绝：**两个发送端同时发送相同文件，并且文件名、分片大小、通道数等身份材料都相同。**

它们即使 UUID 不同，也会得到相同的 SessionKey：

```text
发送端 A：UUID=A，SessionKey=K → 已占用
发送端 B：UUID=B，SessionKey=K → 拒绝加入
```

因为两者会操作同一个 `session_K` 续传目录。当前设计允许多个不同会话并行接收，但不允许两个独立任务同时占用同一份续传数据。

### Session和SessionKey的区别：

**Session 是“管理文件接收过程的对象”，SessionKey 是“标识续传数据的字符串”。** 它们不是同一种东西，只是名字比较像。

拿接收 `video.mp4` 举例：

| 名称 | 实际类型 | 里面是什么 |
| --- | --- | --- |
| Session | `ReliableReceiveSession` 对象 | 元信息、接收位图、进度、文件路径、锁，以及处理这些状态的函数 |
| SessionKey | `QString` | 根据文件信息和分片配置计算出的 SHA-256 字符串，例如 `a8f3…` |

也就是说：

```text
SessionKey：一个字符串，用来找到 session_a8f3… 目录。

Session：一个正在工作的 C++ 对象，
         负责读取这个目录中的状态、接收分片、更新位图和合并文件。
```

回到这两个表：

```text
m_mapSessions：
    A → Session A
```

表示：**根据任务 UUID `A`，找到内存中的接收会话对象。**  

严格来说，表里保存的是包含共享指针和通道集合的 `SSessionEntry`。

```text
m_mapSessionKeys：
    K → A
```

表示：**续传键 `K` 对应的那份磁盘数据，目前被任务 `A` 占用。** 这个表没有保存 Session 对象。

最能体现区别的是程序重启：

```text
重启前：
任务 A → Session A → 使用 session_K 中的数据

重启后：
Session A 已随进程销毁，但 session_K 目录还在。
新任务 B → 新建 Session B → 通过相同的 K 恢复旧数据。
```

因此，**Session 对象可以重新创建，SessionKey 则可以保持相同，让新对象找到之前保存的接收进度。**

# `ReliableTcpServer`

> ReliableTcpServer 继承 QTcpServer，通过描述符把新连接交给 Manager，再由接收线程中的 Worker 创建 QTcpSocket。

## Server的行为改变

```cpp
void ReliableTcpServer::incomingConnection(
    qintptr nSocketDescriptor)
{
    emit SignalConnectionAccepted(nSocketDescriptor);
}
```

这里虽然只有几行，但改变了一个关键行为。

QTcpServer 的默认 incomingConnection() 会创建 QTcpSocket，加入待处理连接队列，供你调用 nextPendingConnection() 取得。

我们重写后，不调用基类的这个默认实现，而是通过自己的信号转交描述符。因此，新版这条路径后续不再使用 nextPendingConnection() 取连接。

## 描述符是什么？

可以先把它理解为：操作系统用来标识这条已建立连接的句柄。

```cpp
qintptr nSocketDescriptor
```

它不是文件传输 UUID，也不是数据通道编号。

后面 Worker 会执行：

```cpp
m_pSocket = new QTcpSocket(this);
m_pSocket->setSocketDescriptor(nSocketDescriptor);
```

含义是：

```text
创建一个 Qt socket 对象
    ↓
让它管理操作系统已经接受的那条 TCP 连接
```

不是重新连接一次，也不是复制出第二条 TCP 连接。

这样，QTcpSocket 就从创建开始属于接收线程，后续读写和关闭也在那个线程中完成。

## 入口与线程的配合

```text
监听线程：ReliableTcpServer 接受连接
    ↓ SignalConnectionAccepted(描述符)
Manager：创建接收线程，把 Worker 移入该线程
    ↓ 在接收线程执行 AcceptConnection()
Worker：创建 QTcpSocket，并通过 setSocketDescriptor() 接管连接
```

`incomingConnection()` 是 Qt 在接受新连接时调用的虚函数，`override` 用于让编译器检查是否正确重写。这里不调用基类默认实现，因此不走默认的待处理 socket 队列；后续通过自定义信号分派连接。

三个“编号”要分开：

| 标识 | 标识什么 |
| --- | --- |
| `nSocketDescriptor` | 操作系统中的这条网络连接 |
| 传输 UUID | 当前文件传输任务，所有相关连接共用 |
| `laneIndex` | 当前连接的角色；-1 为控制，0..N-1 为数据通道 |

构造函数前的 `explicit` 禁止通过构造函数进行隐式转换，不影响 `new ReliableTcpServer(this)` 这样的显式创建。继承处必须写 `public QTcpServer`；省略 `public` 会变成私有继承，影响 Manager 从外部调用 `listen()`、`close()` 等接口。

---

# `ReliableReceiverWorker`

> Worker 在接收线程中接管连接、清理缓存并拆包，处理 Metadata、Data 和 CompleteRequest；文件状态交给 Session。

## 方法职责速查

| 方法 | 做什么 |
| --- | --- |
| 构造函数 | 保存共享 Store，初始化连接状态；不创建 socket |
| `AcceptConnection()` | 在接收线程创建 socket，接管描述符，关联 readyRead/disconnected，并检查已有可读数据 |
| `OnSocketReadyRead()` | 追加本连接缓存，循环调用 Codec 取完整包，再交给 HandlePacket |
| `HandlePacket()` | 处理握手、分片和完成请求，构造对应协议回复 |
| `OnSocketDisconnected()` | 解除通道登记，清空会话指针，通知 Worker 工作结束 |
| `Stop()` / 析构函数 | 在所属线程关闭 socket，兜底解绑并释放对象 |

Worker 管“这条连接怎么收发”，Session 管“这个文件接收到了哪里”，Store 管“连接和文件会话怎么关联”。Worker 不需要重复实现 Session 中的文件写入、位图统计或合并算法。

## 区分两种状态

| 状态 | 属于谁 | 是否跨连接共享 |
| --- | --- | --- |
| `SConnectionState` | 一条 TCP 连接 | 不共享，缓存和通道编号各自独立 |
| `SReceiverSessionState` | 一个文件会话 | 同一文件的多个连接共享位图和进度 |

## 区分两种断开

```cpp
m_pSocket->disconnect(this);
```

断开的是 Qt 信号槽连接。

```cpp
m_pSocket->abort();
```

关闭的是 TCP 网络连接。

先断开信号槽，再主动关闭 socket，就不会因为本次关闭又进入 OnSocketDisconnected()，重复执行同一套退出流程。

## Worker断开连接的流程

```text
对端断开
    ↓
OnSocketDisconnected()
    ↓
解除通道登记，清空共享会话指针
    ↓
SignalWorkerFinished()
    ↓
配套连接请求接收线程退出
    ↓
Worker 被回收，析构调用 Stop() 清理 socket
```

## 类创建时的细节：

```cpp
ReliableReceiverWorker::ReliableReceiverWorker(
    const QSharedPointer<ReliableReceiveSessionStore> &pStore)
    : QObject(nullptr),
      m_pSocket(nullptr),
      m_stConnection(),
      m_pStore(pStore)
{
}
```

这里有意做了两件事：

```cpp
QObject(nullptr)
```

不设置父对象，方便 Manager 使用 `moveToThread()` 把 Worker 移入接收线程；当前 Manager 已接入这一安排。

```cpp
m_pSocket(nullptr)
```

构造时不创建 socket。因为 Worker 可能先在主线程创建、再移到接收线程；socket 应当等进入接收线程之后再创建。

## 接收缓存与 `readyRead`

```text
readyRead → readAll() → 追加 SConnectionState 的接收缓存
    ↓
TryTakePacket() 每次取一个包
    ├─ 返回 false：包头等协议格式非法，记录日志并 abort()
    ├─ true 且 bReady=false：正常半包，保留缓存等待后续字节
    └─ true 且 bReady=true：调用 HandlePacket()，继续处理下一个完整包
```

`readAll()` 只表示读取当前可读字节，不保证读到完整包。处理完一个包后继续循环，才能处理一次到达的多个粘连包；遇到半包返回，不清空缓存。

接管连接、关联信号后，额外检查 `bytesAvailable()` 并调用一次读取函数，处理接管期间已经存在的可读数据。槽函数不会因为写在 `slots` 下就自动运行在子线程；当前 Manager 已安排 `moveToThread()`，并通过线程的 `started` 信号调用 Worker 接管连接。

### 当前选择：不限制 Qt 内部读缓冲区

MyServer 当前未启用 `setReadBufferSize()`，使用默认值 0，即不设 Qt 内部读缓冲区上限；原调用保留为注释。它与半包/粘包处理是两件事，取消这个设置不意味着取消协议载荷长度检查。

十几 GB 是文件总大小，读缓冲区容量描述的是某一时刻积压的字节数，不需要把整个文件缓存到内存。不设上限后，若应用读取跟不上，缓冲区仍可能增长；是否设置及设置多大，可以在后续真实传输中比较速度与内存占用后再决定。

## HandlePacket() 的整体结构

```text
    ├─ Metadata：握手，回复 ResumeState，返回 true
    ├─ 检查当前连接是否已加入正确会话
    ├─ Data：接收分片，回复 ACK，返回 true
    ├─ CompleteRequest：合并校验，回复 Result，返回 true
    └─ 其他消息：返回 false
```

### 当新握手到来时的连接

```text
Worker 解析 Metadata
    ↓
Store::AttachSession()
    ↓
找到已有 Session，或创建并恢复 Session
    ↓
Session::BuildResumeStatePacket()
    ↓
Worker 通过 socket 返回续传状态
```

### 当来的是data时

```text
收到 Data
    ↓
确认连接已握手，任务 ID 正确
    ↓
Session 检查并写入所属通道文件
    ↓
Worker 报告进度、回复 ACK
```

## 三类消息的处理结果

| 收到的消息 | Session / Store 的工作 | Worker 回复 |
| --- | --- | --- |
| `MetadataPacket` | 解析并校验元信息；第一次通过 Store 加入会话，已有连接则核对任务和通道不能改变 | `ResumeStatePacket` |
| `DataPacket` | Session 检查通道归属、写入局部偏移并更新位图；重复片不重复计数 | 可以接受时回复 `AckPacket`，新分片才报告进度 |
| `CompleteRequestPacket` | 控制连接调用 Session 检查缺失；齐全时顺序合并、SHA-256 校验并生成正式文件 | `ResultPacket`，携带最终结果和缺失范围 |

Metadata 的 UUID 从包头补入，不在 JSON 中重复保存。重复 Metadata 不能再次 Attach 同一通道，而是核对既有身份并再次回复续传状态。其他消息必须已经握手，且包头 UUID 与当前会话一致；控制连接的 laneIndex 为 -1，不能用来发送 Data。

### 疑问：接收端在哪里区分协调连接和数据连接？

接收端接收的是 TCP 连接及其消息，不会直接识别发送端的 C++ 线程对象。Manager 为每条连接创建相同类型的 ReceiverWorker，由握手中的 `laneIndex` 声明角色，保存到 `stConnection.m_nLaneIndex`。

| 阶段 | 区分方式与代码位置 |
| --- | --- |
| Metadata 握手 | Worker 的 HandlePacket 解析 laneIndex、调用 Store 加入会话并保存编号；控制和数据连接都回复 ResumeState |
| Data 分片 | Worker 将保存的编号交给 Session::AcceptDataPacket；Session 拒绝负数编号，再检查当前通道的分片区间，成功接受后由 Worker 回复 ACK |
| CompleteRequest | Worker 的 HandlePacket 明确要求编号为 -1，再调用 Session 合并与校验，回复 Result；数据连接不能发起最终合并 |

因此不需要两种 Worker 类。相同接收框架通过“连接身份 + 消息类型”执行不同业务；控制连接不负责写入某个通道文件。

### 完成请求：两个成功含义不同

```text
CompleteRequest（仅控制连接）
    ↓ VerifyAndFinalizeSession()
得到 bSuccess、缺失范围或正式路径
    ↓ 先将 Result 加入 socket 发送队列
成功时再发出本地完成信号和日志
    ↓
HandlePacket() 返回 true，表示这条完成请求已按协议处理
```

| 判断 | 表达什么 |
| --- | --- |
| `bSuccess` | 文件是否完整、校验通过且正式文件生成成功 |
| `HandlePacket()` 返回值 | 本条消息是否已按协议处理；false 会让外层尝试回复 Error 并关闭连接 |

例如仍有缺失分片时，`bSuccess=false`，Result 告知缺失范围，但 `HandlePacket()` 仍返回 true，允许协议继续安排补传。完成请求分支末尾的 `return true;` 必须放在 `if (bSuccess)` 外面；否则发送 Result 后还会落入“消息类型不支持”的兜底，再发送 Error 并关闭连接。

`strResultMessage` 为成功和失败共用的 Result 准备描述；后面的 `if (bSuccess)` 只负责本地成功通知。两者分开可以共用一次 Result 发送，并保持“先排队回复对端，再通知本地”的顺序。`write()` 返回或加入发送队列，不表示对端已经收到结果。

### 关闭与错误的收尾

包头非法时直接 `abort()`；完整消息业务校验失败时，先尝试写入 Error，再调用 `disconnectFromHost()`，让排队的数据有机会发送后关闭，但不保证对端一定收到。

主动 Stop 先 `disconnect(this)` 解除 socket 发往 Worker 的信号槽连接，再 `abort()` 关闭 TCP，避免重入断开处理。对端断开时先解绑和通知结束，socket 留到配套的对象回收流程清理。清空 Session 指针后，析构中的再次解绑是空操作；通道真正退出前不能继续写文件。

文件处理保持在 Session，连接缓存保持在 Worker，Manager 管理接收线程、进度汇总和完成去重。

---

# `ReliableTransferManager`

> Manager 调度 TcpServer、Store、ReceiverWorker 和发送协调线程；文件处理交给 Session，对外提供启停和发送接口，并转发通知。

## 1. 注册类型名称的用法

```cpp
qRegisterMetaType<qintptr>("qintptr");
```

**作用是：让 Qt 在运行时知道，怎样保存、复制和销毁这种类型的参数。**

你可能会想：编译器不是已经认识 `qintptr` 了吗？对，但编译器认识类型，和 Qt 的运行时类型系统认识类型，是两回事。

例如跨线程发送信号：

```cpp
emit SignalConnectionAccepted(nSocketDescriptor);
```

如果采用队列连接，接收线程此时可能正在忙，Qt 就需要：

1. 复制 `nSocketDescriptor` 的值，保存到一个事件中。
2. 把事件放进接收线程的事件队列。
3. 接收线程处理事件时，取出参数，调用槽函数。
4. 处理完后，清理保存的参数。

**类型注册提供的信息，就是供 Qt 完成这些操作使用的。**

对于你的这句：

```cpp
qRegisterMetaType<qintptr>("qintptr");
```

`qintptr` 本身是 Qt 已支持的整数类型的别名，这里主要是让 Qt 能通过 **`"qintptr"` 这个名字**找到对应的类型信息。

你可以把它理解成：

> C++ 编译器负责“代码怎么编译”，Qt 类型注册负责“程序运行时，Qt 怎么识别和处理这个参数”。

普通函数调用、同线程直接连接一般不需要这一步；队列连接涉及运行时参数传递时，它才可能发挥作用。

当前 TcpServer 与 Manager 在同一线程，这条直接交付通常不依赖注册；后面的 started lambda 按值捕获描述符，也不等同于用队列信号参数传递 qintptr。注册不会复制 socket、创建线程或改变对象归属。

## 2. `StopListening()`：停止、等待与回收

### 为什么先修改 `m_nReceiverGeneration`？

例如上一轮监听的编号是 5，接收线程发出了进度信号，但主线程还没处理。此时停止监听，把编号改成 6。

当前进度转发时会比较：

```cpp
if (nGeneration != m_nReceiverGeneration)
{
    return;
}
```

旧通知携带的编号是 5，因此会被忽略，避免干扰新一轮监听。

### 为什么 `RequestStop()`、`quit()` 和 `wait()` 都需要？

- RequestStop() 修改共享标记，让正在执行的接收、文件合并代码发现停止请求，主动返回。
- quit() 请求线程的事件循环退出，不会强行打断正在执行的函数。
- wait() 等待线程真正结束，之后才能安全删除 QThread 对象。

先给所有线程调用 quit()，再逐个 wait()，可以让它们共同开始退出，减少串行等待。

### 原来的保存状态和关闭 socket 去哪里了？

这些工作已经交给 Worker 和 Store。当前 `OnIncomingConnection()` 已连接线程结束信号，形成这条清理链：

```text
接收线程结束
    → 释放 Worker
    → Worker 析构调用 Stop()
    → 关闭 socket，调用 DetachSession()
    → 最后一条连接离开时，尝试保存会话状态
```

Manager 负责组织停止与回收；状态保存由 Store/Session 执行。该清理链已写入代码，停止时的真实行为仍需联调验证。

## 3. `StartListening()`：每轮创建新的 Store

核心变化是这一句：

```cpp
m_pSessionStore.reset(
    new ReliableReceiveSessionStore(m_strOutputDirectory));
```

它创建一个新的 Store，由 m_pSessionStore 持有。后面每个新连接对应的 Worker，都会取得这个共享指针，因而可以访问本轮监听共用的会话登记表和停止标记。

为什么每轮重新创建？因为上一次调用 StopListening() 时，旧 Store 的停止标记已经设为 1。新 Store 会创建初始值为 0 的停止标记，让新一轮接收正常开始。磁盘上的续传文件仍然保留，后续由 Session 按需恢复。

还有两个顺序细节：

- 先校验参数，再停止旧监听：输入错误时，原来的监听可以继续运行。
- 先保存 errorString()，再清理：保留此次监听失败的原因。
当前 `OnIncomingConnection()` 已负责为新连接创建接收线程和 Worker。

## 4. 当前接收职责分工

| 内容 | 负责位置 |
| --- | --- |
| 一个文件的状态、创建和恢复 | Session |
| 分片校验、通道文件写入和位图 | Session::AcceptDataPacket |
| 状态持久化、缺失范围、串行合并和校验 | Session |
| socket 缓存、消息分发和协议回复 | ReceiverWorker |
| 查找共享会话、登记通道和解除关联 | SessionStore |
| 监听、接收线程、进度汇总和完成去重 | Manager |

字节编解码始终交给 Codec，Manager 不重复实现 JSON 或包头编码。

## 5. 半包和粘包：连接模块缓存，Codec 拆包

TCP 不保留每次 `write()` 的消息边界。新版仍采用**每条连接独立缓存，按包头声明的长度取完整包**的方式；协议格式是：

```text
一个完整协议包 = 62 字节固定包头 + m_nPayloadSize 字节载荷
```

当前 Manager 不再实现 `OnSocketReadyRead()` 或维护接收缓存；ReceiverWorker 按连接保存缓存，复用 Codec 的 `TryTakePacket()` 拆包。下面保留的是拆包原理示例。

```cpp
//示意：每条连接拥有自己的 arrReceiveBuffer。
arrReceiveBuffer.append(pSocket->readAll());

while (true)
{
    SReliablePacketHeader stHeader;
    QByteArray arrPayload;
    bool bReady = false;
    QString strErrorMessage;

    if (!ReliablePacketCodec::TryTakePacket(
            arrReceiveBuffer, stHeader, arrPayload,
            bReady, strErrorMessage))
    {
        pSocket->disconnectFromHost();
        return;
    }

    if (!bReady)
    {
        break; //半包：缓存保留，等待下一次可读通知。
    }

    //完整包：交给当前连接的消息处理函数，再继续取下一个包。
}
```

`readAll()` 只读取 socket 当前可读的数据，不保证读到完整包。`TryTakePacket()` 先检查包头，再判断“包头 + 载荷”是否足够；足够时用 `mid()` 取出载荷，用 `remove()` 移除当前包，其余字节继续保留。长度和载荷上限由 Codec 统一检查。

例如包 A 为 `62 + 20 = 82` 字节，包 B 为 `62 + 50 = 112` 字节：

| 次数 | 本次读到 | 处理结果 |
| --- | ---: | --- |
| 第一次 | 40 字节 | A 包头不完整，全部保留 |
| 第二次 | 52 字节 | 累计 92 字节，取出 A，留下 B 的前 10 字节 |
| 第三次 | 102 字节 | B 补齐，取出后缓存为空 |

`mid(pos, len)` 从 0 起算，返回新的字节数组，不直接修改原数组。`remove()` 删除缓存前部的数据，不会使已经取得的 `arrPayload` 失效。消息分发中 `switch` 的 `break` 只退出 switch，不会结束外层拆包循环。

## 6. 保留的 Qt 连接与线程学习笔记

以下保留早期代码中的 Qt 学习例子，代码块不全部代表当前 Manager 的成员。当前监听指针为 `ReliableTcpServer *`；接收缓存位于 Worker；线程停止逻辑已集中到 `StopListening()` 和 `StopSending()`。

### 6.1 类的前置声明

在.h文件中

例如：

```cpp
class QTcpServer;
```

它没有定义 `QTcpServer` 的成员和功能，但足够让你声明一个指向它的指针：

```cpp
QTcpServer *m_pTcpServer;
```

早期 `ReliableTransferManager.h` 中，这几个类用于声明指针；当前监听类型已改为 `ReliableTcpServer *`：

```cpp
QTcpServer *m_pTcpServer;
QThread *m_pSenderThread;
ReliableSenderWorker *m_pReliableSenderWorker;
```

编译器知道指针占多大空间，因此此处不必了解这些类的完整定义。

### 6.2 每条连接独立的接收缓存

> 下面的 `QHash` 是旧 Manager 写法，保留用于理解。当前每个 Worker 使用自己的 `m_stConnection.m_arrReceiveBuffer`。

```cpp
//    它是“每一个 TCP 连接各自的接收缓存”。
//    键和值分别是：
//    Key：   QTcpSocket*，某个已连接客户端
//    Value： QByteArray，该客户端尚未解析完的原始字节
    QHash<QTcpSocket*,QByteArray> m_mapReceiveBuffer;
```

### 6.3 socket 的信号断开、关闭和释放

```cpp
pSocket->disconnect(this);
pSocket->close();
pSocket->deleteLater();
```

断开连接的这三步的作用分别如下：

- `pSocket->disconnect(this);`断开 `pSocket` 发给当前 `ReliableTransferManager` 的信号槽连接。
- `pSocket->close();`真正关闭这条 TCP 连接，断开对端。
- `pSocket->deleteLater();`安排 Qt 在下一次事件循环中删除 `QTcpSocket` 对象。

不用立即 `delete pSocket`，是因为它可能正处于信号槽调用过程中；延迟删除更安全，避免对象被立刻销毁后仍有代码访问它。

### 6.4 线程的退出与回收

```cpp
    if(m_pSenderThread != nullptr)//如果发送线程对象已经创建
    {
        m_pSenderThread->quit();
        m_pSenderThread->wait();
        delete m_pSenderThread;
        m_pSenderThread = nullptr;
        m_pReliableSenderWorker = nullptr;
    }
```

```cpp
m_pSenderThread->quit();
```

请求线程的事件循环退出。

这里需要特别注意：`quit()` 不会强行中断正在执行的 `ProcessSend()`。如果 Worker 正在阻塞等待或执行发送循环，需要等这个函数返回，线程才能完成退出。

```cpp
m_pSenderThread->wait();
```

让当前线程等待，直到发送线程真正结束。

这样可以避免后台线程还在运行，就把相关对象销毁。

不过，如果发送函数一直不返回，`wait()` 也可能一直等待；如果当前是 UI 主线程，界面就会卡住。因此后续需要给发送任务设计取消和退出机制。

```cpp
delete m_pSenderThread;
```

线程已经结束，现在释放 `QThread` 对象占用的内存。

要区分：

线程执行结束 ≠ `QThread` 对象自动销毁

```cpp
m_pSenderThread = nullptr;
m_pReliableSenderWorker = nullptr;
```

把成员指针清空，避免保留指向已释放对象的地址。

赋值为 `nullptr` 本身不会释放对象。

你可能还会疑惑：为什么这里没有 `delete m_pReliableSenderWorker`？

这种清理方式需要在线程创建时配套连接；后续接入发送线程时要核对：

```cpp
connect(m_pSenderThread, &QThread::finished,
        m_pReliableSenderWorker, &QObject::deleteLater);
```

在线程结束与 deleteLater 正确配套的情况下，由这个连接安排 Worker 的释放，不再重复手动删除它。

这段代码的清理顺序就是：

```text
停止接收监听
     ↓
请求发送线程退出
     ↓
等待发送线程真正结束
     ↓
释放线程对象
     ↓
清空成员指针
```

### 6.5 sender() 找到信号来源

```cpp
 //在槽函数里，找出本次发出信号的那个 socket，并把它转换成 QTcpSocket *。
    //sender()能取得发出信号的对象。但它返回的是通用类型：QObject *
    QTcpSocket *pSocket = qobject_cast<QTcpSocket *>(sender());
```

### 6.6 静态封包函数为什么可以直接调用

因为 `BuildErrorPacket()` 在类里被声明为 `static` 静态成员函数：

普通成员函数需要通过对象调用：

```cpp
ReliablePacketCodec codec;
codec.SomeFunction();
```

而静态成员函数属于类，不依赖某个具体对象，所以可以直接：

```cpp
QByteArray packet = ReliablePacketCodec::BuildErrorPacket(
    stMetadata,
    QStringLiteral("分片序号越界"));
```

---

## 7. `OnIncomingConnection()`：一条连接对应一个接收 Worker

当前流程：检查 Store 与线程总数 → 创建 QThread/Worker → `moveToThread()` → 连接信号 → 登记并启动线程。Worker 在自己的接收线程中创建 QTcpSocket 并接管描述符。线程名称中的连接编号不等于 `laneIndex`，数据通道身份要等 Metadata 握手后才能确定。

### 线程总数限制是资源策略

```cpp
g_nReliableMaxReceiverThreadCount =
    g_nReliableReceiverCapacityMultiplier
    * (g_nReliableMaxThreadCount + 1);
```

容量倍数当前为 2；每个任务最多 32 条数据连接，另有 1 条控制连接，合计预留 66 个连接线程的容量。这里限制的是整个接收端的线程总数，不是严格限制“最多两个文件任务”。超过上限时关闭新连接，现有连接继续工作。

### 信号可以连接到信号

Worker 的日志信号连接 Manager 的日志信号，用于转发同一条消息，界面只需连接 Manager。默认 AutoConnection 下，Worker 向主线程 Manager 发出的通知通常通过队列交付；信号转发不会自行创建线程。

### `QPointer` 与 `removeAll()`

`QPointer<QThread>` 观察的是 QThread 对象是否仍存在，不是后台线程是否还在运行。对象被删除后，各份 QPointer 会自动变为空；普通指针在 `delete` 后仍可能保存旧地址，lambda 按值捕获的副本也不会因外部指针置空而改变。

```cpp
m_arrReceiverThreads.removeAll(pGuard.data());
pGuard->deleteLater();
```

`removeAll(value)` 只移除所有等于指定指针的列表记录，不会清空其他线程记录，也不会删除指针指向的对象；后面的 `deleteLater()` 才安排释放对象。QPointer 让延迟执行的 finished 回调能避开已被 StopListening 删除的线程对象。

### 多线程进度是共享总数的快照，不能相加

同一文件的各 Worker 共享一个 Session。A 报告 100，B 报告 101，表示“同一个文件先后达到 100、101”，不是各自接收 100 和 101。若两者确实分别接收了 100 和 101，总数才是 201。

| 情况 | Manager 的处理 |
| --- | --- |
| 旧轮次的排队进度或完成通知 | 比较 `nGeneration`，不匹配就忽略 |
| 同一任务先收到 101、后收到 100 | 用 `qMax()` 保留已转发的最大数量，避免进度倒退 |
| 重复完成通知 | `m_setCompletedSessions` 按 UUID 去重 |
| 已完成任务随后又到达进度 | 忽略，避免完成后继续刷新进度 |

## 8. `StopSending()` 与发送接入顺序

当前已写出：`requestInterruption()` → `quit()` → `wait()` → 删除 QThread → 清空成员指针。析构函数依次调用 `StopSending()` 和 `StopListening()`。

`requestInterruption()` 只提出取消请求，发送代码需主动检查；`quit()` 请求事件循环退出，不会打断正在执行的发送函数。当前 StartSend 已连接 `finished → Worker::deleteLater`，仅将 Worker 指针置空不会释放 Worker。

**当前 StartSend 已接入界面并参与整工程构建与本机联调。** Worker 通过 moveToThread 放到后台协调线程，started 信号在该线程触发 ProcessSend；界面经 Controller 调用 Manager 的发送入口。

## 9. `StartSend()`：线程如何创建和启动？

> 主线程中的 Controller 和 MainWidget 负责启动任务与显示结果，耗时工作由后台协调线程和数据线程执行。

**Manager::StartSend 本身在主线程执行。** 它检查参数、创建对象、建立信号连接并调用 start；真正的文件摘要计算、网络等待和通道协调交给后台 ProcessSend。函数名、slots 和 new Worker 都不会自动切换线程。

### 9.1 主线程完成哪些事情？

| 当前代码 | 执行线程 | 具体含义 |
| --- | --- | --- |
| 检查 isRunning、文件基本信息、IP、端口和配置 | 主线程 | 拒绝重复任务或非法参数，不在这里通读文件计算 SHA-256 |
| StopSending 清理上一任务 | 主线程 | 已排除仍在运行的任务，通常用于回收上次结束后保留的 QThread 对象 |
| new QThread(this) | 主线程 | 创建线程管理对象，此时还没有启动新的执行线程 |
| new ReliableSenderWorker | 主线程 | 构造 Worker 对象，此时最初归属于主线程；构造函数只初始化，不开始发送 |
| moveToThread(m_pSenderThread) | 主线程调用 | 改变 Worker 的线程归属，未来事件和队列调用由目标线程处理，不会重新执行构造函数 |
| connect(...) 与参数按值捕获 | 主线程 | 注册连接、保存回调及其参数；此刻不执行 lambda 函数体 |
| m_pSenderThread->start() | 主线程调用 | 请求 Qt 启动新线程；启动后主线程继续往下执行 |
| return true | 主线程 | 表示任务已发起，最终结果由 SignalSendFinished 通知 |

后台可能在 StartSend 返回前就开始执行，不能依赖“先 return，再开始发送”的固定先后顺序。按值捕获路径、IP 和配置，是为了让 StartSend 返回后，后台仍能使用这些参数。

### 9.2 QThread 对象与它管理的执行线程不是一回事

```cpp
m_pSenderThread = new QThread(this);
pWorker->moveToThread(m_pSenderThread);
```

QThread 管理对象由主线程创建，QObject 归属仍在主线程；它代表的后台执行线程在 start 后运行。Worker 从主线程移到该后台线程，不是把 QThread 对象一起移过去。

Worker 不设置 Qt 父对象，才能 moveToThread；如果创建为 new ReliableSenderWorker(this)，就不能直接移走这个带父对象的 Worker。Manager 仍留在主线程，后台结果通过信号传回。

### 9.3 started 怎样让 ProcessSend 在后台执行？

```cpp
connect(m_pSenderThread, &QThread::started,
        pWorker,
        [pWorker, strAbsoluteFilePath, strEffectiveTargetIp,
         nPort, nThreadCount, nWindowSize, nSegmentSize]()
{
    pWorker->ProcessSend(
        strAbsoluteFilePath, strEffectiveTargetIp,
        nPort, nThreadCount, nWindowSize, nSegmentSize);
});

m_pSenderThread->start();
```

上面摘录了启动连接的关键部分，参数值在此前已准备好。关键在于：started 从新启动的线程发出；连接的上下文是 pWorker，而它已经归属于同一后台线程。默认 AutoConnection 在这里直接于该后台线程执行 lambda，lambda 再普通调用 ProcessSend，所以 ProcessSend 也在后台执行。

不是 started 这个名称自动让任何函数进入后台，也不是捕获 pWorker 就能切换线程。若将上下文写成主线程里的 this（Manager），默认连接会把 lambda 排到主线程，耗时发送就可能阻塞界面。

```cpp
//即使 Worker 已 moveToThread，这种从主线程直接调用仍在主线程执行。
pWorker->ProcessSend(/* 参数 */);
```

普通 C++ 成员调用由调用者当前所在的执行线程决定；moveToThread 影响对象归属与事件派发，不会把每一次普通调用自动改成跨线程调用。

### 9.4 Sender 协调线程和 Lane 数据线程各处理什么？

| 线程 | 主要工作 |
| --- | --- |
| 主线程 | MainWidget、Manager、按钮响应、StartSend 的启动准备；接收进度与结果信号后更新界面 |
| Sender 协调线程 | ProcessSend、BuildMetadata 文件读取与 SHA-256、控制连接握手与最终 Result、恢复共享位图、创建 Lane 对象、检查取消、等待退出、汇总快照、回收 Lane 对象 |
| N 个 Lane 数据线程 | 各自 run 中创建/使用文件句柄和数据 socket，读取所属分片、窗口发送、处理 ACK、超时补传和自身重连 |

Lane 的 QThread 对象在协调线程里 new，因此对象最初归属于协调线程；pLane->start() 后，其重写的 run 才在新的数据线程执行。与 Sender 的情况一样，不能把“对象在哪里创建”和“run 在哪里执行”混为一谈。

```text
主线程：StartSend → 创建对象、moveToThread、connect → start → 返回，继续处理界面事件
                                                     ↓
Sender 协调线程：started → lambda → ProcessSend → 哈希、控制连接和通道调度
                                                     ↓
Lane 数据线程：                                 各自 run 并行发送

Sender 汇总进度与结果 → 队列通知主线程的 Manager → 界面槽更新控件
```

### 9.5 信号在哪里转发，为什么这里使用 DirectConnection？

Worker 从 Sender 线程发出日志、进度或结果，接收上下文是主线程的 Manager，默认 AutoConnection 在这里成为队列连接。Manager 在主线程转发信号，界面再执行更新；后台线程不直接操作控件。

StartSend 中显式使用的 DirectConnection 是“Worker 完成信号 → QThread::quit”。直接连接让 quit 在发信号的 Sender 线程执行，即使 QThread 管理对象归属主线程。这是因为 quit 本身允许线程安全调用；不能据此把其他跨线程 socket 或 GUI 操作都改成直接连接。

这样即使主线程正在 StopSending 的 wait 中，退出请求也不用等待主线程先处理队列。quit 仅请求事件循环退出，正在执行的 ProcessSend 仍要自行返回；取消由 requestInterruption 和共享原子标记配合检查。

#### 补充：其他成员函数必须使用 AutoConnection 吗？

不一定。槽函数的执行位置由连接方式决定；槽函数本身没有独立的线程归属，这里说的是“接收对象所属的线程”。

| 连接方式 | 槽函数在哪里执行？ |
| --- | --- |
| `Qt::AutoConnection`，默认方式 | 发信号的当前线程与接收对象归属线程相同，立即直接执行；不同则排队，由接收对象所属线程执行 |
| `Qt::QueuedConnection` | 始终排队，由接收对象所属线程的事件循环执行；即使同线程也不会立即调用 |
| `Qt::DirectConnection` | 立即在发出信号时的当前线程执行，不自动切换到接收对象所属线程 |

AutoConnection 比较的是**此刻发信号的执行线程**与接收对象的归属线程，不是简单比较发送对象和接收对象的 thread()。发送对象即使归属主线程，若某次信号实际从后台线程发出，也要按后台线程判断。

普通跨线程业务调用通常使用默认 AutoConnection；明确要求排队时，也可以使用 QueuedConnection。队列调用要求接收线程有事件循环，并且能处理事件：若线程一直停留在耗时 ProcessSend 中，排队的停止槽不能自动打断它，所以当前仍用中断请求和共享原子标记检查取消。

**DirectConnection 不会自动提供线程安全保障，但不代表它一定不安全：**

- 同一线程内直接执行，是正常且常见的方式。
- 跨线程直接执行时，如果访问接收对象的非线程安全状态，就可能出现问题。
- 明确允许跨线程安全调用的接口，例如 QThread::quit，可以这样调用。
- GUI 更新仍必须在主线程，不能靠加锁后从后台直接调用控件函数。

当前例子中 pWorker 归属 Sender 线程，m_pSenderThread 指向的 QThread 管理对象归属主线程。Worker 在后台发完成信号：若用 AutoConnection，quit 会排到主线程；显式 DirectConnection 则立即在后台调用 quit。无论 quit 在哪个线程被调用，请求退出的都是该 QThread 管理的后台事件循环，不是调用者所在的主线程事件循环，也不是强制终止正在执行的 ProcessSend。

**记忆：DirectConnection 决定“立即在发信号的当前线程执行”；是否安全，要看槽具体做什么。QueuedConnection 决定“排队交给接收对象所属线程”；是否及时执行，要看该线程能否处理事件。**

### 9.6 结束与回收：为什么通常不卡界面，停止时仍可能等待？

正常结束顺序：各 Lane 已退出并由协调者回收 → Worker 发出最终信号、请求协调线程 quit → ProcessSend 返回 → 协调线程结束 → finished 触发 Worker::deleteLater，在结束清理阶段释放 Worker。QThread 管理对象保留给下一次启动前或 Manager 析构时的 StopSending 回收。

ProcessSend 中的 wait(10) 阻塞的是 Sender 协调线程，主线程仍可处理界面事件。但如果从主线程执行 StopSending，其中无超时的 wait 会让主线程等待后台退出；遇到较慢的阻塞操作，停止期间仍可能暂时卡顿。后台发送不会自动保证任何主线程操作都绝不卡顿，后续关闭和取消验收需要检查这条路径。

StartSend 里的 m_pReliableSenderWorker 是保存对象地址的指针，不负责自动释放。Worker 通过 finished/deleteLater 释放后，不应再用旧指针调用函数；当前后续 StopSending 只清空这个成员，不再解引用它。

---

# `TransferController.h/.cpp`

Controller 为界面提供统一入口，将 SetOutputDirectory、StartReceiver、StopReceiver、StartSend 和 StopSending 转交 Manager；将日志、发送进度、发送结果、接收进度和接收完成五类信号转发给 MainWidget。

MainWidget、Controller 和 Manager 都归属主线程。Controller 不创建线程、不计算文件摘要、不重复统计进度；后台工作由 Manager 启动的 Worker 和 Lane 执行。

## 父子对象与默认析构

```cpp
TransferController::~TransferController() = default;
```

表示使用编译器生成的默认析构行为，没有手写额外清理逻辑。仍会析构成员对象并调用基类析构函数。Controller 创建 Manager 时传入 this，QObject 基类析构会删除子对象，继而执行 Manager 的停止与回收逻辑。

```text
MainWidget → 子对象 Controller → 子对象 Manager
```

普通指针成员本身不会自动 delete 所指对象；这里能释放 Manager，依靠的是 QObject 父子关系。= default 不会给普通指针增加所有权管理。

## Worker 对象删除后，普通指针为什么还可能非空？

Manager 的 m_pReliableSenderWorker 只保存 Worker 地址。线程 finished 连接 Worker::deleteLater，Worker 删除后，该普通指针不会自动变为 nullptr，可能暂时成为悬空指针。

- `pWorker = nullptr`：清除保存的地址，不删除对象。
- `pWorker->函数()`：访问所指对象，要求对象仍然存在；只检查非空不能证明对象还活着。
- 当前 StopSending 操作 QThread 的中断、quit 和 wait，等待结束后只清空 Worker 成员，不通过旧 Worker 指针调用函数或重复删除它。

---

# `mainwidget.h/.cpp`：界面接入

## 两个独立参数

| 控件 | 含义 | 当前默认值 / 上限 |
| --- | --- | --- |
| m_pThreadCountSpinBox | 请求的数据发送线程数，不含协调线程；小文件可能减少实际通道数 | 4 / 32 |
| m_pWindowSizeSpinBox | 每条数据连接允许等待业务 ACK 的分片数 | 16 / 128 |

例如实际 4 条数据连接、每通道窗口 16，合计最多约 64 个在途分片，另有一条控制连接；不是 64 个线程。

## 按钮与通知

- 启动接收：读取监听 IP、端口与输出目录 → Controller → Manager；返回成功后才切换监听按钮和状态。监听期间禁用监听 IP 和端口输入框。
- 停止接收：调用 StopReceiver，等待后台接收线程退出，再恢复按钮和状态。当前主线程可能短暂等待。
- 发送文件：读取路径、目标地址、端口、线程数、窗口、分片大小 → 检查界面参数 → 禁用发送按钮并显示准备状态 → Controller::StartSend。
- StartSend 返回 false：尚未启动任务，当前槽恢复按钮并显示原因；返回 true：后台任务已发起，哈希、连接或传输的后续结果通过 SignalSendFinished 交给 OnSendFinished。
- 日志、进度和最终结果由已连接的五类信号更新界面；后台线程不直接操作控件。

GetSegmentSizeBytes 先用 qint64 计算数值乘以 KB/MB 倍率，确认大于 0 且不超过 4 MiB 后再转 int；非法时返回 0。避免先窄化转换后才发现超限。小于 8 KiB 的正数当前仍会由元信息构建逻辑调整为至少 8 KiB。

构造函数初始化列表按头文件中成员声明顺序书写；窗口指针紧跟在线程数指针后，保持声明与初始化顺序一致。输出目录在构造时、选目录后及启动监听前同步给 Controller；监听已启动后更改目录用于后续新一轮监听。

当前取消逻辑已有 StopSending 接口和后台检查点，界面尚未提供“取消发送”按钮。不要把接口存在等同于按钮已接通。

---

# `ReliableSha256.h/.cpp`：跨平台 SHA-256 封装

> 文件摘要统一使用 ReliableSha256：Windows CNG、Linux OpenSSL EVP、其余平台 Qt。当前主工程为串行合并，没有合并计时或独立摘要线程。

## 摘要封装与业务调用位置

| 文件 | 改动 |
| --- | --- |
| ReliableSha256.h | 统一声明 Initialize、AddData、Finish，业务代码不直接持有 Windows 句柄 |
| ReliableSha256.cpp | Windows CNG、Linux EVP、其余平台 Qt；独占状态并检查返回值和释放资源 |
| ReliableSenderWorker.cpp / BuildMetadata | 文件读取前 Initialize，每读一块 AddData，读完 Finish，再转十六进制；保持 1 MiB 块大小、取消检查和文件大小校验 |
| ReliableReceiveSession.cpp / VerifyAndFinalizeSession | 同样替换摘要调用，仍在顺序合并时计算，保持 4 MiB 缓冲区、QSaveFile 提交及正式文件生成流程 |
| MyServer.pro | 加入摘要类，Windows 链接 BCrypt，Linux 通过 pkg-config 查找和链接 libcrypto |

代码标有 `【CNG SHA-256】`。算法仍是标准 SHA-256，结果为 32 字节原始摘要，转为原来 64 个小写十六进制字符；没有改成 MD5，也没有改变协议版本、字段、SessionKey 规则和接收状态格式。Codec 对短身份字符串的 Qt SHA-256 计算保留，它不是整文件计算热点。

建议阅读顺序：**头文件接口 → SHashState 的状态与生命周期 → Initialize → AddData → Finish → 两处业务调用 → .pro 链接配置**。

## 三个接口怎样配合？

```cpp
ReliableSha256 oShaHash;
if (!oShaHash.Initialize(strErrorMessage))
{
    return false;
}
//循环读取文件，每次都检查 AddData 的返回值。
if (!oShaHash.AddData(arrBuffer.constData(), arrBuffer.size(), strErrorMessage))
{
    return false;
}
QByteArray arrDigest;
if (!oShaHash.Finish(arrDigest, strErrorMessage))
{
    return false;
}
const QString strSha256 = QString::fromLatin1(arrDigest.toHex());
```

Initialize 打开 SHA-256 算法提供者、检查摘要长度、创建摘要句柄；AddData 把本批数据追加到同一摘要状态，不保存整份文件；Finish 获取摘要并缓存。完成后再追加数据会被拒绝，重复 Finish 返回同一结果；重新 Initialize 开始另一轮计算。

头文件中的 SHashState 只有前置声明，实际定义在 .cpp。QScopedPointer 独占状态，对象离开作用域自动释放；Windows 下先 BCryptDestroyHash，再 BCryptCloseAlgorithmProvider。类禁止复制，避免两个对象重复关闭同一批句柄。每次文件计算使用独立对象，同一对象按顺序在所属调用线程使用，没有额外创建摘要工作线程。

CNG 错误由 NTSTATUS 返回，代码检查 nStatus < 0 并生成操作名称与十六进制状态码；不使用 GetLastError 解释 CNG 失败。Windows 调用失败直接返回错误，不会静默跳过校验或自动降级到 Qt。

## 关键概念

### 1. 动态库、API、句柄和上下文不是同一个层次

| 概念 | 当前例子 | 含义 |
| --- | --- | --- |
| 系统动态库 | Windows 的 bcrypt.dll | 提供 CNG 功能代码，不是某次计算的状态 |
| API | BCryptOpenAlgorithmProvider、BCryptHashData 等 | 操作资源的函数接口 |
| 算法提供者句柄 | m_hAlgorithm | 标识已打开的 SHA-256 算法提供者 |
| 摘要状态句柄 | m_hHash | 标识本次具体的增量摘要计算状态 |
| EVP 上下文指针 | m_pOpenSslContext | 指向 OpenSSL 管理的本次摘要上下文 |

```cpp
BCRYPT_ALG_HANDLE m_hAlgorithm = nullptr;
```

BCRYPT_ALG_HANDLE 是 Windows SDK 定义的类型名，m_hAlgorithm 是我们命名的变量；声明时仍为空。API 调用成功后，把实际句柄值写入变量。传 `&m_pState->m_hAlgorithm` 是传这个句柄变量的地址，让 API 可以修改它。

句柄是访问资源的不透明标识，不等于动态库，也不能简单理解成一定是整数编号；它在类型上可能像指针，但不应直接解引用。EVP_MD_CTX 也属于不透明类型，拿到指针不代表能直接访问其内部字段。当前可对应理解的是 m_hHash 与 m_pOpenSslContext，它们都让多次更新接着同一份状态继续；m_hAlgorithm 是另一级算法提供者。

资源必须用对应接口释放：CNG 摘要用 BCryptDestroyHash、提供者用 BCryptCloseAlgorithmProvider；EVP 上下文用 EVP_MD_CTX_free，不用 delete，也不混用其他句柄的关闭函数。

### 2. 类型转换与输出参数：地址、值和空间大小

```cpp
reinterpret_cast<PUCHAR>(&nDigestSize)
```

PUCHAR 相当于 unsigned char*。这里把 ULONG* 重新解释为字节缓冲区指针，仍指向同一个变量的存储区域；转换本身不改变整数值、不转成字符串，也不分配或复制内存。真正写入结果的是后面的 API。reinterpret_cast 是底层类型转换，能编译不等于任意解引用都正确；这里查询属性确实返回 ULONG，且提供了对应大小的空间。

Windows 查询 BCRYPT_HASH_LENGTH 时：

| 名称 | 正常值 | 实际含义 |
| --- | ---: | --- |
| nDigestSize | 32 | 查询得到的摘要长度，单位字节 |
| sizeof(nDigestSize) | 4 | Windows ULONG 变量本身的空间大小 |
| nResultSize | 4 | 本次属性查询实际写入输出位置的字节数 |

因此“答案是 32”和“用 4 字节整数存这个答案”不矛盾。这里查询的不是文件大小，也不是内部摘要状态的内存大小。

AddData 的 CNG 输入转换还包含 const_cast，用来匹配未声明 const 的 API 指针类型；BCryptHashData 不修改输入内容。不要由此推断 const_cast 后就可以随意修改原本只读的对象。长度在先确认非负后，分别转为 CNG 要求的 ULONG 或 EVP 要求的 size_t。

### 3. 初始化的顺序与资源生命周期

SHashState 在头文件前置声明、在 .cpp 完整定义；`struct ReliableSha256::SHashState` 表示定义这个类内部的结构体，`::` 不是继承。

构造函数中的 m_pState 最初为空；Initialize 的 `reset(new SHashState)` 创建新状态并接管它，已有的旧状态会被释放。QScopedPointer 为独占所有权，Q_DISABLE_COPY 禁止复制，避免重复关闭同一批句柄；它不是多连接共享 Session 时使用的 QSharedPointer。

| 平台 | 初始化顺序 |
| --- | --- |
| Windows | 打开算法提供者 → 查询并检查 32 字节摘要长度 → 创建本次摘要状态 |
| Linux | 创建 EVP_MD_CTX → 使用 EVP_sha256() 选择并初始化算法 → 检查摘要长度 |

EVP_sha256() 提供算法描述，不会开始读取文件或计算文件内容。EVP_MD_CTX_size 返回当前算法的摘要输出长度，不是上下文自身的内存大小。

BCryptCreateHash 中第一组 nullptr/0 让 CNG 分配内部状态内存并在 DestroyHash 时回收（Windows 7 起支持），不是“不需要内存”；第二组 nullptr/0 表示不提供密钥，当前计算普通 SHA-256 而非 HMAC。所有准备步骤成功后才将 m_bInitialized 置 true。

CNG 检查 NTSTATUS 是否为负；EVP_DigestInit_ex、EVP_DigestUpdate、EVP_DigestFinal_ex 检查是否返回 1；EVP_MD_CTX_new 则检查指针是否为空，不能混用成功判断方式。ERR_clear_error 清理的是当前线程的 OpenSSL 错误队列，strErrorMessage.clear 清理的是我们自己的字符串。

### 4. AddData 与 Finish 的状态和缓冲区约定

AddData 必须在初始化后、完成前调用；多个块按顺序更新同一状态，不是分别计算后拼接摘要。不保存整份文件，也不保存 pData 供以后异步读取，当前调用返回后可复用输入缓冲区。长度为 0 允许空地址并直接成功；长度为正时不能传空地址。

状态检查中的 `m_pState.isNull() || ...` 利用短路求值，指针为空时不会继续解引用。底层更新或完成失败会将状态标记为无效；仅输入参数校验失败不会偷偷追加数据。

Finish 在已有状态上完成收尾，不重新读取整份文件；它也不负责释放全部计算资源。成功后 m_bInitialized 仍为 true，m_bFinished 为 true，内部 m_arrDigest 缓存结果；再次 Finish 返回缓存，但再次 AddData 被拒绝。重新 Initialize 才开始下一轮。

| 完成接口 | 输出缓冲区相关参数 |
| --- | --- |
| BCryptFinishHash | 调用方传入缓冲区地址和长度；当前 SHA-256 准备 32 字节 |
| EVP_DigestFinal_ex | 调用方事先准备足够空间，最后一个参数输出实际摘要长度，不是输入容量 |

Linux 当前先分配 EVP_MAX_MD_SIZE，再检查实际 nDigestSize 为 32，并将 QByteArray 长度收缩为 32，避免 toHex 时把多余尾部也算进去。Finish 的 arrDigest 是调用方输出变量，m_pState->m_arrDigest 是类内部缓存；二者不要混淆。32 字节摘要是 256 位，转十六进制后是 64 个字符，不是“64 位摘要”。

### 5. CNG 为什么更快：不要归因于“直接进内核”

SHA-256 的速度取决于所用计算后端。当前 Qt 5.14.2 源码路径使用内置 RFC6234 C 实现；CNG 使用 Windows 的算法实现，OpenSSL 使用自己的优化实现，可能利用汇编和受支持的 CPU SHA 指令扩展。没有跟踪本机实际执行的具体指令，不能把全部加速确定归因于某条硬件指令。

调用系统 DLL 不等于每次操作都进入内核。当前是普通应用调用 bcrypt.dll 的用户态接口；CNG 另外支持内核态调用，但“进内核”本身不是加速原因。函数封装层数也不是这次差距的主要来源：新增 ReliableSha256 封装和检查后依然更快，主要差别在大块数据的底层运算。

本机早期 256 MiB 内存小基准中，CNG 约快 10 倍，仅代表该环境下纯摘要运算，不代表总传输加速 10 倍，也不能推广成“Qt 都慢”“系统 API 都快”或“Linux 会有相同提升”。算法和输出保持相同，磁盘读取、合并写入等成本依然存在。

参考：[BCryptHashData 的输入与用户态/内核态说明](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcrypthashdata)、[OpenSSL CPU 能力与 SHA 扩展](https://docs.openssl.org/3.5/man3/OPENSSL_ia32cap/)。

## 是不是只能在 Windows 运行？

**CNG 本身是 Windows API；当前封装源码并不只写了 Windows 路径。**

```cpp
#ifdef Q_OS_WIN
//Windows：Windows 头文件、BCrypt 句柄和 API。
#elif defined(Q_OS_LINUX)
//Linux：OpenSSL EVP 上下文和接口。
#else
//其余平台：QCryptographicHash::Sha256。
#endif
```

这是编译期选择，不是运行时自动切换：在 Linux 编译时，Windows 头文件、句柄和 BCrypt 调用不参与编译。头文件先包含 Qt 类型，使 Q_OS_WIN 平台宏在 .cpp 判断前已经可见。

```qmake
win32: LIBS += -lbcrypt
```

qmake 的 win32 条件覆盖 Windows 32/64 位；Linux 不会加这项库，也不需要复制 bcrypt.dll。当前 Windows 分支仍使用 CNG；Linux 分支已经改为 EVP，但不能将 Windows CNG 或 Windows 上 EVP 的性能结果直接套用到 Linux。

## Linux 应该怎样做？

1. 在 Linux 上准备匹配的 Qt 5 开发环境、C++17 编译器、make、pkg-config 和 OpenSSL 开发包（1.1.1 或 3.x），包含工程使用的 Core、Widgets、Network 模块及 libcrypto.pc。
2. 复制源码，在 Linux 创建独立构建目录，进入后用 Linux 的 qmake 读取 MyServer.pro，再执行 make -j4；不要复用 Windows 的 Makefile、.o 或 EXE。
3. 生成 Linux 可执行文件，处理 Linux 的 Qt 动态库及平台插件部署，先做摘要一致性、小文件收发和取消测试。Windows EXE 及 windeployqt 发布包不能直接作为 Linux 原生程序运行。

Linux OpenSSL 后端现已实现：创建 EVP_MD_CTX、调用 EVP_DigestInit_ex，循环 EVP_DigestUpdate，最后 EVP_DigestFinal_ex，析构时 EVP_MD_CTX_free。每步检查返回值和结果长度，使用当前线程的 OpenSSL 错误队列报告失败；完成缓存、禁止完成后追加和重新初始化语义与 CNG 保持一致。

```qmake
linux {
    CONFIG += link_pkgconfig
    PKGCONFIG += libcrypto
}
```

Windows CNG 与 Linux OpenSSL 都计算标准 SHA-256。当前主工程采用串行合并；流水线实验及结果见文末。Linux 原生构建和跨系统收发尚待验收。

参考：[Microsoft CNG 摘要流程](https://learn.microsoft.com/en-us/windows/win32/seccng/creating-a-hash-with-cng)、[OpenSSL EVP 摘要接口](https://docs.openssl.org/3.5/man3/EVP_DigestInit/)。

---

# `ReliableSenderWorker`

## 1. 分片数量：向上取整与当前写法

> 下面保留原来的加法公式推导；当前实现改用“商 + 是否有余数”，避免加法中间值溢出，详见下一节。

```cpp
stMetadata.m_nTotalPacketCount = static_cast<quint32>((stMetadata.m_nFileSize + stMetadata.m_nSegmentSize -1) / stMetadata.m_nSegmentSize);
```

因为要计算**装下整个文件需要多少个分片**，最后不足一个分片的部分，也要算一个。所以这里需要**向上取整**。

```cpp
(文件大小 + 分片大小 - 1) / 分片大小
```

C++ 的整数除法会舍去小数。例如，简化为文件 10 字节、每片 4 字节：

```text
第 0 片：4 字节
第 1 片：4 字节
第 2 片：2 字节
```

实际需要 3 片，但直接计算：

```cpp
10 / 4 // 结果为 2，少算了最后一片
```

加上 `分片大小 - 1` 后：

```cpp
(10 + 4 - 1) / 4
= 13 / 4
= 3
```

**如果文件大小恰好整除，也不会多算：**

```cpp
(12 + 4 - 1) / 4
= 15 / 4
= 3
```

对应关系如下：

| 文件大小 | 每片大小 | 所需分片数 |
| ---: | ---: | ---: |
| 2 字节 | 4 字节 | 1 |
| 10 字节 | 4 字节 | 3 |
| 12 字节 | 4 字节 | 3 |

外面的：

```cpp
static_cast<quint32>(...)
```

只是把计算结果转换成 `quint32`，与向上取整无关。这个公式的前提是文件大小非负、分片大小大于 0，并且计算结果没有超出类型范围。

## 2. 当前 SenderWorker 的职责

- 文件摘要统一使用 SHA-256，业务代码通过 ReliableSha256 调用平台后端。
- 确定实际通道数：例如请求 8 个线程，但文件只有 3 个分片，就使用 3 个数据通道。
- 区分两种身份：SessionKey 根据文件和分片配置稳定生成，用于寻找续传记录；UUID 每次新任务重新生成，用于区分当前任务。


## 3. `BuildMetadata()`：发送前准备一次元信息

当前实现先检查文件、分片大小和请求线程数，再调用公共 GetReliablePacketCount 计算分片数量并确定实际通道数，随后分块读取文件计算 SHA-256，最后调用 Codec 生成 SessionKey 并检查元信息。当前协议暂不支持空文件。下面是公共函数中保留的计算公式：

```cpp
const qint64 nPacketCount =
    stMetadata.m_nFileSize / stMetadata.m_nSegmentSize
    + (stMetadata.m_nFileSize % stMetadata.m_nSegmentSize != 0);
```

存在余数时，比较表达式为 true，参与加法时按 1 计算。结果先用 qint64 保存，确认不超过 `std::numeric_limits<int>::max()` 后再转换，因为 Qt 5 的确认位图使用 int 表示位数。

实际通道数取 `min(请求线程数, 分片总数)`；SessionKey 的材料包含版本、文件名、大小、分片大小、实际通道数和 SHA-256。改变分片配置可能改变续传身份。UUID 使用 `QUuid::createUuid()` 为每次新任务生成，任务内重连沿用它。

哈希阶段记录实际读取字节数，结束时与起始文件大小比较，可发现读取量不一致；这不等于锁住源文件，也不能发现所有同大小改写，因此传输期间源文件应保持稳定。

### 分块取消检查不是忙等

每次读取 1 MiB 前检查当前线程的中断请求，读取和哈希之间只顺便检查一次。它不能立刻中断正在进行的 read 或哈希操作，需要等到下一个检查点。

队列连接的停止槽需等线程处理事件，无法自动打断正在执行的长函数。信号量适合等待资源；在这里反复 `tryAcquire()` 仍是轮询，阻塞 `acquire()` 则会阻止正常计算继续。

## 4. `ProcessSend()`：完整协调流程与有限恢复

头文件中的完整 @details 描述的是最终目标：准备元信息、控制握手、组织数据线程、汇总进度、请求合并校验并报告最终 Result。

**当前流程：** 十三段主流程及外层协调恢复均已接入；文件元信息只准备一次，各轮保持相同 UUID 和共享状态。每轮握手恢复位图，创建并启动 Lane，等待全部退出后读取结果并回收，再通过控制连接请求合并校验、读取并解析最终 Result。成功才报告 true，其他路径按错误类别结束或进入有限恢复。Manager 后台发送入口、Controller 与界面接线现已完成，并已接通本机收发。

| 原来单连接版本的变量 | 新设计中的位置 |
| --- | --- |
| 数据文件句柄、数据 socket、每连接缓存 | 每个 ReliableSendLane 独立持有 |
| 待发送队列、在途表、补传次数与超时记录 | 各数据通道分别维护 |
| 全文件确认位图、确认总数、取消标记 | SReliableSendTaskState，共享同一个对象 |
| 控制连接、控制回包缓存、线程调度和最终结果 | ProcessSend；完整主流程与有限协调恢复已接入 |

### 疑问：控制连接握手后会断开，再等发送结束重新连接吗？

**完整设计中不会主动断开。** 协调线程先通过控制连接握手，数据分片由另外 N 条数据连接发送；等待各数据通道结束后，再通过原来的控制连接发送完成请求包，不是再次“发送连接”。

```text
控制连接：建立 TCP → Metadata(-1) → ResumeState
                    → 保持连接，协调数据线程
                    → CompleteRequest → Result → 任务结束关闭

数据连接：建立 TCP → Metadata(各自编号) → ResumeState
                    → Data / ACK → 各自区间完成后结束
```

`oControlSocket` 和 `arrControlBuffer` 是 ProcessSend 的局部变量；只要函数尚未返回，它们就保留到后续流程使用。正常情况下保持同一条控制连接；控制写出或读取失败时，当前已接入的 nRound 恢复循环会重新握手，并清理旧连接和旧缓存。

**当前已完成控制长连接主流程：** 等待数据通道、完成请求与最终 Result 都在 ProcessSend 返回前执行。任务结束离开作用域时，局部 socket 析构并关闭连接。SignalSendFinished 本身只是发信号，不是关闭局部 socket 的直接原因。

控制连接保持登记期间，即使其他数据连接暂时全部退出，Store 的活动通道集合仍包含 -1，不会按“最后一条连接退出”移除 Session；但最后是否移除仍以集合为空为准，并没有给控制连接另设特殊的销毁规则。

### 疑问：协调线程是不是一直在等，用 wait 防止忙等？

**在第八段中，可以这样理解：协调者等待各数据通道退出，同时转交取消请求、汇总日志和进度。** 这是“带阻塞等待的轮询”，不是完全依靠事件通知，也不是不停空转检查。

`pLane->wait(10)` 等的是当前 Lane 线程退出，最多等 10 ms；不是让 Lane 休眠，也不是等待某个分片的 ACK。

| wait(10) 返回值 | 含义 |
| --- | --- |
| true | 当前 Lane 线程已经退出；如果早已退出，或等待期间提前退出，就无需等满 10 ms |
| false | 本次等待超时，当前 Lane 仍未退出；继续检查其他通道，下一轮再检查 |

阻塞等待的是协调线程，所有已启动的数据线程仍然并行发送。等待 A 时，B、C 不会因此暂停。使用无超时的 wait() 会让协调者可能长时间停在某个通道上，无法继续执行取消检查和进度报告；使用短等待，则能定期继续这些工作。单轮可能对多个尚未退出的通道分别等待，不能把 10 ms 当作整个循环或进度通知的固定周期，实际时间还受调度和快照处理影响。

#### bAllFinished 为什么每轮先设为 true？

它是本轮的初始假设：“所有通道都已退出”。只要有一次 wait 返回 false，就将它改为 false，并保持到本轮检查结束。下一轮重新检查全部通道，因此重新设为 true。

例如 A 已退出、B 未退出、C 已退出，本轮结果仍然是 false，不能结束等待。只有整轮都没有发现未退出的线程，才跳出外层 while。

**线程退出不等于发送成功。** 线程可能正常完成，也可能因错误或取消退出；第九段还要读取 WasSuccessful 并回收对象。各通道成功后，整个文件仍需通过控制连接取得成功的最终 Result。

#### 为什么等待后还要 EmitTaskSnapshot？

数据线程在后台持续更新共享确认数和日志，协调者每轮读取真实状态并报告。不是“等了 10 ms 就认为有进度”；确认数量没变化时，EmitTaskSnapshot 不重复发进度信号，日志则取走后报告。信号最终供界面显示，快照不负责推进数据发送。

#### 括号位置：先检查所有通道，再报告和判断退出

```cpp
while (true)
{
    bool bAllFinished = true;

    for (ReliableSendLane *pLane : arrLanes)
    {
        fnCancelled();

        if (!pLane->wait(10))
        {
            bAllFinished = false;
        }
    } //先检查完所有通道。

    EmitTaskSnapshot(
        pTaskState,
        nTotalPacketCount,
        nLastCompletedCount);

    if (bAllFinished)
    {
        break; //退出外层 while，进入结果检查和回收。
    }
}
```

之前手写时曾把快照和最后的完成判断放进 for 内：第一个已退出通道就可能触发 break，漏查后面的通道，而且该 break 只退出 for，外层 while 仍会继续。正确顺序是“检查完所有通道 → 报告一次快照 → 判断是否退出 while”；当前源文件的括号位置已修正。

ProcessSend 由独立后台协调线程执行，这些等待不直接阻塞 GUI 事件循环。当前 Manager::StartSend 已通过 started 信号和 Worker 上下文连接好后台启动；Worker 名称、slots 声明或 moveToThread 本身都不会让一次普通 C++ 直接调用自动切换线程。界面入口仍待通过 Controller 接入。

### 最终结果：解析成功与传输成功是两件事

ReadPacket 只取得一个完整协议包，随后检查任务 UUID；Error 的载荷作为 UTF-8 错误文本处理，正常 Result 还要核对文件大小和总分片数。ParseResultPayload 返回 true 只表示 JSON 合法并解析成功，输出参数 bSuccess 才表示接收端是否完成合并、SHA-256 校验和正式文件生成。

| bSuccess | 缺失范围 | 当前处理 |
| --- | --- | --- |
| true | 空 | 报告整个任务成功并返回 |
| true | 非空 | 结果自相矛盾，报协议结果错误 |
| false | 空 | 分片齐全但最终处理失败，报告接收端错误并返回 |
| false | 非空 | 接收端报告仍缺片，有限轮次重新握手并补传 |

### 疑问：为什么“没有缺片”还会失败？

Session 先根据位图检查缺片，齐全后才开始合并。因此磁盘空间不足、通道文件读取失败、SHA-256 不一致、QSaveFile 提交失败或正式文件重命名失败，都会产生“失败，但缺失范围为空”的 Result。

例如通道文件合计 10 GB，磁盘只剩 1 GB，额外生成合并文件可能失败。缺片列表仍为空，再补传并不能解决空间不足，所以发送端保留 strResultMessage 直接结束。位图齐全表示分片接收记录齐全，不等于正式文件已成功生成。

### 疑问：所有 Lane 已成功，为什么最终还可能报告缺片？

按当前正常实现，ACK 在分片写入并更新位图后回复；所有 Lane 成功后才请求最终检查。在会话状态一致、程序没有缺陷的前提下，最终不应突然再出现缺片。因此这条分支是处理接收状态异常或对端异常回复的兜底，不是 TCP 正常丢包的必要处理，也不能用“还有几个分片没来得及写入”解释。

接收端重启可能使持久化进度回退，但通常会先导致旧连接断开，下一轮重新握手时就发现并补齐缺片；不能据此认定正常最终结果必然经过这个兜底分支。

### 疑问：为什么要多轮？正常传输需要重复执行吗？

正常情况下第一轮收到成功 Result 就 return。多轮是自动恢复策略，单轮遇错后报告失败、由用户重新发起也可以成立；它不是文件传输必须具备的步骤。

更直接的恢复场景：数据通道已完成，但控制连接在完成请求送达前断开。重新握手取得接收状态后，跳过已确认分片，再请求合并。当前每轮只有前一轮数据线程全部退出并回收后，才可能进入控制写出、结果读取和下一轮。

| 循环 | 负责范围 |
| --- | --- |
| ProcessSend 的 nRound | 一轮握手、数据通道调度和最终检查；初次一轮加最多三轮恢复 |
| ProcessSend 的 nAttempt | 当前轮控制连接的握手重试 |
| SendLane::run 的 nAttempt | 一个数据通道自身的重连；最终失败会记录首个错误并设置共享取消 |

控制请求写出或最终回包读取失败时 continue 到下一轮；失败 Result 仍有缺片时保存错误、记录日志，运行到 for 末尾自然进入下一轮。格式、身份错误，以及没有缺片的最终失败直接结束。数据通道最终失败设置的取消标记不在下一轮清零；主动取消或恢复轮数耗尽也会结束。

### 恢复流程的验证边界

控制连接恢复与数据通道重连是不同层次的行为。模块检查和当前本机收发不能替代两机断线、进程重启及大文件恢复的系统性验收，当前验证范围统一见开篇。

## 5. `EmitTaskSnapshot()`：锁内取记录，锁外发信号

参数 `pTaskState` 指向共享任务状态，`nTotalPacketCount` 是全文件分片数，引用参数 `nLastCompletedCount` 保存并更新上次报告的完成数量；当前已由初始化、恢复和等待循环调用。

```cpp
{
    QMutexLocker oLocker(&pTaskState->m_oMutex);
    nCompletedPacketCount = pTaskState->m_nConfirmedCount;
    arrLogMessages.swap(pTaskState->m_arrLogMessages);
}
//离开局部作用域后解锁，再发日志与进度信号。
```

本地日志列表初始为空，swap 后取走原有日志，共享列表变为空，供数据线程继续追加；避免下次重复输出。进度只在完成数量与上次不同时报告，不把每个通道的快照重复相加。

发信号时不持状态锁，避免接收信号的一方处理较慢或回调共享状态时阻碍数据线程。QSharedPointer 管对象寿命，互斥锁管内容的并发访问，两者作用不同。

---

# `ReliableSendTaskState.h`

> 当前已创建并加入 `.pro`，结构体名为 `SReliableSendTaskState`。它是一个文件发送任务的共享运行状态，不包含网络连接或线程对象。

| 字段 | 含义与访问方式 |
| --- | --- |
| `m_oMutex` | 保护下方位图、计数、错误和日志；不持锁执行网络等待 |
| `m_oAckedPacketMap` | 全文件确认位图，按全局分片序号定位；初始全为 false |
| `m_nConfirmedCount` | 位图中已确认的不同分片数；重复 ACK 不重复增加 |
| `m_strFirstError` | 第一个导致任务失败的原因，避免被后续取消信息覆盖 |
| `m_arrLogMessages` | 数据线程追加，协调线程通过快照函数批量取走并报告 |
| `m_nCancelled` | 原子取消标记：0 未请求取消，1 已请求取消；不代表线程已经结束 |

构造函数按整个文件的分片数创建确认位图。一个任务只创建一份状态，协调线程与所有数据通道通过共享指针访问；不同文件任务使用各自独立的状态。

确认新分片时，在同一次加锁范围内检查位图、置位并增加计数，保持两者一致。初始化完成、尚未共享给其他线程时，不需要为构造阶段额外加锁。

---

# `ReliableTransferIo.h/.cpp`

> WritePacket、ReadPacket 和 ConnectAndHandshake 由后台发送线程调用。取消检查使用 fnCancelled；ReadPacket 接收阶段专用超时；握手数量一致性检查按当前源码保留为注释，范围检查仍执行。

这是发送侧后台线程共用的网络工具，不创建工作线程，也不管理全任务进度。当前函数职责如下：

| 函数 | 职责 | 当前状态 |
| --- | --- | --- |
| `WritePacket()` | 将一个完整协议包交给 socket，并等待 Qt 待发送缓冲区排空 | 已写出，取消调用括号已修正 |
| `ReadPacket()` | 结合连接缓存和 Codec，等待一个完整回包，处理半包/粘包、超时及取消 | 实现已写出，网络行为待验证 |
| `ConnectAndHandshake()` | 建立连接、发送 Metadata、解析 ResumeState，恢复真实缺失范围 | 已写出；数量一致性校验暂时停用 |

## `WritePacket()` 的两个数量

| 数量 | 含义 |
| --- | --- |
| `nOffset` | 当前协议包已经交给 socket 的字节数 |
| `oSocket.bytesToWrite()` | Qt 发送缓冲区中尚待写出的字节数 |

包未全部交给 socket，或 Qt 仍有待发送数据时继续处理；每次短暂等待后重新检查取消、断线和总超时。整个写包过程使用 QElapsedTimer 计时，不把每次短等待当成重新开始的总超时。

本地写出完成不等于对端接受分片。业务 ACK 确认分片，最终成功的 Result 确认完整文件已合并并校验。

### 为什么包已经交给 socket，还要继续循环？

`arrPacket` 在正常调用期间保持不变。循环有两个条件：

```cpp
while (nOffset < arrPacket.size() || oSocket.bytesToWrite() > 0)
```

- `nOffset < arrPacket.size()`：还有包数据没交给 socket，需要继续调用 write。
- `bytesToWrite() > 0`：Qt 缓冲区还有数据没写到底层，需要继续等待。

两者是“或”。例如 1000 字节已全部交给 socket，nOffset 已是 1000，但 Qt 缓冲区尚未排空，仍会进入循环；此时跳过内部的 write，只继续等待。若 write 一次只接受部分字节，才会在下一轮提交剩余部分。

`write(arrPacket.constData() + nOffset, arrPacket.size() - nOffset)` 表示“从尚未提交的位置读取剩余字节”。返回值是本次接受的字节数，负值表示错误；调用不会删除 arrPacket 的内容。

### `waitForBytesWritten()` 在等待什么？

它阻塞等待本地写出取得进展，不是等待业务 ACK，也不保证一次就排空缓冲区。每次最多等待 `min(40 毫秒, 总超时剩余时间)`，有进展或错误时可能提前返回。

返回后重新检查取消、连接状态和总超时，再决定是否继续等待。单次短等待超时不直接等于整个写包失败；是否继续由外层循环和总超时判断。

### 计时刚开始，为什么不是等待 0 ms？

start 后 elapsed 初始接近 0，但剩余时间是 `3000 - elapsed`，所以最初接近 3000 ms；再取 `min(40, 剩余时间)`，本次最多等待 40 ms。若剩余 15 ms，就最多等 15 ms；剩余时间小于等于 0 时已提前返回失败，不再调用等待函数。

计时器在循环外只 start 一次，各轮等待共用这次写包的总额度，不会在每轮重新计时。

### 取消检查要区分“函数存在”和“执行函数”

`if (fnCancelled)` 判断 std::function 是否保存了可调用对象；`if (fnCancelled())` 才执行回调并判断返回值。这里必须用后者，否则正常传入有效函数时也会被当成取消。

## `ReadPacket()` 与接收端 Worker 有什么区别？

**发送端也需要接收消息。** TCP 是双向连接：接收端处理 Metadata、Data、CompleteRequest；发送端读取 ResumeState、ACK、Result 等回复。例如发送 Metadata 后，需要等待 ResumeState，才能按接收端的缺失范围安排发送。

| 对比 | 接收端 `OnSocketReadyRead()` | 发送端 `ReadPacket()` |
| --- | --- | --- |
| 触发方式 | socket 发出 readyRead 信号 | 后台发送流程主动调用 |
| 不足一个完整包 | 保留缓存，返回，等待下次信号 | 继续读取，或短暂阻塞等待 |
| 返回时机 | 当前可处理的数据处理完后返回 | 取得一个完整包，或取消、断线、超时后返回 |
| 后续处理 | 分发文件元信息、数据和完成请求 | 调用方判断回包类型，继续握手、发送或完成流程 |

**拆包规则没有重复实现。** 两边都通过 `ReliablePacketCodec::TryTakePacket()` 检查缓存、取出完整包，各自为每条连接保留缓存。区别在于什么时候读取、半包时如何等待，以及取得包后交给谁处理。

- Codec 负责识别和提取协议包。
- 接收 Worker 负责事件驱动地接收、分发请求。
- ReadPacket 负责发送侧的有限时间阻塞读包，不自行判断业务成功。

这些阻塞等待只放在 socket 所属的后台发送线程，不在界面主线程执行，也不替换接收端的 readyRead 处理。

### ReadPacket 的包头参数不能用缺失范围类型

两项目的 ReadPacket 目前都是 7 个参数，第三个参数为 `SReliablePacketHeader &`。最终回复变量应声明为 `SReliablePacketHeader stResultHeader`，不能写成 SReliablePacketRange；后者只包含缺失序号的起止边界，无法接收协议包头。此次调用错误已在 MyServer 修正。

提取到全局的是 Utils 中的计算函数，ReadPacket 仍是 ReliableTransferIo 的静态成员。QtLargeFileTransfer 已同步公共 Utils、count(true) 和读取总超时检查顺序；参考工程 Release 编译与 4 项相关回归通过，仍与 MyServer 整应用验收分开记录。

### 为什么先解析缓存，再从 socket 读取？

数据有两层缓存：QTcpSocket 的内部接收缓冲区，以及调用方保留的 `arrBuffer`。`bytesAvailable()` 检查前者，`TryTakePacket()` 解析后者；`readAll()` 将当前可读字节搬到应用缓存。

例如一次 readAll 收到包 A、B，第一次 ReadPacket 取出 A 后，B 仍在 arrBuffer；下一次调用可直接取出 B，即使 socket 的 bytesAvailable 已是 0。首次调用若缓存为空，拆包得到 bReady=false，再读取数据并 continue 回到顶部重试即可。

先读再拆也可以正确实现；当前顺序优先利用已有缓存，确认不足后再读取更多数据。追加数据前检查总超时，也避免对端持续零碎发送时一直拖延检查。

### 为什么 `ReadPacket()` 的超时需要由参数传入？

`nTimeoutMs` 表示本次读包最多等待多久，由调用方按业务阶段选择。原来定义的固定超时常量仍然使用，只是不在 ReadPacket 内写死其中某一个。

| 等待场景 | 传入的常量 | 原因 |
| --- | --- | --- |
| Metadata 握手后等待 ResumeState | `g_nReliableControlWaitTimeoutMs`：3000 ms，即 3 秒 | 普通控制回复通常较快，长时间不回复应尽早报错 |
| 请求完成后等待最终 Result | `g_nReliableResultWaitTimeoutMs`：300000 ms，即 5 分钟 | 接收端需要合并各通道文件并计算 SHA-256，大文件处理可能较久 |

如果统一固定为 3 秒，可能将仍在正常合并的大文件任务误判为超时；如果统一固定为 5 分钟，握手无响应又要等待过久。调用方知道当前处于哪个阶段，ReadPacket 只负责执行等待。

```cpp
const qint64 nRemainingMs = nTimeoutMs - oTimer.elapsed();
```

这里限制的是一次 ReadPacket 调用的总等待时间，不是整个文件任务的总时长，也不是每轮都重新获得一份超时额度。每次短等待仍取 `min(40 ms, nRemainingMs)`，兼顾取消检查；5 分钟是可配置的等待上限，不保证所有大文件都能在该时间内合并完成。

## `ConnectAndHandshake()`：连接并取得续传状态

这个函数把 WritePacket、ReadPacket 和 Codec 串起来。它返回 true 表示当前连接的应用层握手成功，不表示文件已经发送完成。

```text
abort 旧连接，清空旧缓存及输出状态
    → connectToHost + waitForConnected 建立 TCP 连接
    → Codec 构造 Metadata，WritePacket 写出
    → ReadPacket 等待回复
    → 检查任务 ID、Error/ResumeState 类型及文件信息
    → 解析缺失范围和已接收数量，检查范围边界
```

TCP 连接建立与应用层握手是两步：前者建立通信通道，后者确认文件任务身份和接收进度。重新连接前清空旧字节缓存，避免把上条连接残留的半包带入新握手。

| 参数或输出 | 含义 |
| --- | --- |
| `stMetadata` | 同一任务各连接共用的元信息，UUID 在任务内重连时保持不变 |
| `nLaneIndex` | 默认 -1 为控制连接，0～N-1 为数据通道；默认值只写在头文件声明 |
| `arrMissingRanges` | 全文件缺失分片的闭区间列表，后续各通道只处理自己的区间 |
| `nReceivedCount` | 接收端报告的全文件已接收数量，不是当前连接独自收到的数量 |
| `strErrorMessage` | 本次连接、读写、解析或检查失败的原因 |

控制连接先发 Metadata 创建或恢复会话，各数据连接以相同 UUID 加入。握手工具不重新计算文件摘要，也不生成新任务 UUID。

### 为什么检查“缺失数 + 已接收数 = 总数”？当前为什么注释？

这项检查用于判断续传回复是否自相矛盾，不用于判断文件是否传完。例如总数 100、已收到 60、仍缺失 50，就无法对上；重复分片被错误计数，或缺失范围生成遗漏，都可能导致这种矛盾。

正常断线续传也应满足该等式。接收端 BuildResumeStatePacket 在同一次会话加锁范围内生成缺失范围并读取计数，避免两个字段取自不同时间的状态。这里检查的是回复内部一致性，不是源文件内容变化；内容身份与最终完整性另由 SessionKey、SHA-256 等机制处理。

**当前代码状态：** 按学习阶段要求，已注释缺失数量的计数变量、区间累计和最终总数比较，恢复时三处一起恢复；范围越界检查仍执行。这项统计只在握手时遍历缺失区间，不会每接收一个分片就重新统计，暂时停用不应当作为显著的性能优化。

## `ReliableCancelCheck` 的用途 （回调函数的用法）

这里是在使用 **“回调函数”**：调用方提供一段“如何判断是否取消”的代码，网络工具在需要时执行它。

我们按“定义类型 → 创建检查函数 → 传进去 → 调用”来看。

**第一步：给一种函数类型起名字。**

```cpp
using ReliableCancelCheck = std::function<bool()>;
```

拆开解释：

| 部分 | 含义 |
|---|---|
| `std::function` | 可以保存可调用对象，例如普通函数、lambda |
| `bool` | 调用后返回 `bool` |
| `()` | 调用时不需要传参数 |
| `ReliableCancelCheck` | 为这个类型起的别名 |

因此，这句代码**只是定义类型名称，还没有创建检查函数，也没有执行检查**。

类似于：

```cpp
using Number = int; //给类型起别名。
Number nValue = 10; //再用这个类型定义变量。
```

**补充：这个类型别名为什么可以在另一个文件中直接使用？**

`ReliableCancelCheck` 写在 ReliableTransferIo.h 的类、函数和 namespace 外面，因此属于全局命名空间，不是 ReliableTransferIo 的类成员，使用时无需加 `ReliableTransferIo::`。

ReliableSendLane.cpp 包含头文件后，预处理会展开其中的声明，编译器就能认识这个名字：

```cpp
#include "ReliableTransferIo.h"

//在 ReliableSendLane 的成员函数中使用：
const ReliableCancelCheck fnCancelled = [this]()
{
    return IsCancelled();
};
```

这里的 ReliableCancelCheck 与 `std::function<bool()>` 表示同一个类型，只是名字更能表达业务用途。它不是全局变量，不分配一个共享回调对象；fnCancelled 才是当前函数里创建的可调用对象，fnCancelled() 才会执行检查。

“全局命名空间”不等于所有 cpp 自动可见。每个 cpp 独立编译，仍需直接或间接包含相应头文件，并在别名声明可见的位置之后使用它。

**第二步：创建一个具体的取消检查函数。**

例如，检查当前线程是否收到了中断请求：

```cpp
ReliableCancelCheck fnCancelled = []()
{
    return QThread::currentThread()->isInterruptionRequested();
};
```

这里的 lambda 被保存到 `fnCancelled` 中。以后可以像普通函数一样调用：

```cpp
bool bCancelled = fnCancelled();
```

执行到这里，才会运行 lambda 内部的代码，取得当前的检查结果。

注意：`true` 表示取消，是我们约定的业务含义；`std::function<bool()>` 本身只规定“返回布尔值”。

**第三步：把这个检查函数传给网络工具。**

```cpp
ReliableTransferIo::WritePacket(
    oSocket,
    arrPacket,
    fnCancelled,
    strErrorMessage);
```

这里传递的是 **`fnCancelled` 这个可调用对象**，不是提前调用它得到的结果。

这两个写法有区别：

```cpp
fnCancelled    //检查函数本身，可以稍后反复调用。
fnCancelled()  //现在执行检查，得到一个 bool。
```

所以，`WritePacket()` 的参数写成：

```cpp
const ReliableCancelCheck &fnCancelled
```

表示通过引用接收这个检查函数。

**第四步：网络工具在循环中调用它。**

```cpp
if (fnCancelled())
{
    strErrorMessage = QStringLiteral("发送任务已取消。");
    return false;
}
```

这时实际执行的，就是调用方传进来的 lambda：

```cpp
return QThread::currentThread()->isInterruptionRequested();
```

每次调用都会重新检查，不是保存第一次得到的结果。

**那为什么不直接在 `WritePacket()` 中写线程中断检查？**

因为不同调用方可能使用不同的取消方式。例如，数据通道需要检查整个任务的共享标记：

```cpp
ReliableCancelCheck fnLaneCancelled = [pTaskState]()
{
    return pTaskState->m_nCancelled.loadAcquire() != 0;
};
```

再传给同一个工具函数：

```cpp
ReliableTransferIo::WritePacket(
    oSocket,
    arrPacket,
    fnLaneCancelled,
    strErrorMessage);
```

此时 `WritePacket()` 内部仍然调用：

```cpp
fnCancelled();
```

但实际执行的检查变成了“读取共享原子标记”。

其中 `[pTaskState]` 表示把共享指针的副本保存在 lambda 中，让它之后能访问同一个任务状态对象。

因此，职责分配是：

> **调用方决定“怎样判断取消”，网络工具决定“什么时候检查，以及取消后怎样退出”。**

这些回调都是在执行 `WritePacket()` 的线程中同步运行的。**传入 lambda 不会创建线程，也不是发送 Qt 信号。** 所以网络工具本身仍必须在 socket 所属的后台线程调用。


---

# `ReliableSendLane.h/.cpp`（单通道发送与重连已写出）

> 六个发送通道方法已由 ProcessSend 创建、启动、等待和回收；窗口、ACK 与有限重连属于当前发送流程。

一个 Lane 代表一条数据发送连接及其后台线程。ProcessSend 负责整个任务的协调，Lane 只负责分配给自己的连续分片区间。

| 已声明成员 | 用途 |
| --- | --- |
| `m_strFilePath`、`m_strTargetIp`、`m_nPort` | 源文件和接收端连接配置 |
| `m_stMetadata` | 当前通道保存的任务元信息副本 |
| `m_nLaneIndex` | 当前数据通道编号，从 0 开始 |
| `m_nFirstSequence`、`m_nEndSequence` | 本通道负责的左闭右开区间，例如 [10,20) 表示序号 10～19 |

当前也已声明并初始化每连接窗口 `m_nWindowSize`、共享状态指针 `m_pTaskState` 和初始为 false 的 `m_bSuccess`。各通道保存各自的元信息副本，通过共享指针共同访问同一份任务状态。WasSuccessful 在协调线程 wait 成功返回后读取；IsCancelled 同时检查全任务原子标记和当前数据线程的中断请求。

构造函数在创建对象的线程执行，只保存配置；调用 start() 后，由 Qt 在新线程执行重写的 run()。直接调用 run() 只是普通函数调用，不会创建新线程。数据 socket 和文件句柄应在后台执行时创建和使用。通道成功表示本区间的分片已确认，整个文件成功还需要控制连接收到成功的最终 Result。


## `setObjectName()`：给通道对象设置调试名称

```cpp
setObjectName(QStringLiteral("sender-lane-%1").arg(nLaneIndex));
```

通道编号为 2 时，当前 QThread 对象的运行时名称就是 `sender-lane-2`。该接口继承自 QObject，可通过 `objectName()` 读取，并输出到日志中，方便区分多个通道。

它只是识别和调试用的标签：不会启动线程、改变线程数量或自动分配分片；名称也不要求唯一。通道实际身份仍由 `m_nLaneIndex` 决定。当前构造函数只保存配置并命名，后台发送由 start() 启动，再进入 run()。


## 各函数如何配合？

| 函数 | 作用与边界 |
| --- | --- |
| 构造函数 | 保存连接、区间、窗口和共享状态，设置调试名称，不开始发送 |
| `run()` | 组织有限次数的发送尝试；成功设置结果，失败重试，最终失败记录原因并请求其他通道取消 |
| `SendPartition()` | 完成一次连接中的握手、续传恢复、窗口发送、ACK 处理和当前保留的超时补传 |
| `SendDataPacket()` | 调用 GetReliablePacketLayout 计算位置和长度，再读取分片、封包和本地写出，更新在途记录；不等待该片业务 ACK |
| `IsCancelled()` | 检查共享取消标记或当前通道线程的中断请求，不负责终止线程 |
| `WasSuccessful()` | 协调者在 wait() 成功返回后读取本通道结果；不代表整文件最终校验成功 |

调用关系：`start() → run() → SendPartition() → SendDataPacket()`。每次重新进入 SendPartition，都会新建本轮的文件句柄、socket、接收缓存和在途表，并重新握手；接收端报告已收到的分片会被跳过，不是无条件重发整个区间。

## ACK 判断：为什么检查“尚未发送的分片”？

```cpp
else if (!oLocalAcked.testBit(nLocalIndex))
{
    strErrorMessage = QStringLiteral("收到尚未发送分片的 ACK。");
    return false;
}
```

这段代码位于“未能从在途表移除该序号”的分支，因此需要同时看两份记录：

| 在途表有该序号 | 本地已确认 | 处理 |
| --- | --- | --- |
| 有 | 当前尚未确认 | 新的有效 ACK：移除记录、置位并更新共享计数 |
| 无 | true | 已确认分片的重复 ACK，忽略 |
| 无 | false | 当前连接尚未发送也未恢复确认为完成，却收到了 ACK，报协议错误 |

例如窗口为 3，当前只发了 0、1、2，却收到 ACK 7，就属于最后一种。正常配套实现不应产生这种回复；该检查用于发现错误回复或程序缺陷，不是处理 TCP 正常丢包、延迟或乱序的必要步骤。TCP 在同一连接内按序交付字节；示例按任意 ACK 序号处理，表示业务逻辑不依赖 ACK 编号顺序，并不表示 TCP 会把字节乱序交给应用。

## `run()`：重试次数与分段等待

`nAttempt <= g_nReliableDefaultReconnectCount` 且上限为 3，所以最多执行 4 轮：首次尝试一次，加上后续重试三次。当前代码对 SendPartition 返回的非取消失败都可能重试，不仅限于网络断线。

```cpp
for (int nDelay = 0;
     nDelay < nAttempt * 5 && !IsCancelled();
     ++nDelay)
{
    QThread::msleep(g_nReliableAckPollIntervalMs);
}
```

**疑问：不都是 sleep，拆开有什么区别？** 每次都休眠，但每次醒来都会重新检查取消条件。

| nAttempt | 最多休眠次数 | 按当前 40 ms 常量计算的等待时间 |
| --- | --- | --- |
| 0，首次尝试 | 0 | 0 ms |
| 1，第一次重试 | 5 | 约 200 ms |
| 2，第二次重试 | 10 | 约 400 ms |
| 3，第三次重试 | 15 | 约 600 ms |

逐渐增加等待时间，避免失败后立即连续重连。若直接 sleep(600 ms)，期间不能执行取消检查；拆成 15 次 40 ms 后，每次醒来都有机会退出。取消请求不能打断正在进行的那一次 msleep，实际响应时间还受线程调度影响。这里复用了 ACK 轮询间隔常量，等待时间也会随该常量改变。

## 取消时，为什么有的地方 `break`，有的地方 `return`？

`IsCancelled()` 返回 true 表示“已请求取消”，不是“发送成功”。`break` 退出最近一层循环，`return` 结束整个当前函数。

之前给出的写法，在 nDelay 循环结束后，用 break 退出 nAttempt 重试循环，再由循环外的取消判断 return。这里的 break 作用于 nAttempt，因为它并不在 nDelay 的循环体内。

**当前 MyServer 已改成直接 return，可以保留：** 取消时无需再执行后面的失败处理；按当前函数结构，两种写法效果相同。如果将来在循环后增加必须执行的公共收尾操作，需要重新考虑是否允许直接返回。

循环外仍要检查取消，因为取消也可能发生在 SendPartition 内，或者在下一轮循环条件检查时被发现。此时应直接结束，不把取消误写成新的“通道发送失败”。发送成功则设置 `m_bSuccess = true` 并 return，避免落到后面的失败处理。

## `storeRelease(1)` 与 `loadAcquire()`：发布取消请求

```cpp
//发布：当前通道最终失败，请求整个任务停止。
m_pTaskState->m_nCancelled.storeRelease(1);

//检查：其他通道在 IsCancelled() 中读取共享标记。
return m_pTaskState->m_nCancelled.loadAcquire() != 0
    || isInterruptionRequested();
```

- **0：** 未请求取消；**1：** 已请求取消，不表示线程已经退出。
- **store：** 原子写入标记；**load：** 原子读取标记，允许多个线程安全访问这个整数。
- **Release / Acquire：** 配对规定内存顺序。读取方读到这次发布的值时，发布前的写入对读取方后续操作可见；共享错误、日志和位图仍按现有规则加互斥锁访问。

当前 run 在最终失败时，先在锁内保留第一个错误原因，再设置共享取消标记。其他通道在下一次 IsCancelled 检查时自行退出，协调者后续还要 wait() 并回收线程。这个操作不会强制杀死线程，也不会立即打断 sleep 或网络等待。

`IsCancelled()` 中的两个条件各有来源：共享标记让一个通道的最终失败能够通知其他通道；`isInterruptionRequested()` 检查当前通道自己的中断请求。协调线程的中断请求不会自动传给各数据线程，ProcessSend 的 fnCancelled 将协调线程收到的请求转为共享取消标记，当前已在网络 IO 和等待各 Lane 的循环中调用。

## 续传同步：确认数量为什么允许减一？

```cpp
if (bAcked != m_pTaskState->m_oAckedPacketMap.testBit(nIndex))
{
    m_pTaskState->m_oAckedPacketMap.setBit(nIndex, bAcked);
    m_pTaskState->m_nConfirmedCount += bAcked ? 1 : -1;
}
```

`bAcked` 是根据本次握手生成的本地位图新状态，共享位图保存发送端原来的旧记录；只有两者不同，才调整确认计数。

| 发送端旧记录 | 本次握手的新状态 | 计数变化 |
| --- | --- | --- |
| false | false | 仍缺失，不调整 |
| false | true | 新确认一个分片，+1 |
| true | true | 仍已确认，不重复计数 |
| true | false | 撤销原来的确认，-1，安排补传 |

例如发送端已确认分片 0、1、2，总数为 3；接收端虽回复过分片 2 的 ACK，但位图采用批量持久化，如果异常退出前未保存这次进度，重启后可能只恢复 0、1。重新握手会把分片 2 报告为缺失，因此发送端将其由 true 改为 false，确认数量从 3 修正为 2；补传并重新确认后再加回来。

**确认数量是当前有效确认记录的数量，不是历史 ACK 的累计次数。** 不是每遇到缺失分片都减一；原来就是 false 的分片不调整。同步时每个 Lane 只修正自己的区间，避免用全文件旧快照覆盖其他通道的新进度。

## 发送时的整体流程

这里描述 `ReliableSendLane::SendPartition()`，它负责一个数据通道的一轮发送。外层 run 的有限重试与重连也已写出；ProcessSend 已接入多通道协调、最终 Result 与有限协调恢复，Manager 和界面接线均已接通。

```text
打开本通道的源文件，创建本线程的 socket 和接收缓存
    → 建立数据连接，发送带 laneIndex 的 Metadata
    → 取得 ResumeState，提取属于本通道的缺失分片
    → 建立本地确认位图，同步本通道的共享确认记录
    → 启动本轮 oClock，建立在途表和下一个待检查序号
    → 循环：填充窗口 → 处理 ACK → 判断完成
            → 检查断线 → 检查超时补传 → 短暂等待
    → 本区间全部确认返回 true；错误、补传超限或取消返回 false
```

| 环节 | 当前代码做什么 |
| --- | --- |
| 续传恢复 | 全文件缺失范围与本通道 [first,end) 取交集；本地位图只覆盖本通道，更新共享状态时也只修正自己的区间 |
| 填充窗口 | 从 nNextSequence 向后检查，跳过已确认分片；调用 SendDataPacket 读取、封包和写出，成功后登记在途记录 |
| 处理 ACK | 从缓存循环提取完整包，检查任务与通道信息；有效新 ACK 移除在途记录、置位并更新共享计数，重复 ACK 不重复计数 |
| 当前超时补传 | 用同一个 oClock 计算距上次发送的时间；达到 1500 ms 时重发，成功后更新时间并增加补传次数，最多补传 8 次 |
| 等待与退出 | 缓存不足完整包时先退出内层拆包循环；完成检查、断线检查及补传后，没有新数据就最多等待 40 ms，再进入下一轮 |

SendDataPacket 返回 true 只表示本地写出完成，不表示分片已确认；SendPartition 返回 true 表示本通道区间已确认，也不表示整个文件已经合并校验。整个任务最终成功仍需协调者通过控制连接请求完成并取得成功 Result。

**为什么有两处完成判断？** 填充窗口后检查一次，可在续传中发现本区间已经全部收到时提前退出；处理 ACK 后检查一次，识别刚刚完成的最后几个分片。两处条件都是 `nNextSequence == m_nEndSequence && mapInFightPacket.isEmpty()`：序号已检查到底，并且不再等待任何 ACK。按当前顺序，第一处属于可精简的提前退出位置。

**while (true) 怎么退出？** 内层只处理已有缓存，Codec 每取出一个完整包就从缓存移除它；缓存为空或只剩半包时，bReady=false，break 只退出内层拆包循环。格式或业务检查失败则 return false，退出整个 SendPartition。外层循环仍继续检查超时并等待新数据，取消后退出并返回失败。

oClock 在握手和状态同步后、发送循环之前只启动一次。每片写出成功时记录 elapsed，检查时用“当前 elapsed − 上次发送时间”计算等待时长；补传复用原在途记录，保留累计补传次数。当前保留的同连接补传策略及其弊端见下一节。

### 发送时的窗口控制（业务在途数量限制）

**这个窗口限制的是每条数据连接中“已经写出、尚未收到业务 ACK”的分片数，不是线程数量，也不是 TCP 自身的拥塞窗口。** `m_nWindowSize` 是上限，`mapInFightPacket.size()` 是当前占用数量。

```cpp
while (mapInFightPacket.size() < m_nWindowSize &&
       nNextSequence < m_nEndSequence && !IsCancelled())
```

有空位且还有序号需要检查时继续发送；续传中已确认的分片直接跳过，不占窗口。有效新 ACK 从在途表移除一条记录后，下轮就可以补入新的分片，不需要等待整批全部确认。

例如窗口为 3，所有分片最初都缺失：

| 时刻 | 在途表中的序号 | 含义 |
| --- | --- | --- |
| 发送 0、1、2 后 | [0,1,2] | 窗口已满，暂不发送新分片 |
| 收到 ACK 1 后 | [0,2] | 一个位置被释放，不要求 ACK 按序到达 |
| 下一轮发送 3 后 | [0,2,3] | 窗口重新填满 |
| 再收到重复 ACK 1 | [0,2,3] | 不再移除记录，不额外增加可用位置，也不增加确认总数 |

补传复用原有在途记录，不额外占一个窗口位置；它会额外发送字节，其性能弊端另行保留记录。

每个 Lane 都有独立窗口。若有 4 个数据通道、每个窗口为 16，业务上最多约有 64 个不同分片等待确认；这不表示创建了 64 个线程。窗口按分片数计量，实际字节量还取决于分片大小和最后一片长度。

窗口允许连续写出多个分片，减少“每发一片就等一次 ACK”的串行等待，同时限制接收程序尚未确认的工作量。窗口增大不保证更快，仍取决于网络、磁盘和处理能力；TCP 另外负责其拥塞控制与流量控制，业务窗口不替代这两者。


## 当前 TCP 上的业务超时补传及其取舍

> 当前仍保留逐片超时补传，以下说明现行策略及其局限。

### 当前实现做了什么？

SendPartition 遍历在途表，发现某分片超过 `g_nReliableDefaultPacketTimeoutMs`（1500 ms）未收到业务 ACK，就在同一 TCP 连接上重新写出整个 DataPacket；补传次数达到上限后结束本轮。对应的逐片补传次数、发送时间记录和常量目前继续保留。

### 为什么这不是 TCP 文件传输必须具备的重传？

TCP 已负责网络丢包后的重传、按序交付和拥塞控制。业务 ACK 的作用是确认接收程序已处理并接受文件分片，而 TCP 的确认不能证明应用程序已经完成文件写入。因此保留业务 ACK 有意义，但“业务 ACK 超时”不能直接推出“分片在网络中丢失”。

例如接收端正在慢速磁盘写入、线程调度延迟，或回复仍在传输，1.5 秒内没有业务 ACK 也可能只是处理较慢。TCP 的可靠性也不意味着必定在指定时间内收到业务确认，连接仍可能最终失败。

### 当前设计的主要弊端

- **可能产生无必要的重复数据。** 对 TCP 而言，再次 write 是新的字节，不会因文件分片内容相同而自动去重。接收端即使忽略重复分片，数据也已消耗带宽和处理资源。
- **慢连接或慢磁盘下可能放大负担。** 固定 1.5 秒阈值没有区分处理慢与真正停滞，多通道、大分片和较大窗口下可能反复补发，增加缓冲压力并拖慢有效数据传输。
- **不能绕过同一连接前面的阻塞。** 重复分片仍进入同一条有序字节流，不能越过尚未交付的原始数据。这些重复字节仍受 TCP 拥塞控制约束，并非绕过拥塞窗口，但额外流量可能加重已有瓶颈。

这里的业务发送窗口限制“未确认分片数量”，用于约束应用在途数据，不等同于 TCP 自身按网络状态调整的拥塞控制。

当前仍保留逐片重试次数、发送时间和相关超时常量；没有改为仅按整条连接的 ACK 进展判定停滞。

参考：[TCP 的可靠字节流与重传职责（RFC 9293）](https://www.rfc-editor.org/rfc/rfc9293.html#section-2.2)、[TCP 拥塞控制（RFC 5681）](https://www.rfc-editor.org/rfc/rfc5681.html)。

---

# Windows Release 编译与便携打包

> 本章是手动操作与学习指南。下面命令供自己逐步执行，每一步成功后再继续；写入笔记不代表重新执行编译或打包。

## 一、先分清编译、复制与部署

```text
.cpp/.h 源码 + .pro 工程配置
    ↓ qmake 生成 Makefile，mingw32-make 编译、链接
build-release/release/MyServer.exe
    ↓ Copy-Item 复制
dist/MyServer-win64/MyServer.exe
    ↓ windeployqt 收集 DLL、运行库和插件
完整发布目录
    ↓ 压缩、复制到另一台电脑、完整解压
双击 MyServer.exe 运行
```

**windeployqt 处理的是已经存在的 EXE，不负责将 C++ 源码编译成 EXE。** 编译成功也不会自动把文件复制到任意 dist 目录。

| 目录 | 存什么 | 为什么分开？ |
| --- | --- | --- |
| `D:\Qt test\MyServer\MyServer` | 源码、头文件、MyServer.pro、qrc 等 | 保留项目源文件 |
| `D:\Qt test\MyServer\build-release` | Makefile、中间目标文件、release/MyServer.exe | 将编译产物与源码分离，便于管理，这种方式叫影子构建（shadow build） |
| `D:\Qt test\MyServer\dist\MyServer-win64` | EXE 和运行依赖 | 发布时只发送运行文件，不必发送源码和编译中间文件 |

这些目录名称都是自己起的，不是 Qt 的固定要求。目录名里写 release 不会自动开启 Release；实际构建模式由 qmake 参数或 Qt Creator 的构建配置决定。

## 二、PowerShell：从编译到打包，每一步为什么这样做？

以下对应当前 Qt 5.14.2 / MinGW 7.3 / Windows x64 工程。路径中的日期沿用现有目录，后续可以自行更换，但前后路径必须一致。

### 第 1 步：配置当前终端的工具搜索路径

```powershell
$env:PATH = 'D:\Qt\5.14.2\mingw73_64\bin;D:\Qt\Tools\mingw730_64\bin;' + $env:PATH
```

**作用：** 将 Qt 工具目录和 MinGW 编译器目录放到 PATH 最前面，保留原有 PATH。

**原因：** 让当前终端及其后续启动的程序优先找到匹配的 qmake、mingw32-make、编译器及运行库，避免混用其他 Qt/编译器版本。分号是 Windows 路径列表的分隔符。这只修改当前 PowerShell 的进程环境，不会永久修改系统环境变量，也不会切换当前工作目录。

### 第 2 步：创建编译目录，再进入它

```powershell
New-Item -ItemType Directory -Path 'D:\Qt test\MyServer\build-release' -Force
Set-Location 'D:\Qt test\MyServer\build-release'
```

**作用：** 第一行创建文件夹，第二行把它设为当前工作目录。

**原因：** 只创建文件夹不能决定编译位置；关键是进入它，再执行 qmake。qmake 默认在当前工作目录生成 Makefile，后续 make 在这里读取规则并输出构建文件。`-Force` 允许目录已经存在，不表示删除、清空或重新构建其中内容。

可以执行 `Get-Location` 检查当前目录，避免在源码目录或其他目录生成构建文件。

### 第 3 步：让 qmake 读取工程，生成 Release 构建规则

```powershell
& 'D:\Qt\5.14.2\mingw73_64\bin\qmake.exe' 'D:\Qt test\MyServer\MyServer\MyServer.pro' 'CONFIG+=release' 'CONFIG-=debug'
```

| 部分 | 含义 |
| --- | --- |
| `&` | PowerShell 调用运算符，执行后面引号里的程序路径 |
| qmake.exe 的完整路径 | 明确使用这一套 Qt 工具 |
| MyServer.pro 的完整路径 | 指定读取哪个工程及其源码、Qt 模块、资源等配置 |
| `CONFIG+=release` | 在构建配置中启用 Release |
| `CONFIG-=debug` | 从构建配置中移除 Debug |

**原因：** qmake 负责根据工程配置准备编译规则，它自身并不完成 C++ 编译。`.pro` 参数决定“读哪个工程”；当前工作目录决定默认“在哪里生成构建文件”。通常 Release 开启优化，适合发布；不代表它会把所有运行库合成一个 EXE。

### 第 4 步：按 Makefile 真正编译和链接

```powershell
& 'D:\Qt\Tools\mingw730_64\bin\mingw32-make.exe' -j4
```

**作用：** 读取当前目录中的 Makefile，组织 C++ 编译、Qt 元对象/资源生成及最终链接。

**原因：** 只有完成这一步才得到可执行程序。`-j4` 表示最多同时执行四项构建工作，与程序运行时的文件发送线程数无关。如果源码和配置未变化，显示没有需要执行的工作属于正常的增量构建行为，不表示失败。

当前工程最终产物在：

```text
D:\Qt test\MyServer\build-release\release\MyServer.exe
```

可用下面命令确认它存在且不是空文件；编译失败时先处理错误，不要继续发布旧版本：

```powershell
Get-Item -LiteralPath 'D:\Qt test\MyServer\build-release\release\MyServer.exe' | Select-Object FullName, Length, LastWriteTime
```

### 第 5 步：创建发布目录，复制本次生成的 EXE

```powershell
New-Item -ItemType Directory -Path 'D:\Qt test\MyServer\dist\MyServer-win64' -Force
Copy-Item -LiteralPath 'D:\Qt test\MyServer\build-release\release\MyServer.exe' -Destination 'D:\Qt test\MyServer\dist\MyServer-win64\MyServer.exe' -Force
```

**作用：** 将编译产物复制到独立的发布目录。这里 Copy-Item 的 `-Force` 用于覆盖旧 EXE，不会清空整个目录。

**原因：** 让部署工具把 DLL 放到准备分发的位置，避免发布目录混入 Makefile、.o 等中间文件。dist 是自己的目录约定，编译器不会自动把 EXE 放进去；每次重新发布，都要更新这里的 EXE。新建一个空发布目录有利于避免遗留旧插件，重复使用目录时要留意旧文件。

### 第 6 步：windeployqt 自动收集运行依赖

```powershell
& 'D:\Qt\5.14.2\mingw73_64\bin\windeployqt.exe' --compiler-runtime --no-translations 'D:\Qt test\MyServer\dist\MyServer-win64\MyServer.exe'
```

**作用：** 分析这个已经存在的 EXE，将所需 Qt DLL、MinGW 运行库、平台插件及相关依赖部署到发布目录。

**原因：** 当前工程使用动态链接 Qt，EXE 本身不包含全部 Qt 实现；外机若没有这些文件，就可能无法启动。Qt 界面程序还需要 `platforms/qwindows.dll`。QSS 已通过 qrc 编译进程序，不需要保留开发机的源码路径。

- `--compiler-runtime`：收集 MinGW 编译器运行库，例如 libgcc、libstdc++、libwinpthread。
- `--no-translations`：跳过 Qt 自带翻译文件，可按需省略；不会删除源码里写好的中文字符串。
- 最后一个路径：待部署的应用程序，不是输出文件名，也不是让工具创建一个空 EXE。

本机这套 Qt 5.14.2 MinGW 调用时不加 `--release` / `--debug`，原因见本章常见问题。部署不会重新编译 EXE，也不会改变先前选择的 Release 模式。

部署完成后目录大致如下，实际内容由工具扫描结果决定：

```text
MyServer-win64/
    MyServer.exe
    Qt5Core.dll、Qt5Gui.dll、Qt5Widgets.dll、Qt5Network.dll
    MinGW 运行库 DLL
    platforms/qwindows.dll
    styles/、imageformats/ 等插件目录
    其他图形、SVG 等运行依赖
```

### 第 7 步：压缩完整目录，在外机解压运行

```powershell
Compress-Archive -LiteralPath 'D:\Qt test\MyServer\dist\MyServer-win64' -DestinationPath 'D:\Qt test\MyServer\dist\MyServer-win64.zip' -Force
```

**作用：** 将整个发布目录打成 ZIP，`-Force` 用于覆盖同名旧压缩包。

**原因：** DLL 和插件目录结构必须保留，不能只发 MyServer.exe，也不要直接从 ZIP 里单独运行 EXE。目标 Windows x64 电脑完整解压后运行，不必安装 Qt Creator、Qt 或 MinGW；先测启动和小文件，再测大文件与异常恢复。本机打包不产生 Linux、macOS 或 Windows 32 位版本。

两台电脑传输时，发送端使用接收电脑的局域网 IPv4 和监听端口；127.0.0.1 只访问本机，并需允许程序通过 Windows 防火墙。

## 三、如果使用 CMD，命令如何对应？

看到 `PS` 提示符表示当前是 PowerShell；下面这一组用于 CMD，不要混用两套环境变量语法。其流程和每一步目的与上面相同。

```bat
set "PATH=D:\Qt\5.14.2\mingw73_64\bin;D:\Qt\Tools\mingw730_64\bin;%PATH%"
if not exist "D:\Qt test\MyServer\build-release" mkdir "D:\Qt test\MyServer\build-release"
cd /d "D:\Qt test\MyServer\build-release"
"D:\Qt\5.14.2\mingw73_64\bin\qmake.exe" "D:\Qt test\MyServer\MyServer\MyServer.pro" "CONFIG+=release" "CONFIG-=debug"
"D:\Qt\Tools\mingw730_64\bin\mingw32-make.exe" -j4
if not exist "D:\Qt test\MyServer\dist\MyServer-win64" mkdir "D:\Qt test\MyServer\dist\MyServer-win64"
copy /Y "D:\Qt test\MyServer\build-release\release\MyServer.exe" "D:\Qt test\MyServer\dist\MyServer-win64\MyServer.exe"
"D:\Qt\5.14.2\mingw73_64\bin\windeployqt.exe" --compiler-runtime --no-translations "D:\Qt test\MyServer\dist\MyServer-win64\MyServer.exe"
```

CMD 的 `cd /d` 会同时切换盘符和目录；`copy /Y` 表示覆盖同名文件时不再询问。CMD 用 `%PATH%` 读取变量、`set` 赋值；执行带空格路径用双引号，不使用 PowerShell 的 `&` 和单引号。最后可以在资源管理器中把发布文件夹压缩为 ZIP。

## 四、常见问题与原因

### 1. Unable to find the platform plugin

本机同一套 Qt 与同一个 Release exe 的对照结果：

| windeployqt 参数 | 实际结果 |
| --- | --- |
| --release --compiler-runtime --no-translations | 失败；扫描器将 qwindows.dll 等插件标记为 debug，然后按 Release 筛选排除它们，报 Unable to find the platform plugin |
| --compiler-runtime --no-translations | 成功；默认检测模式自动部署 qwindows.dll、Qt DLL、MinGW 运行库及相关插件 |

本机 5.14.2 源码中，MinGW 的 PE 检测用 IMAGE_FILE_DEBUG_STRIPPED 标记判断是否 debug。MyServer.exe 的 PE Characteristics 为 0x22f，具有此标记；qwindows.dll 为 0x2026，没有此标记。该检测会把带有调试信息的二进制视作 debug，不能据此断定插件就是 Qt Debug 构建，更不能断定项目编译错了。

Qt 5.14.2 的 windeployqt 源码明确说明 MinGW 的 Debug 检测不可靠：默认 DebugDetectionAuto 对 MinGW 保留 MatchDebugOrRelease；显式 --release 则强制 MatchRelease，重新触发了严格筛选。平台插件原本存在，只是被筛掉；设置 QT_PLUGIN_PATH 不能解决这次筛选问题。

**修正方法：仍然编译 Release，但调用此套 MinGW 的 windeployqt 时不加 --release，也不加 --debug。** 去掉部署工具的参数不会重新编译或把 Release exe 改成 Debug。此结论针对当前已验证环境，不应机械套用到所有 Qt 版本和编译器。

源码依据：本机 `D:\Qt\5.14.2\Src\qttools\src\windeployqt\main.cpp` 的 debugDetection 分支及 `src\shared\winutils\utils.cpp` 的 MinGW 检测；对应 [Qt 5.14.2 官方源码](https://github.com/qt/qttools/blob/v5.14.2/src/windeployqt/main.cpp)。

### 2. “不是此操作系统平台的有效应用程序”

先检查实际执行的工具路径、文件是否为空或损坏，以及程序与系统架构是否兼容。错误发生在 windeployqt 启动阶段时，尚未扫描 MyServer.exe，应先让工具自身正常运行。windeployqt.exe 属于 Qt 工具，MyServer.exe 才是工程编译产物。

### 3. “MyServer.exe does not exist”

命令指定的发布目录里没有这个 EXE。windeployqt 要处理已有程序，不能把一个不存在的路径当作“请帮我编译输出到这里”。先确认第 4 步生成 EXE，再执行第 5 步复制到命令指定的位置，最后才部署。

### 4. Win64 和 windeployqt 两份目录有什么区别？

`MyServer-win64-20261105` 是早期手工收集基本依赖的临时目录；`MyServer-win64` 是使用标准工具自动收集依赖的发布目录。之前核对时，两者 EXE 的 SHA-256 相同，依赖内容不同。Win64 表示 Windows 64 位，deploy 表示部署，这些名称由自己约定，不是 Qt 的特殊目录类型。

日常发布使用 windeployqt 流程。旧 `package_release.py` 及旧 Win64 发布包仅为历史临时方案，不作为日常打包步骤。

## 五、当前验证边界

当前 Release 已完成编译、运行依赖收集和隔离 Qt/MinGW 开发路径的本机启动检查。完整发布目录位于 `D:/Qt test/MyServer/dist/MyServer-win64/`，ZIP 位于同一 dist 目录。全新外机与两机收发仍需单独验收。

---

# 合并流水线实验与当前选择

## 实验设计

在同一份 MyServer CNG 版本中比较两种方式，协议、分片、通道文件和摘要算法保持一致：

- **串行**：一个接收 Worker 线程，复用一个 4 MiB 缓冲，按文件顺序读取、写入和更新 SHA-256。
- **流水线**：同一接收线程顺序读写，另加一个摘要线程，使用 FIFO 和三个 4 MiB 缓冲顺序计算整文件 SHA-256。摘要完成前不得复用缓冲；校验通过后才能提交文件。

测试为同一台电脑自发自收，源文件、接收临时文件和最终输出位于同一块物理磁盘。文件 ext4.vhdx 共 7,132,413,952 字节，约 6.64 GiB。以下采用同一工程最新一组记录：先流水线，后串行。

## 实测结果

| 阶段 | 流水线 | 串行 |
| --- | ---: | ---: |
| 总计 | 14,003.6 ms | **9,138.4 ms** |
| 读取 | 5,511.9 ms | 2,948.7 ms |
| 写入 | 6,902.9 ms | 1,822.6 ms |
| SHA-256 | 3,879.7 ms，与读写重叠 | 3,554.2 ms |
| 等空闲缓冲 | 34.1 ms | 不适用 |
| 等摘要收尾 | 1.5 ms | 不适用 |
| commit | 1,411.0 ms | 668.0 ms |
| 改名 | 0.6 ms | 0.9 ms |
| 清理 | 129.0 ms | 140.3 ms |
| 其他 | 12.7 ms | 3.6 ms |

流水线比串行多耗时 4,865.2 ms，增加约 **53.2%**。流水线的摘要耗时与主线程读写重叠，不能再次加进总计。

## 结果说明与可能原因

1. **没有观察到端到端收益。** 流水线读取与写入合计 12,414.8 ms，串行为 4,771.3 ms，增加了 7,643.5 ms；commit 又增加 743.0 ms。主要新增耗时位于文件操作，超过了重叠摘要可能节省的时间。
2. **不是摘要队列长时间堵住生产者。** 等空闲缓冲与等摘要收尾合计只有 35.6 ms，表明该轮摘要线程基本跟得上；这个等待量不足以解释 4.87 秒差距，但也不能由此断言并发调度和内存访问没有成本。
3. **缓存与回写可能影响阶段计时。** Windows 默认缓存文件读写并延迟回写，因此 read、write 的墙钟耗时不等同于物理磁盘裸吞吐；第二轮可能有更有利的缓存或后台负载条件。[Windows 文件缓存说明](https://learn.microsoft.com/en-us/windows/win32/fileio/file-caching)
4. **并发可能改变资源竞争和读写节奏。** 两种方式都要读完并写出整份合并文件，同盘情况下不会减少 I/O 数据量；新增摘要线程、缓冲交接和内存访问，以及更连续的写入节奏，可能影响调度与缓存回写。这里是待验证解释，尚无 CPU、磁盘队列或内存带宽采样来确认根因。
5. **一组顺序测试不足以推广。** 流水线先测、串行后测，仍存在顺序与缓存影响；不能据此断定所有硬件和负载下串行一定更快，也不能把最初跨工程、不同路径的观测直接当作严格 A/B 收益。

## 当前版本的决定

主工程只保留串行合并和平台加速的标准 SHA-256，删除合并计时、计时输出参数、模式环境变量和独立摘要线程。源码中的网络超时计时器继续用于超时与重试，不属于合并性能计时。

流水线及 A/B 模式的完整工程快照、测试源码、验证证据和启动脚本保存在 `D:/Qt test/MyServer/experiments/merge-pipeline/`，不参与主工程编译。旧构建和打包结果已删除，只保留当前 Release 构建和发布包。该决定依据当前实测和实现复杂度，不宣称已经定位文件操作变慢的根因。
