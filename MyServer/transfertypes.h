#ifndef TRANSFERTYPES_H
#define TRANSFERTYPES_H

#include <QFlags>
#include <QString>
#include <QUuid>

static const quint32 g_nPacketMagic = 0x51465450;
static const quint16 g_nPacketVersion = 1;
static const quint16 g_nPacketHeaderLength = 64;
static const qint64 g_nDefaultChunkSize = 4 * 1024 * 1024;
static const qint32 g_nDefaultSegmentSize = 60 * 1024;
static const qint32 g_nDefaultThreadCount = 4;
static const qint32 g_nDefaultSendRetryCount = 3;
static const qint32 g_nDefaultSendRetryDelayMs = 60;

enum class ETransferMode
{
    TcpMode = 0,
    UdpMode = 1
};

enum class EPacketMessageType : quint16
{
    MetadataPacket = 1,
    ChunkPacket = 2,
    CompletePacket = 3
};

//这样标记可以进行或运算，来判断同时存在哪几个状态
enum EPacketFeatureFlag
{
    PacketFeatureNone = 0x0000,
    PacketFeatureFirstSegment = 0x0001,
    PacketFeatureLastSegment = 0x0002,
    PacketFeatureChunkStart = 0x0004,
    PacketFeatureChunkEnd = 0x0008
};
Q_DECLARE_FLAGS(TPacketFeatureFlags, EPacketFeatureFlag) //大致等价于创建一个 Qt 的类型安全位集合：using TPacketFeatureFlags = QFlags<EPacketFeatureFlag>;
Q_DECLARE_OPERATORS_FOR_FLAGS(TPacketFeatureFlags) //给这个类型补上位运算符，例如 |、&、|=。这样才能自然组合：TPacketFeatureFlags flags = PacketFeatureFirstSegment | PacketFeatureChunkStart;

struct SPacketHeader
{
    quint32 m_nMagic;
    quint16 m_nVersion;
    quint16 m_nMessageType;
    quint16 m_nFeatureFlags;
    quint16 m_nHeaderLength;
    QUuid m_oTransferId;
    quint32 m_nChunkIndex;
    quint32 m_nChunkCount;
    quint32 m_nSegmentIndex;
    quint32 m_nSegmentCount;
    quint32 m_nPayloadSize;
    quint64 m_nFileSize;
    quint64 m_nChunkOffset;

    SPacketHeader()
        : m_nMagic(g_nPacketMagic),
          m_nVersion(g_nPacketVersion),
          m_nMessageType(static_cast<quint16>(EPacketMessageType::ChunkPacket)),
          m_nFeatureFlags(PacketFeatureNone),
          m_nHeaderLength(g_nPacketHeaderLength),
          m_oTransferId(),
          m_nChunkIndex(0),
          m_nChunkCount(0),
          m_nSegmentIndex(0),
          m_nSegmentCount(0),
          m_nPayloadSize(0),
          m_nFileSize(0),
          m_nChunkOffset(0)
    {
    }
};

struct STransferFileMetadata
{
    QUuid m_oTransferId;
    QString m_strFileName;
    qint64 m_nFileSize;
    qint32 m_nChunkCount;
    qint64 m_nChunkSize;
    qint32 m_nSegmentSize;

    STransferFileMetadata()
        : m_oTransferId(),
          m_strFileName(),
          m_nFileSize(0),
          m_nChunkCount(0),
          m_nChunkSize(g_nDefaultChunkSize),
          m_nSegmentSize(g_nDefaultSegmentSize)
    {
    }
};

#endif // TRANSFERTYPES_H
