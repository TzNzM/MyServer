#include "ReliablePacketCodec.h"

#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>

ReliablePacketCodec::ReliablePacketCodec()
{

}

int ReliablePacketCodec::GetHeaderLength()
{
    return  g_nReliablePacketHeaderLength;
}

QByteArray ReliablePacketCodec::BuildPacket(const SReliablePacketHeader &stPacketHeader, const QByteArray &arrPayload)
{
    QByteArray arrHeaderData;
    arrHeaderData.resize(GetHeaderLength());

    QDataStream oDataStream(&arrHeaderData, QIODevice::WriteOnly);
    oDataStream.setByteOrder(QDataStream::BigEndian);

    QByteArray arrTransferId = stPacketHeader.m_oTransferId.toRfc4122();
    if (arrTransferId.size() != 16)
    {
        arrTransferId = QUuid::createUuid().toRfc4122();
    }

    oDataStream << stPacketHeader.m_nMagic
                << stPacketHeader.m_nVersion
                << stPacketHeader.m_nMessageType
                << stPacketHeader.m_nHeaderLength
                << stPacketHeader.m_nFlags;
    oDataStream.writeRawData(arrTransferId.constData(), arrTransferId.size()); //DataStream 在序列化 QByteArray 时，不只写内容，还会先写长度,所以需要这样写，确保只写入的是内容
    oDataStream << stPacketHeader.m_nPacketSequence
                << stPacketHeader.m_nPayloadOffset
                << stPacketHeader.m_nPayloadSize
                << stPacketHeader.m_nTotalPacketCount
                << stPacketHeader.m_nFileSize
                << stPacketHeader.m_nWindowSize
                << stPacketHeader.m_nReserved
                << stPacketHeader.m_nReserved64;

    return arrHeaderData + arrPayload;
}

//build JSON Packet
static QByteArray BuildJsonPacketPayload(const QJsonObject &oJsonObject)
{
    return QJsonDocument(oJsonObject).toJson(QJsonDocument::Compact);//设置为紧凑类型的
}


bool ReliablePacketCodec::ParsePacketHeader(const QByteArray &arrHeaderData, SReliablePacketHeader &stPacketHeader, QString &strErrorMessage)
{
    if(arrHeaderData.size() != GetHeaderLength()){
        strErrorMessage = QStringLiteral("可靠协议包头长度非法：%1").arg(arrHeaderData.size());
        return false;
    }

    QDataStream oDataStream(arrHeaderData);
    oDataStream.setByteOrder(QDataStream::BigEndian);

    QByteArray arrTransferId(16, '\0');
    oDataStream >> stPacketHeader.m_nMagic
                >> stPacketHeader.m_nVersion
                >> stPacketHeader.m_nMessageType
                >> stPacketHeader.m_nHeaderLength
                >> stPacketHeader.m_nFlags;
    oDataStream.readRawData(arrTransferId.data(), arrTransferId.size());
    oDataStream >> stPacketHeader.m_nPacketSequence
                >> stPacketHeader.m_nPayloadOffset
                >> stPacketHeader.m_nPayloadSize
                >> stPacketHeader.m_nTotalPacketCount
                >> stPacketHeader.m_nFileSize
                >> stPacketHeader.m_nWindowSize
                >> stPacketHeader.m_nReserved
                >> stPacketHeader.m_nReserved64;
    stPacketHeader.m_oTransferId = QUuid::fromRfc4122(arrTransferId);

    if (stPacketHeader.m_nMagic != g_nReliablePacketMagic)
    {
        strErrorMessage = QStringLiteral("可靠协议魔数不匹配。");
        return false;
    }

    if (stPacketHeader.m_nHeaderLength != GetHeaderLength())
    {
        strErrorMessage = QStringLiteral("可靠协议头长度不兼容。");
        return false;
    }

    return true;
}

QByteArray ReliablePacketCodec::BuildMetadataPacket(const SReliableFileMetadata &stMetadata, int nWindowSize)
{
    QJsonObject oJsonObject;
    oJsonObject.insert(QStringLiteral("sessionKey"), stMetadata.m_strSessionKey);
    oJsonObject.insert(QStringLiteral("fileName"), stMetadata.m_strFileName);
    oJsonObject.insert(QStringLiteral("fileSize"), QString::number(stMetadata.m_nFileSize));
    oJsonObject.insert(QStringLiteral("segmentSize"), stMetadata.m_nSegmentSize);
    oJsonObject.insert(QStringLiteral("totalPacketCount"), static_cast<int>(stMetadata.m_nTotalPacketCount));
    oJsonObject.insert(QStringLiteral("md5"), stMetadata.m_strMd5);
    oJsonObject.insert(QStringLiteral("sha256"), stMetadata.m_strSha256);

    const QByteArray arrPayload = BuildJsonPacketPayload(oJsonObject);

    SReliablePacketHeader stPacketHeader; //这里是给这个MetaData上一个统一的头
    stPacketHeader.m_oTransferId = stMetadata.m_oTransferId;
    stPacketHeader.m_nMessageType = static_cast<quint16>(EReliableMessageType::MetadataPacket);
    stPacketHeader.m_nPayloadSize = static_cast<quint32>(arrPayload.size());
    stPacketHeader.m_nTotalPacketCount = stMetadata.m_nTotalPacketCount;
    stPacketHeader.m_nFileSize = static_cast<quint64>(stMetadata.m_nFileSize);
    stPacketHeader.m_nWindowSize = static_cast<quint32>(nWindowSize);

    return BuildPacket(stPacketHeader,arrPayload);
}

//解析JSON Packet是否成功
static bool ParseJsonPacketPayload(const QByteArray &arrPayload, QJsonObject &oJsonObject, QString &strErrorMessage)
{
    QJsonParseError oParseError;
    const QJsonDocument oJsonDocument = QJsonDocument::fromJson(arrPayload, &oParseError);
    if (oParseError.error != QJsonParseError::NoError || !oJsonDocument.isObject())
    {
        strErrorMessage = QStringLiteral("JSON 解析失败：%1").arg(oParseError.errorString());
        return false;
    }

    oJsonObject = oJsonDocument.object();
    return true;
}


bool ReliablePacketCodec::ParseMetadataPayload(const QByteArray &arrPayload, SReliableFileMetadata &stMetadata, QString &strErrorMessage)
{
    QJsonObject oJsonObject;
    if(!ParseJsonPacketPayload(arrPayload,oJsonObject,strErrorMessage)){
        return false;
    }

    stMetadata.m_strSessionKey = oJsonObject.value(QStringLiteral("sessionKey")).toString();
    stMetadata.m_strFileName = oJsonObject.value(QStringLiteral("fileName")).toString();
    stMetadata.m_nFileSize = oJsonObject.value(QStringLiteral("fileSize")).toString().toLongLong();
    stMetadata.m_nSegmentSize = oJsonObject.value(QStringLiteral("segmentSize")).toInt();
    stMetadata.m_nTotalPacketCount = static_cast<quint32>(oJsonObject.value(QStringLiteral("totalPacketCount")).toInt());
    stMetadata.m_strMd5 = oJsonObject.value(QStringLiteral("md5")).toString();
    stMetadata.m_strSha256 = oJsonObject.value(QStringLiteral("sha256")).toString();

    if (stMetadata.m_strSessionKey.isEmpty() ||
        stMetadata.m_strFileName.isEmpty() ||
        stMetadata.m_nFileSize <= 0 ||
        stMetadata.m_nSegmentSize <= 0 ||
        stMetadata.m_nTotalPacketCount == 0)
    {
        strErrorMessage = QStringLiteral("元信息字段非法。");
        return false;
    }

    return  true;
}

QByteArray ReliablePacketCodec::BuildResumeStatePacket(const SReliableFileMetadata &stMetadata, const QList<SReliablePacketRange> &arrMissingRangeList, quint32 nReceivedPacketCount)
{
    QJsonObject oJsonObject;
    oJsonObject.insert(QStringLiteral("receivedPacketCount"),static_cast<int>(nReceivedPacketCount));
    oJsonObject.insert(QStringLiteral("missingRanges"),BuildRangeJsonArray(arrMissingRangeList));

    const QByteArray arrPayload = BuildJsonPacketPayload(oJsonObject);

    SReliablePacketHeader stPacketHeader;
    stPacketHeader.m_oTransferId = stMetadata.m_oTransferId;
    stPacketHeader.m_nMessageType = static_cast<quint16>(EReliableMessageType::ResumeStatePacket);
    stPacketHeader.m_nPayloadSize = static_cast<quint32>(arrPayload.size());
    stPacketHeader.m_nTotalPacketCount = stMetadata.m_nTotalPacketCount;
    stPacketHeader.m_nFileSize = static_cast<quint64>(stMetadata.m_nFileSize);

    return BuildPacket(stPacketHeader,arrPayload);
}

bool ReliablePacketCodec::ParseResumeStatePayload(const QByteArray &arrPayload, QList<SReliablePacketRange> &arrMissingRangeList, quint32 &nReceivedPacketCount, QString &strErrorMessage)
{
    QJsonObject oJsonObject;
    if(!ParseJsonPacketPayload(arrPayload,oJsonObject,strErrorMessage)){
        return false;
    }

    nReceivedPacketCount = static_cast<quint32>(oJsonObject.value(QStringLiteral("receivedPacketCount")).toInt());
    arrMissingRangeList = ParseRangeJsonArray(oJsonObject.value(QStringLiteral("missingRanges")).toArray());
    return true;
}

QByteArray ReliablePacketCodec::BuildDataPacket(const SReliableFileMetadata &stMetadata, quint64 nPacketSequence, quint64 nPayloadOffset, const QByteArray &arrPayload)
{
    SReliablePacketHeader stPacketHeader;
    stPacketHeader.m_oTransferId = stMetadata.m_oTransferId;
    stPacketHeader.m_nMessageType = static_cast<quint16>(EReliableMessageType::DataPacket);
    stPacketHeader.m_nPacketSequence = nPacketSequence;
    stPacketHeader.m_nPayloadOffset = nPayloadOffset;
    stPacketHeader.m_nPayloadSize = static_cast<quint32>(arrPayload.size());
    stPacketHeader.m_nTotalPacketCount = stMetadata.m_nTotalPacketCount;
    stPacketHeader.m_nFileSize = static_cast<quint64>(stMetadata.m_nFileSize);

    return BuildPacket(stPacketHeader,arrPayload);
}

QByteArray ReliablePacketCodec::BuildAckPacket(const SReliableFileMetadata &stMetadata, quint64 nPacketSequence)
{
    SReliablePacketHeader stPacketHeader;
    stPacketHeader.m_oTransferId = stMetadata.m_oTransferId;
    stPacketHeader.m_nMessageType = static_cast<quint16>(EReliableMessageType::AckPacket);
    stPacketHeader.m_nPacketSequence = nPacketSequence;
    stPacketHeader.m_nTotalPacketCount = stMetadata.m_nTotalPacketCount;
    stPacketHeader.m_nFileSize = static_cast<quint64>(stMetadata.m_nFileSize);

    return  BuildPacket(stPacketHeader,QByteArray());//包里并没有数据
}

QByteArray ReliablePacketCodec::BuildCompleteRequestPacket(const SReliableFileMetadata &stMetadata)
{
    SReliablePacketHeader stPacketHeader;
    stPacketHeader.m_oTransferId = stMetadata.m_oTransferId;
    stPacketHeader.m_nMessageType = static_cast<quint16>(EReliableMessageType::CompleteRequestPacket);
    stPacketHeader.m_nTotalPacketCount = stMetadata.m_nTotalPacketCount;
    stPacketHeader.m_nFileSize = static_cast<quint64>(stMetadata.m_nFileSize);

    return BuildPacket(stPacketHeader,QByteArray());//包里并没有数据
}

QByteArray ReliablePacketCodec::BuildResultPacket(const SReliableFileMetadata &stMetadata, bool bSuccess, const QString &strMessage, const QList<SReliablePacketRange> &arrMissingRangeList)
{
    QJsonObject oJsonObject;
    oJsonObject.insert(QStringLiteral("success"), bSuccess);
    oJsonObject.insert(QStringLiteral("message"), strMessage);
    oJsonObject.insert(QStringLiteral("missingRanges"), BuildRangeJsonArray(arrMissingRangeList));

    const QByteArray arrPayload = BuildJsonPacketPayload(oJsonObject);

    SReliablePacketHeader stPacketHeader;
    stPacketHeader.m_oTransferId = stMetadata.m_oTransferId;
    stPacketHeader.m_nMessageType = static_cast<quint16>(EReliableMessageType::ResultPacket);
    stPacketHeader.m_nPayloadSize = static_cast<quint32>(arrPayload.size());
    stPacketHeader.m_nTotalPacketCount = stMetadata.m_nTotalPacketCount;
    stPacketHeader.m_nFileSize = static_cast<quint64>(stMetadata.m_nFileSize);

    return  BuildPacket(stPacketHeader,arrPayload);
}

QByteArray ReliablePacketCodec::BuildErrorPacket(const SReliableFileMetadata &stMetadata, const QString &strMessage)
{
    const QByteArray arrPayload = strMessage.toUtf8();

    SReliablePacketHeader stPacketHeader;
    stPacketHeader.m_oTransferId = stMetadata.m_oTransferId;
    stPacketHeader.m_nMessageType = static_cast<quint16>(EReliableMessageType::ErrorPacket);
    stPacketHeader.m_nPayloadSize = static_cast<quint32>(arrPayload.size());
    stPacketHeader.m_nTotalPacketCount = stMetadata.m_nTotalPacketCount;
    stPacketHeader.m_nFileSize = static_cast<quint64>(stMetadata.m_nFileSize);

    return BuildPacket(stPacketHeader,arrPayload);
}

bool ReliablePacketCodec::ParseResultPayload(const QByteArray &arrPayload, bool &bSuccess, QString &strMessage, QList<SReliablePacketRange> &arrMissingRangeList, QString &strErrorMessage)
{
    QJsonObject oJsonObject;
    if(!ParseJsonPacketPayload(arrPayload,oJsonObject,strErrorMessage))
    {
        return false;
    }
    bSuccess = oJsonObject.value(QStringLiteral("success")).toBool();
    strMessage = oJsonObject.value(QStringLiteral("message")).toString();
    arrMissingRangeList = ParseRangeJsonArray(oJsonObject.value(QStringLiteral("missingRanges")).toArray());

    return true;
}


