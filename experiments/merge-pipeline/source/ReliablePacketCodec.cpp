#include "ReliablePacketCodec.h"
#include "ReliableTransferUtils.h"

#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <limits>

/**
 * @brief ReliablePacketCodec::ReliablePacketCodec
 * @param 无
 * @return 无
 * @details 保留原有构造接口；下面的静态函数仍可直接通过类名调用。
 */
ReliablePacketCodec::ReliablePacketCodec()
{

}

/**
 * @brief ReliablePacketCodec::GetHeaderLength
 * @param 无
 * @return 可靠传输协议固定包头长度。
 * @details 返回当前长连接可靠协议使用的固定头长度。
 */
int ReliablePacketCodec::GetHeaderLength()
{
    return  g_nReliablePacketHeaderLength;
}

/**
 * @brief ReliablePacketCodec::BuildPacket
 * @param stPacketHeader 需要编码的包头。
 * @param arrPayload 需要附加的负载数据。
 * @return 完整网络包。
 * @details 把可靠传输头和负载合成为可直接收发的包。
 */
QByteArray ReliablePacketCodec::BuildPacket(const SReliablePacketHeader &stPacketHeader, const QByteArray &arrPayload)
{
    QByteArray arrHeaderData;
    arrHeaderData.fill('\0', GetHeaderLength()); // v3 明确写入 62 字节，不保留旧格式的预留字段和尾部填充

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
                << stPacketHeader.m_nHeaderLength;
    oDataStream.writeRawData(arrTransferId.constData(), arrTransferId.size()); //DataStream 在序列化 QByteArray 时，不只写内容，还会先写长度,所以需要这样写，确保只写入的是内容
    oDataStream << stPacketHeader.m_nPacketSequence
                << stPacketHeader.m_nPayloadOffset
                << stPacketHeader.m_nPayloadSize
                << stPacketHeader.m_nTotalPacketCount
                << stPacketHeader.m_nFileSize
                << stPacketHeader.m_nWindowSize;

    return arrHeaderData + arrPayload;
}

//build JSON Packet
/**
 * @brief BuildJsonPacketPayload
 * @param oJsonObject 待编码的 JSON 对象。
 * @return 紧凑 JSON 字节数组。
 * @details 保留原有 JSON 编码方式，减少控制包中的空白字符。
 */
static QByteArray BuildJsonPacketPayload(const QJsonObject &oJsonObject)
{
    return QJsonDocument(oJsonObject).toJson(QJsonDocument::Compact);//设置为紧凑类型的
}


/**
 * @brief ReliablePacketCodec::ParsePacketHeader
 * @param arrHeaderData 固定长度包头字节流。
 * @param stPacketHeader 输出的包头结构。
 * @param strErrorMessage 输出错误信息。
 * @return 成功返回 true，否则返回 false。
 * @details 将 62 字节固定包头解析回结构化字段，并校验魔数、版本、类型、传输 ID 和载荷上限。
 */
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
                >> stPacketHeader.m_nHeaderLength;
    oDataStream.readRawData(arrTransferId.data(), arrTransferId.size());
    oDataStream >> stPacketHeader.m_nPacketSequence
                >> stPacketHeader.m_nPayloadOffset
                >> stPacketHeader.m_nPayloadSize
                >> stPacketHeader.m_nTotalPacketCount
                >> stPacketHeader.m_nFileSize
                >> stPacketHeader.m_nWindowSize;
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

    if (oDataStream.status() != QDataStream::Ok ||
        stPacketHeader.m_nVersion != g_nReliablePacketVersion ||
        stPacketHeader.m_nMessageType < 101 || stPacketHeader.m_nMessageType > 107 ||
        stPacketHeader.m_oTransferId.isNull() ||
        stPacketHeader.m_nPayloadSize > static_cast<quint32>(g_nReliableMaxPayloadSize))
    {
        strErrorMessage = QStringLiteral("协议版本、类型、传输 ID 或载荷长度非法。");
        return false;
    }
    const EReliableMessageType eType = static_cast<EReliableMessageType>(stPacketHeader.m_nMessageType);
    if (((eType == EReliableMessageType::AckPacket || eType == EReliableMessageType::CompleteRequestPacket) &&
         stPacketHeader.m_nPayloadSize != 0) ||
        (eType == EReliableMessageType::DataPacket &&
         stPacketHeader.m_nPayloadSize > static_cast<quint32>(g_nReliableMaxSegmentSize)))
    {
        strErrorMessage = QStringLiteral("消息载荷长度与类型不符。");
        return false;
    }
    return true;
}

/**
 * @brief ReliablePacketCodec::TryTakePacket
 * @param arrBuffer 当前连接独立维护的接收缓存。
 * @param stHeader 输出解析后的包头。
 * @param arrPayload 输出当前完整包的载荷。
 * @param bReady 输出是否已经取得一个完整包。
 * @param strErrorMessage 输出非法包的原因。
 * @return 缓存中的协议格式有效返回 true，格式非法返回 false。
 * @details true 且 bReady 为 false 表示半包，保留缓存等待后续字节；完整包只移除自身，后续粘连包继续保留。
 */
bool ReliablePacketCodec::TryTakePacket(QByteArray &arrBuffer, SReliablePacketHeader &stHeader,
                                        QByteArray &arrPayload, bool &bReady, QString &strErrorMessage)
{
    bReady = false;
    if (arrBuffer.size() < GetHeaderLength())
    {//小于包头，半包
        return true;
    }
    if (!ParsePacketHeader(arrBuffer.left(GetHeaderLength()), stHeader, strErrorMessage))
    {//return False 表示解析失败
        return false;
    }
    const qint64 nLength = GetHeaderLength() + static_cast<qint64>(stHeader.m_nPayloadSize);
    if (arrBuffer.size() < nLength)
    {//小于整个完整包，半包
        return true;
    }
    arrPayload = arrBuffer.mid(GetHeaderLength(), static_cast<int>(stHeader.m_nPayloadSize));
    //pos：从哪个位置开始截取，从 0 开始计数。
//    len：截取多少个字节。
    arrBuffer.remove(0, static_cast<int>(nLength));
    bReady = true;
    return true;
}

/**
 * @brief ReliablePacketCodec::BuildSessionKey
 * @param stMetadata 已设置文件 SHA-256、分片大小和实际通道数的元信息。
 * @return 用于定位历史续传状态的稳定 SHA-256 会话键。
 * @details 对 v3、文件名、文件大小、分片大小、实际通道数和文件 SHA-256 拼接后的 UTF-8 字节计算摘要。
 */
QString ReliablePacketCodec::BuildSessionKey(const SReliableFileMetadata &stMetadata)
{
    const QByteArray arrSource = QStringLiteral("v3|%1|%2|%3|%4|%5")
        .arg(stMetadata.m_strFileName).arg(stMetadata.m_nFileSize).arg(stMetadata.m_nSegmentSize)
        .arg(stMetadata.m_nLaneCount).arg(stMetadata.m_strSha256).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(arrSource, QCryptographicHash::Sha256).toHex());
}

/**
 * @brief ReliablePacketCodec::ValidateMetadata
 * @param stMetadata 待校验的文件元信息。
 * @param strErrorMessage 输出校验失败原因。
 * @return 文件名、大小、分片数、通道数、摘要和会话键合法返回 true。
 * @details 不校验 UUID，因为 Metadata 载荷中不保存 UUID，调用方从包头读取并关联当前任务。
 */
bool ReliablePacketCodec::ValidateMetadata(const SReliableFileMetadata &stMetadata, QString &strErrorMessage)
{
    const QRegularExpression oShaPattern(QStringLiteral("^[0-9a-f]{64}$"));
    if (stMetadata.m_strFileName.isEmpty() || stMetadata.m_strFileName == QStringLiteral(".") ||
        stMetadata.m_strFileName == QStringLiteral("..") || stMetadata.m_strFileName.contains('/') ||
        stMetadata.m_strFileName.contains('\\') || stMetadata.m_strFileName.contains(':') ||
        stMetadata.m_strFileName.contains(QChar(0)) ||
        stMetadata.m_strFileName.endsWith('.') || stMetadata.m_strFileName.endsWith(' ') ||
        stMetadata.m_nFileSize <= 0 || stMetadata.m_nSegmentSize < 8 * 1024 ||
        stMetadata.m_nSegmentSize > g_nReliableMaxSegmentSize ||
        stMetadata.m_nLaneCount < 1 || stMetadata.m_nLaneCount > g_nReliableMaxThreadCount ||
        static_cast<quint32>(stMetadata.m_nLaneCount) > stMetadata.m_nTotalPacketCount ||
        stMetadata.m_strSha256.size() != 64 || !oShaPattern.match(stMetadata.m_strSha256).hasMatch())
    {
        strErrorMessage = QStringLiteral("文件名、大小、分片大小或摘要非法（暂不支持空文件）。");
        return false;
    }
    //【公共计算提取】与发送端 BuildMetadata 使用同一个总分片数计算。
    const qint64 nCount = GetReliablePacketCount(
        stMetadata.m_nFileSize,
        stMetadata.m_nSegmentSize);
    if (nCount > std::numeric_limits<int>::max() || nCount != stMetadata.m_nTotalPacketCount ||
        stMetadata.m_strSessionKey != BuildSessionKey(stMetadata))
    {
        strErrorMessage = QStringLiteral("分片总数或续传会话键不匹配。");
        return false;
    }
    return true;
}

/**
 * @brief ParseMissingRanges
 * @param oValue JSON 缺失范围数组。
 * @param arrRanges 输出缺失分片闭区间列表。
 * @param strErrorMessage 输出范围错误原因。
 * @return 所有范围格式和边界合法返回 true。
 * @details 检查端点转换、区间顺序和重叠；替代旧的宽松 ParseRangeJsonArray，不能将解析失败当作序号 0。
 */
static bool ParseMissingRanges(const QJsonValue &oValue, QList<SReliablePacketRange> &arrRanges,
                                QString &strErrorMessage)
{
    arrRanges.clear();
    if (!oValue.isArray())
    {
        strErrorMessage = QStringLiteral("缺失范围必须为数组。");
        return false;
    }
    for (const QJsonValue &oItem : oValue.toArray())
    {
        const QJsonArray oRange = oItem.toArray();
        bool bStartOk = false;
        bool bEndOk = false;
        const quint64 nStart = oRange.size() == 2 ? oRange.at(0).toString().toULongLong(&bStartOk) : 0; //将字符串转换为无符号 64 位整数，并通过 bStartOk 告知转换是否成功
        const quint64 nEnd = oRange.size() == 2 ? oRange.at(1).toString().toULongLong(&bEndOk) : 0;
        if (!bStartOk || !bEndOk || nStart > nEnd || nEnd >= static_cast<quint64>(std::numeric_limits<int>::max()) ||
            (!arrRanges.isEmpty() && nStart <= arrRanges.last().m_nEndSequence))
        {
            strErrorMessage = QStringLiteral("缺失范围的端点、顺序或重叠关系非法。");
            return false;
        }
        arrRanges.append(SReliablePacketRange(nStart, nEnd));
    }
    return true;
}

/**
 * @brief ReliablePacketCodec::BuildMetadataPacket
 * @param stMetadata 文件元信息。
 * @param nWindowSize 发送端声明的每条数据连接的窗口大小。
 * @param nLaneIndex 本连接的通道编号；-1 为控制连接，0..N-1 为数据连接。
 * @return 元信息控制包。
 * @details 构造会话建立或断点续传恢复握手包。
 */
QByteArray ReliablePacketCodec::BuildMetadataPacket(const SReliableFileMetadata &stMetadata, int nWindowSize, int nLaneIndex)
{
    QJsonObject oJsonObject;
    oJsonObject.insert(QStringLiteral("sessionKey"), stMetadata.m_strSessionKey);
    oJsonObject.insert(QStringLiteral("fileName"), stMetadata.m_strFileName);
    oJsonObject.insert(QStringLiteral("fileSize"), QString::number(stMetadata.m_nFileSize));
    oJsonObject.insert(QStringLiteral("segmentSize"), stMetadata.m_nSegmentSize);
    oJsonObject.insert(QStringLiteral("totalPacketCount"), static_cast<int>(stMetadata.m_nTotalPacketCount));
    oJsonObject.insert(QStringLiteral("laneCount"), stMetadata.m_nLaneCount);
    oJsonObject.insert(QStringLiteral("laneIndex"), nLaneIndex);
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
/**
 * @brief ParseJsonPacketPayload
 * @param arrPayload JSON 字节数组。
 * @param oJsonObject 输出解析后的对象。
 * @param strErrorMessage 输出格式错误原因。
 * @return 合法 JSON 对象返回 true。
 * @details 拒绝语法错误及非对象类型的 JSON 载荷。
 */
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


/**
 * @brief ReliablePacketCodec::ParseMetadataPayload
 * @param arrPayload 元信息 JSON。
 * @param stMetadata 输出的文件元信息。
 * @param pLaneIndex 可选输出本连接的通道编号，不需要时可省略。
 * @param strErrorMessage 输出错误信息。
 * @return 成功返回 true，否则返回 false。
 * @details 将元信息 JSON 负载还原成可靠传输会话元数据。
 */
bool ReliablePacketCodec::ParseMetadataPayload(const QByteArray &arrPayload, SReliableFileMetadata &stMetadata, QString &strErrorMessage, int *pLaneIndex)
{
    QJsonObject oJsonObject;
    if(!ParseJsonPacketPayload(arrPayload,oJsonObject,strErrorMessage)){
        return false;
    }

    stMetadata.m_strSessionKey = oJsonObject.value(QStringLiteral("sessionKey")).toString();
    stMetadata.m_strFileName = oJsonObject.value(QStringLiteral("fileName")).toString();
    bool bSizeOk = false;
    stMetadata.m_nFileSize = oJsonObject.value(QStringLiteral("fileSize")).toString().toLongLong(&bSizeOk); // 检查字符串转整数是否成功
    stMetadata.m_nSegmentSize = oJsonObject.value(QStringLiteral("segmentSize")).toInt();
    const int nPacketCount = oJsonObject.value(QStringLiteral("totalPacketCount")).toInt(-1);
    stMetadata.m_nTotalPacketCount = nPacketCount > 0 ? static_cast<quint32>(nPacketCount) : 0;
    stMetadata.m_nLaneCount = oJsonObject.value(QStringLiteral("laneCount")).toInt(0);
    const int nLaneIndex = oJsonObject.value(QStringLiteral("laneIndex")).toInt(-2);
    if (nLaneIndex < -1 || nLaneIndex >= stMetadata.m_nLaneCount)
    {
        strErrorMessage = QStringLiteral("数据通道编号非法，-1 仅用于控制连接。");
        return false;
    }
    stMetadata.m_strSha256 = oJsonObject.value(QStringLiteral("sha256")).toString();

    if (!bSizeOk || stMetadata.m_strSessionKey.isEmpty() ||
        stMetadata.m_strFileName.isEmpty() ||
        stMetadata.m_nFileSize <= 0 ||
        stMetadata.m_nSegmentSize <= 0 ||
        stMetadata.m_nTotalPacketCount == 0)
    {
        strErrorMessage = QStringLiteral("元信息字段非法。");
        return false;
    }

    if (!ValidateMetadata(stMetadata, strErrorMessage))
    {
        return false;
    }
    if (pLaneIndex != nullptr)
    {
        *pLaneIndex = nLaneIndex;
    }
    return true;
}

/**
 * @brief ReliablePacketCodec::BuildResumeStatePacket
 * @param stMetadata 文件元信息。
 * @param arrMissingRangeList 接收端尚缺失的分片范围。
 * @param nReceivedPacketCount 已接收分片数。
 * @return 恢复状态包。
 * @details 接收端把当前缺失分片窗口回传给发送端。
 */
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

/**
 * @brief ReliablePacketCodec::ParseResumeStatePayload
 * @param arrPayload 恢复状态 JSON。
 * @param arrMissingRangeList 输出的缺失分片范围。
 * @param nReceivedPacketCount 输出的已接收分片数。
 * @param strErrorMessage 输出错误信息。
 * @return 成功返回 true，否则返回 false。
 * @details 解析接收端返回的断点续传缺失信息。
 */
bool ReliablePacketCodec::ParseResumeStatePayload(const QByteArray &arrPayload, QList<SReliablePacketRange> &arrMissingRangeList, quint32 &nReceivedPacketCount, QString &strErrorMessage)
{
    QJsonObject oJsonObject;
    if(!ParseJsonPacketPayload(arrPayload,oJsonObject,strErrorMessage)){
        return false;
    }

    const int nCount = oJsonObject.value(QStringLiteral("receivedPacketCount")).toInt(-1);
    if (nCount < 0)
    {
        strErrorMessage = QStringLiteral("已接收分片数非法。");
        return false;
    }
    nReceivedPacketCount = static_cast<quint32>(nCount);
    return ParseMissingRanges(oJsonObject.value(QStringLiteral("missingRanges")), arrMissingRangeList, strErrorMessage);
}

/**
 * @brief ReliablePacketCodec::BuildDataPacket
 * @param stMetadata 文件元信息。
 * @param nPacketSequence 当前分片序号。
 * @param nPayloadOffset 当前负载在文件中的偏移量。
 * @param arrPayload 分片内容。
 * @return 数据分片包。
 * @details 构造带偏移和分片序号的可靠数据包。
 */
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

/**
 * @brief ReliablePacketCodec::BuildAckPacket
 * @param stMetadata 文件元信息。
 * @param nPacketSequence 已确认收到的分片序号。
 * @return ACK 控制包。
 * @details 接收端对每个已写入临时文件且更新内存位图的分片进行确认；ACK 不保证状态文件已保存或物理介质持久化。
 */
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

/**
 * @brief ReliablePacketCodec::BuildCompleteRequestPacket
 * @param stMetadata 文件元信息。
 * @return 完成请求包。
 * @details 所有缺失分片补齐后，由控制连接通知接收端合并各通道临时文件并执行最终 SHA-256 校验。
 */
QByteArray ReliablePacketCodec::BuildCompleteRequestPacket(const SReliableFileMetadata &stMetadata)
{
    SReliablePacketHeader stPacketHeader;
    stPacketHeader.m_oTransferId = stMetadata.m_oTransferId;
    stPacketHeader.m_nMessageType = static_cast<quint16>(EReliableMessageType::CompleteRequestPacket);
    stPacketHeader.m_nTotalPacketCount = stMetadata.m_nTotalPacketCount;
    stPacketHeader.m_nFileSize = static_cast<quint64>(stMetadata.m_nFileSize);

    return BuildPacket(stPacketHeader,QByteArray());//包里并没有数据
}

/**
 * @brief ReliablePacketCodec::BuildResultPacket
 * @param stMetadata 文件元信息。
 * @param bSuccess 是否成功。
 * @param strMessage 结果描述。
 * @param arrMissingRangeList 缺失分片范围。
 * @return 结果控制包。
 * @details 接收端在校验或完成阶段回传成功、失败或缺失清单。
 */
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

/**
 * @brief ReliablePacketCodec::BuildErrorPacket
 * @param stMetadata 文件元信息。
 * @param strMessage 错误描述。
 * @return 错误控制包。
 * @details 把协议级异常封装成错误包返回给对端。
 */
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

/**
 * @brief ReliablePacketCodec::ParseResultPayload
 * @param arrPayload 结果 JSON。
 * @param bSuccess 输出成功标记。
 * @param strMessage 输出结果消息。
 * @param arrMissingRangeList 输出缺失分片范围。
 * @param strErrorMessage 输出错误信息。
 * @return 成功返回 true，否则返回 false。
 * @details 解析接收端返回的最终校验或补传结果。
 */
bool ReliablePacketCodec::ParseResultPayload(const QByteArray &arrPayload, bool &bSuccess, QString &strMessage, QList<SReliablePacketRange> &arrMissingRangeList, QString &strErrorMessage)
{
    QJsonObject oJsonObject;
    if(!ParseJsonPacketPayload(arrPayload,oJsonObject,strErrorMessage))
    {
        return false;
    }
    if (!oJsonObject.value(QStringLiteral("success")).isBool() ||
        !oJsonObject.value(QStringLiteral("message")).isString())
    {
        strErrorMessage = QStringLiteral("结果包字段类型非法。");
        return false;
    }
    bSuccess = oJsonObject.value(QStringLiteral("success")).toBool();
    strMessage = oJsonObject.value(QStringLiteral("message")).toString();
    return ParseMissingRanges(oJsonObject.value(QStringLiteral("missingRanges")), arrMissingRangeList, strErrorMessage);
}


