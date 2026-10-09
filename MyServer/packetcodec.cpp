#include "packetcodec.h"

#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>

PacketCodec::PacketCodec()
{

}

int PacketCodec::GetHeaderLength()
{
    return g_nPacketHeaderLength;
}

QByteArray PacketCodec::BuildPacket(const SPacketHeader &stPacketHeader, const QByteArray &arrPayload)
{
    QByteArray arrHeaderData;
    arrHeaderData.resize(GetHeaderLength());// 先把包头的空间划分好
    QDataStream oDataStream(&arrHeaderData,QIODevice::WriteOnly);
    oDataStream.setByteOrder(QDataStream::BigEndian);//网络协议都是采用的BigEndian，保证不同平台传输整数时字节排列一致。

    QByteArray arrTransferId = stPacketHeader.m_oTransferId.toRfc4122(); //本次文件传输的唯一 ID，区分同时传输的多个文件
    if(arrTransferId.size() != 16){//当未创建本次传输的ID时，创建一个
        arrTransferId = QUuid::createUuid().toRfc4122();
    }
//    | `m_nMagic` | 4 字节 | 魔数，例如 `0x51465450`，用来判断收到的是不是本协议的数据包 |
//    | `m_nVersion` | 2 字节 | 协议版本，后续升级协议时用于兼容判断 |
//    | `m_nMessageType` | 2 字节 | 包类型：元信息包、文件数据包、完成通知包等 |
//    | `m_nFeatureFlags` | 2 字节 | 标志位，例如是否为首分片、末分片、块开始或块结束 |
//    | `m_nHeaderLength` | 2 字节 | 包头长度；当前固定为 `64` |
//    | `m_oTransferId` | 16 字节 | 本次文件传输的唯一 ID，区分同时传输的多个文件 |
//    | `m_nChunkIndex` | 4 字节 | 当前是文件的第几个“块” |
//    | `m_nChunkCount` | 4 字节 | 文件总块数 |
//    | `m_nSegmentIndex` | 4 字节 | 当前是这个块里的第几个“网络分片” |
//    | `m_nSegmentCount` | 4 字节 | 当前块总共有多少个网络分片 |
//    | `m_nPayloadSize` | 4 字节 | 后面实际载荷 `arrPayload` 的字节数 |
//    | `m_nFileSize` | 8 字节 | 整个文件的总字节数 |
//    | `m_nChunkOffset` | 8 字节 | 当前块在原始文件中的起始字节偏移 |
    oDataStream << stPacketHeader.m_nMagic
                << stPacketHeader.m_nVersion
                << stPacketHeader.m_nMessageType
                << stPacketHeader.m_nFeatureFlags
                << stPacketHeader.m_nHeaderLength;
    oDataStream.writeRawData(arrTransferId.constData(),arrTransferId.size()); //DataStream 在序列化 QByteArray 时，不只写内容，还会先写长度,所以需要这样写，确保只写入的是内容
    oDataStream << stPacketHeader.m_nChunkIndex
                << stPacketHeader.m_nChunkCount
                << stPacketHeader.m_nSegmentIndex
                << stPacketHeader.m_nSegmentCount
                << stPacketHeader.m_nPayloadSize
                << stPacketHeader.m_nFileSize
                << stPacketHeader.m_nChunkOffset;
    return arrHeaderData + arrPayload;
}

bool PacketCodec::ParsePacketHeader(const QByteArray &arrHeaderData, SPacketHeader &stPacketHeader, QString &strErrorMessage)
{
    if (arrHeaderData.size() != GetHeaderLength()){
        strErrorMessage = QStringLiteral("包头长度非法：%1").arg(arrHeaderData.size());
        return false;
    }

    QDataStream oDataStream(arrHeaderData);
    oDataStream.setByteOrder(QDataStream::BigEndian);

    QByteArray arrTransferId(16, '\0'); //创建了一个长度为 16 字节的 QByteArray，并把每个字节都初始化为 '\0'（数值 0）
    oDataStream >> stPacketHeader.m_nMagic
                >> stPacketHeader.m_nVersion
                >> stPacketHeader.m_nMessageType
                >> stPacketHeader.m_nFeatureFlags
                >> stPacketHeader.m_nHeaderLength;
    oDataStream.readRawData(arrTransferId.data(),arrTransferId.size());
    oDataStream >> stPacketHeader.m_nChunkIndex
                >> stPacketHeader.m_nChunkCount
                >> stPacketHeader.m_nSegmentIndex
                >> stPacketHeader.m_nSegmentCount
                >> stPacketHeader.m_nPayloadSize
                >> stPacketHeader.m_nFileSize
                >> stPacketHeader.m_nChunkOffset;
    //转换arrTransferId的形式
    stPacketHeader.m_oTransferId = QUuid::fromRfc4122(arrTransferId);
    if(stPacketHeader.m_nMagic != g_nPacketMagic){
        strErrorMessage = QStringLiteral("魔数不匹配");
        return false;
    }
    if (stPacketHeader.m_nHeaderLength != GetHeaderLength())
    {
        strErrorMessage = QStringLiteral("包头版本不兼容。");
        return false;
    }

    return true;
}
//| 字段 | 含义 |
//|---|---|
//| `m_oTransferId` | 本次传输的唯一 UUID；用来区分不同文件或并发传输任务 |
//| `m_strFileName` | 原文件名，例如 `video.mp4` |
//| `m_nFileSize` | 文件总大小，单位是字节 |
//| `m_nChunkCount` | 文件一共被切成多少个“块” |
//| `m_nChunkSize` | 每块的大小，例如 4 MB |
//| `m_nSegmentSize` | 每个块还会拆成多少字节的网络分片，例如 60 KB |
QByteArray PacketCodec::BuildMetadataPayload(const STransferFileMetadata &stMetadata)
{
    QJsonObject oJsonObject;
    oJsonObject.insert(QStringLiteral("transferId"),stMetadata.m_oTransferId.toString(QUuid::WithoutBraces));
    oJsonObject.insert(QStringLiteral("fileName"),stMetadata.m_strFileName);
    oJsonObject.insert(QStringLiteral("fileSize"),QString::number(stMetadata.m_nFileSize));
    oJsonObject.insert(QStringLiteral("chunkCount"), stMetadata.m_nChunkCount);
    oJsonObject.insert(QStringLiteral("chunkSize"), QString::number(stMetadata.m_nChunkSize));
    oJsonObject.insert(QStringLiteral("segmentSize"), stMetadata.m_nSegmentSize);

    return QJsonDocument(oJsonObject).toJson(QJsonDocument::Compact); //.toJson(QJsonDocument::Compact) 转成 JSON 字节内容，且使用紧凑格式：没有缩进和多余换行。
}

bool PacketCodec::ParseMetadataPayload(const QByteArray &arrPayload, STransferFileMetadata &stMetadata, QString &strErrorMessage)
{
    QJsonParseError oParseError;
    const QJsonDocument oJsonDocument = QJsonDocument::fromJson(arrPayload,&oParseError);
    if(oParseError.error != QJsonParseError::NoError || !oJsonDocument.isObject()){
        strErrorMessage = QStringLiteral("元信息 JSON 解析失败：%1").arg(oParseError.errorString());
        return false;
    }

    const QJsonObject oJsonObject = oJsonDocument.object();

    stMetadata.m_oTransferId = QUuid(oJsonObject.value(QStringLiteral("transferId")).toString());
    stMetadata.m_strFileName = oJsonObject.value(QStringLiteral("fileName")).toString();
    stMetadata.m_nFileSize = oJsonObject.value(QStringLiteral("fileSize")).toString().toLongLong();
    stMetadata.m_nChunkCount = oJsonObject.value(QStringLiteral("chunkCount")).toInt();
    stMetadata.m_nChunkSize = oJsonObject.value(QStringLiteral("chunkSize")).toString().toLongLong();
    stMetadata.m_nSegmentSize = oJsonObject.value(QStringLiteral("segmentSize")).toInt();

    if (stMetadata.m_oTransferId.isNull() || stMetadata.m_strFileName.isEmpty() || stMetadata.m_nFileSize <= 0){
        strErrorMessage = QStringLiteral("元信息字段缺失或非法。");
        return false;
    }

    return true;
}

//为Meta数据加表头
QByteArray PacketCodec::BuildMetadataPacket(const STransferFileMetadata &stMetadata)
{
    SPacketHeader stPacketHeader;
    const QByteArray arrPayload = BuildMetadataPayload(stMetadata);

    stPacketHeader.m_oTransferId = stMetadata.m_oTransferId;
    stPacketHeader.m_nMessageType = static_cast<quint16>(EPacketMessageType::MetadataPacket);
    stPacketHeader.m_nChunkCount = static_cast<quint32>(stMetadata.m_nChunkSize);
    stPacketHeader.m_nPayloadSize = static_cast<quint32>(arrPayload.size());
    stPacketHeader.m_nFileSize = static_cast<quint64>(stMetadata.m_nFileSize);

    return BuildPacket(stPacketHeader,arrPayload);
}

QByteArray PacketCodec::BuildChunkPacket(const STransferFileMetadata &stMetadata, quint32 nChunkIndex, quint32 nSegmentIndex, quint32 nSegmentCount, quint64 nChunkOffset, const QByteArray &arrPayload)
{
    SPacketHeader stPacketHeader;
    TPacketFeatureFlags oFeatureFlags = PacketFeatureNone;

    //这里实际上有一点语义重复，firstSegment等价于ChunkStart
    if (nSegmentIndex == 0){
        oFeatureFlags |= PacketFeatureFirstSegment;
        oFeatureFlags |= PacketFeatureChunkStart;
    }

    if (nSegmentIndex + 1 == nSegmentCount){
        oFeatureFlags |= PacketFeatureLastSegment;
        oFeatureFlags |= PacketFeatureChunkEnd;
    }

    stPacketHeader.m_oTransferId = stMetadata.m_oTransferId;
    stPacketHeader.m_nMessageType = static_cast<quint16>(EPacketMessageType::ChunkPacket);
    stPacketHeader.m_nFeatureFlags = static_cast<quint16>(oFeatureFlags);
    stPacketHeader.m_nChunkIndex = nChunkIndex;
    stPacketHeader.m_nChunkCount = static_cast<quint32>(stMetadata.m_nChunkCount);
    stPacketHeader.m_nSegmentIndex = nSegmentIndex;
    stPacketHeader.m_nSegmentCount = nSegmentCount;
    stPacketHeader.m_nPayloadSize = static_cast<quint32>(arrPayload.size());
    stPacketHeader.m_nFileSize = static_cast<quint64>(stMetadata.m_nFileSize);
    stPacketHeader.m_nChunkOffset = nChunkOffset;

    return BuildPacket(stPacketHeader,arrPayload);
}

QByteArray PacketCodec::BuildCompletePacket(const STransferFileMetadata &stMetadata)
{
    SPacketHeader stPacketHeader;

    stPacketHeader.m_oTransferId = stMetadata.m_oTransferId;
    stPacketHeader.m_nMessageType = static_cast<quint16>(EPacketMessageType::CompletePacket);
    stPacketHeader.m_nChunkCount = static_cast<quint32>(stMetadata.m_nChunkCount);
    stPacketHeader.m_nFileSize = static_cast<quint64>(stMetadata.m_nFileSize);

    return BuildPacket(stPacketHeader,QByteArray());//传送结束了，返回空包就行
}
