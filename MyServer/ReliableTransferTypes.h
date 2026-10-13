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
static const quint16 g_nReliablePacketVersion = 1;       // 协议版本号，当前为第 1 版
static const quint16 g_nReliablePacketHeaderLength = 80; // 显式序列化后的固定包头长度，单位：字节；不是 sizeof(struct)
static const int g_nReliableDefaultWindowSize = 16;     // 默认发送窗口：最多同时存在 16 个未收到业务 ACK 的分片
static const int g_nReliableDefaultPacketTimeoutMs = 1500; // 分片等待业务 ACK 的超时阈值，单位：毫秒
static const int g_nReliableDefaultMaxRetryCount = 8;      // 单个分片最多补传 8 次，不包含初次发送
static const int g_nReliableDefaultReconnectCount = 3;     // 断线后最多尝试重连 3 次
static const int g_nReliableControlWaitTimeoutMs = 3000;   // 连接、写入等待、握手等操作的等待超时，单位：毫秒
static const int g_nReliableAckPollIntervalMs = 40;        // 发送循环每轮等待可读数据的时间，单位：毫秒；不是接收端 ACK 发送间隔

enum class EReliableMessageType : quint16 // 消息类型枚举，底层使用 16 位无符号整数
{
    MetadataPacket = 101,        // 文件元信息包：发送端介绍文件信息，发起传输或恢复握手
    ResumeStatePacket = 102,     // 续传状态包：接收端返回已接收数量和缺失分片范围
    DataPacket = 103,            // 数据包：携带一个文件分片的原始字节
    AckPacket = 104,             // 业务确认包：确认指定序号的单个分片已被接收端接受
    CompleteRequestPacket = 105,// 完成请求包：请求接收端检查分片完整性并执行最终文件校验
    ResultPacket = 106,          // 结果包：返回成功、失败或仍缺失的分片范围
    ErrorPacket = 107            // 错误包：携带错误原因
};

// 每条协议消息都带的固定包头；不是所有字段在每种消息中都有业务含义
struct SReliablePacketHeader
{
    quint32 m_nMagic;            // 协议魔数，用于检查是否符合本协议
    quint16 m_nVersion;          // 协议版本号，用于判断格式兼容性
    quint16 m_nMessageType;      // 消息类型，对应 EReliableMessageType 的枚举值
    quint16 m_nHeaderLength;     // 包头编码长度，当前固定为 80 字节
    quint16 m_nFlags;            // 标志位，参考实现当前未使用，置 0
    QUuid m_oTransferId;         // 传输会话 ID，将同一文件的消息关联起来；网络编码为 16 字节
    quint64 m_nPacketSequence;   // 数据分片序号，从 0 开始；ACK 中表示被确认的分片序号，不是 TCP 序号
    quint64 m_nPayloadOffset;    // 数据分片在整个文件中的起始偏移，单位：字节，从 0 开始
    quint32 m_nPayloadSize;      // 当前消息的载荷长度，单位：字节，不包含 80 字节包头
    quint32 m_nTotalPacketCount; // 整个文件的数据分片总数，不包含 ACK 等控制消息
    quint64 m_nFileSize;         // 整个文件的总大小，单位：字节
    quint32 m_nWindowSize;       // 发送端声明的业务发送窗口，单位：分片数；参考实现在 Metadata 包中赋值
    quint32 m_nReserved;         // 32 位预留字段，当前置 0
    quint64 m_nReserved64;       // 64 位预留字段，当前置 0

    SReliablePacketHeader()
        : m_nMagic(g_nReliablePacketMagic),
          m_nVersion(g_nReliablePacketVersion),
          m_nMessageType(static_cast<quint16>(EReliableMessageType::MetadataPacket)),
          m_nHeaderLength(g_nReliablePacketHeaderLength),
          m_nFlags(0),
          m_oTransferId(), // 默认构造得到空 UUID，后续需要赋值
          m_nPacketSequence(0),
          m_nPayloadOffset(0),
          m_nPayloadSize(0),
          m_nTotalPacketCount(0),
          m_nFileSize(0),
          m_nWindowSize(0),
          m_nReserved(0),
          m_nReserved64(0)
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
    QUuid m_oTransferId;         // 传输会话 ID，与该文件各消息包头中的 ID 一致
    QString m_strSessionKey;     // 持久化续传会话键，用于定位接收端保存的状态；参考实现由文件信息哈希生成(定位磁盘上的历史传输状态)
    QString m_strFileName;       // 文件名，例如 video.mp4，不包含发送端的目录路径
    qint64 m_nFileSize;          // 整个文件的大小，单位：字节
    int m_nSegmentSize;          // 标准分片大小，单位：字节；最后一个分片可以更小
    quint32 m_nTotalPacketCount; // 总分片数，非空文件按 ceil(文件大小 / 分片大小) 计算
    QString m_strMd5;            // 整个文件的 MD5 摘要，以十六进制字符串保存，用于完整性比较
    QString m_strSha256;         // 整个文件的 SHA-256 摘要，以十六进制字符串保存，用于完整性比较

    SReliableFileMetadata()
        : m_oTransferId(),
          m_strSessionKey(),
          m_strFileName(),
          m_nFileSize(0),
          m_nSegmentSize(0),
          m_nTotalPacketCount(0),
          m_strMd5(),
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
    qint64 m_nLastSendTimestampMs;  // 最近一次发送时间；参考实现使用 Unix 纪元以来的毫秒数，用于判断超时

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
//统计位图中值为 `true` 的位置数量，用于计算已接收或已确认分片数
inline int CountTrueBits(const QBitArray &oBitArray){
    int nMarkedCount = 0;
    for(int nIndex = 0; nIndex < oBitArray.size(); ++nIndex){
//        testBit(nIndex) 是 QBitArray 的成员函数，用来读取指定位置的位，返回 bool：
//        - 该位为 1 → 返回 true
//        - 该位为 0 → 返回 false
        if(oBitArray.testBit(nIndex)){
            ++nMarkedCount;
        }
    }

    return nMarkedCount;
}

#endif // RELIABLETRANSFERTYPES_H
