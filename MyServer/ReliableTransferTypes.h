#ifndef RELIABLETRANSFERTYPES_H
#define RELIABLETRANSFERTYPES_H

#include <QByteArray>
#include <QBitArray>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QUuid>
static const quint32 g_nReliablePacketMagic = 0x52544650; // 协议魔数，用于识别本协议；大端字节对应“RTFP”
static const quint16 g_nReliablePacketVersion = 3;       // 协议版本号，当前为第 3 版：数据通道一一对应、独立临时文件、只保留 SHA-256
static const quint16 g_nReliablePacketHeaderLength = 62; // 显式序列化后的固定包头长度，单位：字节；不是 sizeof(struct)
static const int g_nReliableDefaultWindowSize = 16;     // 每条数据连接的默认发送窗口：最多同时存在 16 个未收到业务 ACK 的分片，不是线程数
static const int g_nReliableDefaultPacketTimeoutMs = 1500; // 分片等待业务 ACK 的超时阈值，单位：毫秒
static const int g_nReliableDefaultMaxRetryCount = 8;      // 单个分片最多补传 8 次，不包含初次发送
static const int g_nReliableDefaultReconnectCount = 3;     // 断线后最多尝试重连 3 次
static const int g_nReliableControlWaitTimeoutMs = 3000;   // 连接、写入等待、握手等操作的等待超时，单位：毫秒
static const int g_nReliableAckPollIntervalMs = 40;        // 发送循环每轮等待可读数据的时间，单位：毫秒；不是接收端 ACK 发送间隔
static const int g_nReliableDefaultSendThreadCount = 4;   // 默认数据发送线程数；接收数据线程与其一一对应，不包含额外的任务协调/控制线程
static const int g_nReliableMaxThreadCount = 32;          // 每个文件任务的数据通道数上限，即最多 32 对发送/接收数据线程
static const int g_nReliableMaxWindowSize = 128;          // 每条数据连接的业务发送窗口上限，单位：未确认分片数
static const int g_nReliableMaxSegmentSize = 4 * 1024 * 1024; // 单个文件分片的数据大小上限，单位：字节，即 4 MiB
static const int g_nReliableMaxPayloadSize = 16 * 1024 * 1024; // 协议载荷总上限，单位：字节，即 16 MiB；Data 还需遵守分片大小上限
static const int g_nReliableResultWaitTimeoutMs = 300000; // 等待接收端完成合并、校验并返回 Result 的超时，单位：毫秒，即 5 分钟
//接收端容量倍数：按多少组最大通道配置预留线程容量。
//仅用于计算线程总数上限，不直接限制文件任务数量。
static const int g_nReliableReceiverCapacityMultiplier = 2;
//接收线程总数上限，包含数据连接线程和控制连接线程。
static const int g_nReliableMaxReceiverThreadCount =
    g_nReliableReceiverCapacityMultiplier
    * (g_nReliableMaxThreadCount + 1);


// v3 删除未使用的 m_nFlags、m_nReserved、m_nReserved64，不再保留旧格式的尾部填充。
// 剩余字段显式编码合计：4 + 2 + 2 + 2 + 16 + 8 + 8 + 4 + 4 + 8 + 4 = 62 字节。
// 修改类型后，Codec 的读写顺序及载荷起点也必须同步更新；v3 不兼容旧版协议。

enum class EReliableMessageType : quint16 // 消息类型枚举，底层使用 16 位无符号整数
{
    MetadataPacket = 101,        // 文件元信息包：发送端介绍文件信息及本连接的通道身份，发起传输或恢复握手
    ResumeStatePacket = 102,     // 续传状态包：接收端返回已接收数量和缺失分片范围
    DataPacket = 103,            // 数据包：携带一个文件分片的原始字节
    AckPacket = 104,             // 业务确认包：确认指定序号的单个分片已被接收端接受
    CompleteRequestPacket = 105,// 完成请求包：控制连接请求接收端检查分片完整性、合并临时文件并执行最终文件校验
    ResultPacket = 106,          // 结果包：返回成功、失败或仍缺失的分片范围
    ErrorPacket = 107            // 错误包：携带错误原因
};

// 每条协议消息都带的固定包头；不是所有字段在每种消息中都有业务含义
struct SReliablePacketHeader
{
    quint32 m_nMagic;            // 协议魔数，用于检查是否符合本协议
    quint16 m_nVersion;          // 协议版本号，用于判断格式兼容性
    quint16 m_nMessageType;      // 消息类型，对应 EReliableMessageType 的枚举值
    quint16 m_nHeaderLength;     // 包头编码长度，当前固定为 62 字节
    QUuid m_oTransferId;         // 传输会话 ID，将同一文件的消息关联起来；网络编码为 16 字节，本任务的控制连接和所有数据通道共用
    quint64 m_nPacketSequence;   // 数据分片序号，从 0 开始；ACK 中表示被确认的分片序号，不是 TCP 序号
    quint64 m_nPayloadOffset;    // 数据分片在整个文件中的起始偏移，单位：字节，从 0 开始
    quint32 m_nPayloadSize;      // 当前消息的载荷长度，单位：字节，不包含 62 字节包头
    quint32 m_nTotalPacketCount; // 整个文件的数据分片总数，不包含 ACK 等控制消息
    quint64 m_nFileSize;         // 整个文件的总大小，单位：字节
    quint32 m_nWindowSize;       // 发送端声明的每条数据连接的业务发送窗口，单位：分片数；参考实现在 Metadata 包中赋值

    SReliablePacketHeader()
        : m_nMagic(g_nReliablePacketMagic),
          m_nVersion(g_nReliablePacketVersion),
          m_nMessageType(static_cast<quint16>(EReliableMessageType::MetadataPacket)),
          m_nHeaderLength(g_nReliablePacketHeaderLength),
          m_oTransferId(), // 默认构造得到空 UUID，后续需要赋值
          m_nPacketSequence(0),
          m_nPayloadOffset(0),
          m_nPayloadSize(0),
          m_nTotalPacketCount(0),
          m_nFileSize(0),
          m_nWindowSize(0)
    {
    }
};

// 连续缺失分片的序号范围，采用闭区间 [start, end]，两端都包含
struct SReliablePacketRange
{
    quint64 m_nStartSequence; // 第一个缺失分片的序号，包含此序号
    quint64 m_nEndSequence;   // 最后一个缺失分片的序号，包含此序号

    SReliablePacketRange()
        : m_nStartSequence(0),
          m_nEndSequence(0) // 默认 [0, 0] 表示分片 0，不表示空范围
    {
    }

    SReliablePacketRange(quint64 nStartSequence, quint64 nEndSequence)
        : m_nStartSequence(nStartSequence),
          m_nEndSequence(nEndSequence)
    {
    }
};

// 整个待传文件的元信息
struct SReliableFileMetadata
{
    QUuid m_oTransferId;         // 传输会话 ID，与该文件各消息包头中的 ID 一致；新任务重新生成，任务内重连保持不变
    QString m_strSessionKey;     // 持久化续传会话键，用于定位接收端保存的状态；参考实现由文件信息哈希生成(定位磁盘上的历史传输状态)
                                // v3 的生成材料包含版本、文件名、文件大小、分片大小、实际数据通道数和文件 SHA-256
    QString m_strFileName;       // 文件名，例如 video.mp4，不包含发送端的目录路径
    qint64 m_nFileSize;          // 整个文件的大小，单位：字节
    int m_nSegmentSize;          // 标准分片大小，单位：字节；最后一个分片可以更小
    quint32 m_nTotalPacketCount; // 总分片数，非空文件按 ceil(文件大小 / 分片大小) 计算
    int m_nLaneCount;            // 实际数据通道数；每个通道对应一个发送线程、一个接收线程和一个独立临时文件，取 min(请求线程数, 总分片数)
    QString m_strSha256;         // 整个文件的 SHA-256 摘要，以十六进制字符串保存，用于完整性比较；统一使用 64 个小写十六进制字符

    SReliableFileMetadata()
        : m_oTransferId(),
          m_strSessionKey(),
          m_strFileName(),
          m_nFileSize(0),
          m_nSegmentSize(0),
          m_nTotalPacketCount(0),
          m_nLaneCount(1),
          m_strSha256()
    {
    }
};

// 发送端本地维护的未确认分片记录，用于跟踪 ACK 和超时补传
struct SReliablePendingPacket
{
    quint64 m_nPacketSequence;      // 等待业务 ACK 的分片序号，从 0 开始
    qint64 m_nPayloadOffset;        // 该分片在源文件中的起始偏移，单位：字节
    int m_nPayloadSize;             // 该分片实际的数据长度，单位：字节，不包含包头
    int m_nRetryCount;              // 已补传次数，初始为 0；初次发送不计入
    qint64 m_nLastSendTimestampMs;  // 最近一次发送时间；新版参考实现记录 QElapsedTimer 的已流逝毫秒数，用于判断超时，不使用 Unix 纪元时间

    SReliablePendingPacket()
        : m_nPacketSequence(0),
          m_nPayloadOffset(0),
          m_nPayloadSize(0),
          m_nRetryCount(0),
          m_nLastSendTimestampMs(0)
    {
    }
};

//把缺失范围列表转换成 JSON 数组
inline QJsonArray BuildRangeJsonArray(const QList<SReliablePacketRange> &arrMissingRangeList){
    QJsonArray oRangeArray;
    for(const SReliablePacketRange &stRange : arrMissingRangeList){
        QJsonArray oItemArray;
        oItemArray.append(QString::number(stRange.m_nStartSequence));
        oItemArray.append(QString::number(stRange.m_nEndSequence));
        oRangeArray.append(oItemArray);
    }

    return oRangeArray;
}
// 从 JSON 数组还原缺失范围列表
// 迁移过渡：当前 Codec 仍在调用，暂时保留；后续修改 Codec 时移入严格解析逻辑，再删除此函数。
inline QList<SReliablePacketRange> ParseRangeJsonArray(const QJsonArray &oRangeArray){
    QList<SReliablePacketRange> arrMissingRangeList;
    for(const QJsonValue &oValue : oRangeArray){
        if(!oValue.isArray()){
            continue;
        }
        const QJsonArray oItemArray = oValue.toArray();
        if(oItemArray.size() < 2){
            continue;
        }
        arrMissingRangeList.append(SReliablePacketRange(oItemArray.at(0).toString().toULongLong(),
                                                       oItemArray.at(1).toString().toULongLong() ));
    }
    return arrMissingRangeList;
}
//【位图计数统一】使用 QBitArray::count(true)，不再维护 CountTrueBits。

#endif // RELIABLETRANSFERTYPES_H
