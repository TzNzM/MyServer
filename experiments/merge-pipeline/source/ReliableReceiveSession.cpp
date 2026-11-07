#include "ReliableReceiveSession.h"
#include "ReliableSha256.h"
#include "ReliableMergeHasher.h"
#include "ReliableTransferUtils.h"
#include "ReliablePacketCodec.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QDataStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QSaveFile>
#include <QElapsedTimer>
#include <QScopedPointer>

namespace
{
//【合并计时】串行方式保留原计时；流水线的摘要耗时与读写重叠，不能重复相加。
//放在函数最外层，成功、失败、取消时都能在返回前生成一条汇总。
struct SFinalizeTiming
{
    QElapsedTimer m_oClock;
    qint64 m_nWaitLockNs = 0;
    qint64 m_nReadNs = 0;
    qint64 m_nWriteNs = 0;
    qint64 m_nHashNs = 0;
    qint64 m_nBufferWaitNs = 0;
    qint64 m_nDrainNs = 0;
    qint64 m_nCommitNs = 0;
    qint64 m_nRenameNs = 0;
    qint64 m_nCleanupNs = 0;
    qint64 m_nMergedBytes = 0;
    qint64 m_nFileSize;
    QString m_strFileName;
    QString m_strResult = QStringLiteral("失败或取消");
    QString *m_pMessage;
    bool m_bPipeline;

    /**
     * @brief SFinalizeTiming
     * @param stMetadata 创建后不变的文件元信息。
     * @param pMessage 可选的汇总输出地址，调用期间必须有效。
     * @param bPipeline 是否使用合并流水线。
     * @return 无
     * @details 在取得会话锁前开始计时，以便单独统计等待锁的时间。
     */
    SFinalizeTiming(const SReliableFileMetadata &stMetadata, QString *pMessage, bool bPipeline)
        : m_nFileSize(stMetadata.m_nFileSize),
          m_strFileName(stMetadata.m_strFileName),
          m_pMessage(pMessage),
          m_bPipeline(bPipeline)
    {
        m_oClock.start();
    }

    /**
     * @brief ~SFinalizeTiming
     * @param 无
     * @return 无
     * @details 纳秒累计值统一转为毫秒，其他项包含打开文件、分配缓冲区、
     *          检查及资源析构等未单独列出的耗时；不执行额外读写或同步。
     */
    ~SFinalizeTiming()
    {
        if (m_pMessage == nullptr)
        {
            return;
        }
        const qint64 nTotalNs = m_oClock.nsecsElapsed();
        const qint64 nOtherNs = qMax<qint64>(0, nTotalNs - m_nWaitLockNs
            - m_nReadNs - m_nWriteNs - (m_bPipeline ? 0 : m_nHashNs)
            - m_nBufferWaitNs - m_nDrainNs - m_nCommitNs
            - m_nRenameNs - m_nCleanupNs);
        const auto fnMs = [](qint64 nNs)
        {
            return QString::number(nNs / 1000000.0, 'f', 1);
        };
        if (m_bPipeline)
        {
            *m_pMessage = QStringLiteral(
                "[合并流水线计时] 模式=流水线，文件=%1，结果=%2，已合并=%3/%4 字节，"
                "总计=%5 ms，等锁=%6 ms，读取=%7 ms，写入=%8 ms，等空闲缓冲=%9 ms，"
                "等摘要收尾=%10 ms，SHA-256（并行，不计入相加）=%11 ms，commit=%12 ms，"
                "改名=%13 ms，清理=%14 ms，其他=%15 ms")
                .arg(m_strFileName).arg(m_strResult).arg(m_nMergedBytes).arg(m_nFileSize)
                .arg(fnMs(nTotalNs)).arg(fnMs(m_nWaitLockNs)).arg(fnMs(m_nReadNs))
                .arg(fnMs(m_nWriteNs)).arg(fnMs(m_nBufferWaitNs)).arg(fnMs(m_nDrainNs))
                .arg(fnMs(m_nHashNs)).arg(fnMs(m_nCommitNs)).arg(fnMs(m_nRenameNs))
                .arg(fnMs(m_nCleanupNs)).arg(fnMs(nOtherNs));
            return;
        }
        *m_pMessage = QStringLiteral(
            "[合并计时] 模式=串行，文件=%1，结果=%2，已合并=%3/%4 字节，"
            "总计=%5 ms，等锁=%6 ms，读取=%7 ms，写入=%8 ms，"
            "SHA-256=%9 ms，commit=%10 ms，改名=%11 ms，清理=%12 ms，其他=%13 ms")
            .arg(m_strFileName).arg(m_strResult).arg(m_nMergedBytes).arg(m_nFileSize)
            .arg(fnMs(nTotalNs)).arg(fnMs(m_nWaitLockNs)).arg(fnMs(m_nReadNs))
            .arg(fnMs(m_nWriteNs)).arg(fnMs(m_nHashNs)).arg(fnMs(m_nCommitNs))
            .arg(fnMs(m_nRenameNs)).arg(fnMs(m_nCleanupNs)).arg(fnMs(nOtherNs));
    }
};

//【合并流水线】先结束摘要线程再读取统计；异常或提前 return 也执行。
//该对象先于 QScopedPointer 析构，缓冲区始终在线程退出后才释放。
struct SMergeHashJoin
{
    ReliableMergeHasher *m_pHasher;
    qint64 &m_nHashNs;

    ~SMergeHashJoin()
    {
        if (m_pHasher != nullptr)
        {
            m_pHasher->CancelAndWait();
            m_nHashNs = m_pHasher->GetHashTimeNs();
        }
    }
};
}

ReliableReceiveSession::ReliableReceiveSession(
    const SReliableFileMetadata &stMetadata,
    const QString &strOutputDirectory,
    const QSharedPointer<QAtomicInt> &pStopFlag)
    : m_stState(),
      m_strOutputDirectory(QDir(strOutputDirectory).absolutePath()),
      m_pStopFlag(pStopFlag)
{
    m_stState.m_stMetadata = stMetadata;
}

SReliableFileMetadata ReliableReceiveSession::GetMetadata() const
{
    return m_stState.m_stMetadata;
}

bool ReliableReceiveSession::PersistSessionState(QString &strErrorMessage)
{
    QMutexLocker oLocker(&m_oMutex);//QMutexLocker 创建时加锁，离开作用域时自动解锁，包括通过 return 返回的情况。

    return PersistSessionStateLocked(strErrorMessage);
}

QString ReliableReceiveSession::GetLaneFilePath(int nLaneIndex) const
{

//    nLaneIndex	要填入的数字	3
//    4	字段最小宽度为 4	0003
//    10	使用十进制表示	3 而不是十六进制 3
//    QChar('0')	不足宽度时，用字符 0 补齐	0003
    const QString strFileName = QStringLiteral("lane_%1.part")
                                    .arg(nLaneIndex, 4, 10, QChar('0'));

    return QDir(m_stState.m_strSessionDirectory)
        .filePath(strFileName);
}


QString ReliableReceiveSession::BuildAvailableFilePath(
    const QString &strPreferredFilePath)
{
    if (!QFileInfo::exists(strPreferredFilePath))
    {//如果该文件路径不存在，直接返回就行
        return strPreferredFilePath;
    }

    //如果存在，就必须特殊处理，找到一个不存在的路径，然后返回它
    const QFileInfo oFileInfo(strPreferredFilePath);
    const QString strBaseName = oFileInfo.completeBaseName();
    const QString strSuffix = oFileInfo.suffix();
    const QString strDirectory = oFileInfo.absolutePath();

    for (int nIndex = 1; nIndex < 10000; ++nIndex)
    {
        const QString strFileName = strSuffix.isEmpty()
            ? QStringLiteral("%1_%2")
                  .arg(strBaseName).arg(nIndex)
            : QStringLiteral("%1_%2.%3")
                  .arg(strBaseName).arg(nIndex).arg(strSuffix);

        const QString strCandidatePath =
            QDir(strDirectory).filePath(strFileName);

        if (!QFileInfo::exists(strCandidatePath))
        {
            return strCandidatePath;
        }
    }

    return QString(); //失败时返回空字符串
}

//Locked 只是命名约定，不会自动加锁。 外部调用 PersistSessionState()；类内部已经持锁时，才调用 PersistSessionStateLocked()。
bool ReliableReceiveSession::PersistSessionStateLocked(
    QString &strErrorMessage)
{
    if (m_stState.m_bCompleted)
    {//已完成的会话不再重新生成状态文件
        return true;
    }

    const SReliableFileMetadata &stMetadata =
        m_stState.m_stMetadata;

    //把接收位图序列化成字节数组
    QByteArray arrBytes;
    QDataStream oDataStream(&arrBytes, QIODevice::WriteOnly);
    oDataStream.setVersion(QDataStream::Qt_5_12);
    oDataStream << m_stState.m_oReceivedPacketMap;

    QJsonObject oJsonObject;

    oJsonObject.insert(QStringLiteral("stateVersion"), 3);
    oJsonObject.insert(QStringLiteral("sessionKey"),
                       stMetadata.m_strSessionKey);
    oJsonObject.insert(QStringLiteral("fileName"),
                       stMetadata.m_strFileName);
    oJsonObject.insert(QStringLiteral("fileSize"),
                       QString::number(stMetadata.m_nFileSize));
    oJsonObject.insert(QStringLiteral("segmentSize"),
                       stMetadata.m_nSegmentSize);
    oJsonObject.insert(QStringLiteral("totalPacketCount"),
                       static_cast<int>(
                           stMetadata.m_nTotalPacketCount));
    oJsonObject.insert(QStringLiteral("laneCount"),
                       stMetadata.m_nLaneCount);
    oJsonObject.insert(QStringLiteral("sha256"),
                       stMetadata.m_strSha256);
    oJsonObject.insert(QStringLiteral("finalFilePath"),
                       m_stState.m_strFinalFilePath);
    oJsonObject.insert(QStringLiteral("receivedPacketCount"),
                       static_cast<int>(
                           m_stState.m_nReceivedPacketCount));
    oJsonObject.insert(QStringLiteral("receivedPacketBitmap"),
                       QString::fromLatin1(arrBytes.toBase64()));

    const QByteArray arrJson =
        QJsonDocument(oJsonObject).toJson(QJsonDocument::Compact);
    //QSaveFile 先写临时文件，commit() 成功后再替换正式状态文件。 相比之前直接 Truncate 重写，能够避免写到一半就破坏原有状态文件。
    QSaveFile oStateFile(m_stState.m_strStateFilePath);

    if (!oStateFile.open(QIODevice::WriteOnly))
    {
        strErrorMessage = QStringLiteral("状态文件打开失败：%1")
                              .arg(oStateFile.errorString());
        return false;
    }

    if (oStateFile.write(arrJson) != arrJson.size())
    {
        strErrorMessage = QStringLiteral("状态文件写入失败：%1")
                              .arg(oStateFile.errorString());
        return false;
    }

    if (!oStateFile.commit())
    {
        strErrorMessage = QStringLiteral("状态文件提交失败：%1")
                              .arg(oStateFile.errorString());
        return false;
    }

    //只有保存成功，才能认为这些接收记录已经保存
    m_stState.m_nUnsavedPacketCount = 0;
    return true;
}


bool ReliableReceiveSession::LoadOrCreateSession(
    QString &strErrorMessage)
{
    //范围较大，但每个会话初始化一次，通常不在高频路径上
    QMutexLocker oLocker(&m_oMutex);

    const SReliableFileMetadata &stMetadata =
        m_stState.m_stMetadata;

    //先确认文件信息、分片数、通道数和会话键合法
    if (!ReliablePacketCodec::ValidateMetadata(
            stMetadata, strErrorMessage))
    {
        return false;
    }

    //根据完整会话键确定目录，不再只使用前 16 个字符
    m_stState.m_strSessionDirectory =
        QDir(m_strOutputDirectory).filePath(
            QStringLiteral("session_%1")
                .arg(stMetadata.m_strSessionKey));

    const QDir oSessionDirectory(
        m_stState.m_strSessionDirectory);

    //这里仅确定路径，assembled.part 留到最终合并时创建
    m_stState.m_strPartFilePath =
        oSessionDirectory.filePath(
            QStringLiteral("assembled.part"));

    m_stState.m_strStateFilePath =
        oSessionDirectory.filePath(
            QStringLiteral("state.json"));

    if (!QDir().mkpath(m_stState.m_strSessionDirectory))
    {
        strErrorMessage = QStringLiteral("会话目录创建失败。");
        return false;
    }

    //ValidateMetadata 已保证总分片数没有超过 int 上限
    const int nPacketCount =
        static_cast<int>(stMetadata.m_nTotalPacketCount);

    //【本次修改2】替换原来的阶段性保护：存在 state.json 时，先校验历史状态是否属于当前文件。
    const bool bHasStateFile =
        QFileInfo::exists(m_stState.m_strStateFilePath);

    if (bHasStateFile)
    {
        QFile oStateFile(m_stState.m_strStateFilePath);
        if (!oStateFile.open(QIODevice::ReadOnly))
        {
            strErrorMessage = QStringLiteral("续传状态打开失败：%1")
                                  .arg(oStateFile.errorString());
            return false;
        }

        //限制状态文件大小，避免损坏的文件造成无上限读取。
        const qint64 nMaxStateSize = 64 * 1024 * 1024;
        const QByteArray arrStateJson = oStateFile.read(nMaxStateSize + 1);
        if (oStateFile.error() != QFile::NoError ||
            arrStateJson.size() > nMaxStateSize)
        {
            strErrorMessage = QStringLiteral("续传状态读取失败或超过大小限制，原文件已保留。");
            return false;
        }

        QJsonParseError oParseError;
        const QJsonDocument oJsonDocument =
            QJsonDocument::fromJson(arrStateJson, &oParseError);
        const QJsonObject oJsonObject = oJsonDocument.object();

        bool bSizeOk = false;
        const qint64 nSavedFileSize =
            oJsonObject.value(QStringLiteral("fileSize"))
                .toString().toLongLong(&bSizeOk);

        if (oParseError.error != QJsonParseError::NoError ||
            !oJsonDocument.isObject() ||
            oJsonObject.value(QStringLiteral("stateVersion")).toInt() != 3 ||
            !bSizeOk || nSavedFileSize != stMetadata.m_nFileSize ||
            oJsonObject.value(QStringLiteral("sessionKey")).toString() != stMetadata.m_strSessionKey ||
            oJsonObject.value(QStringLiteral("fileName")).toString() != stMetadata.m_strFileName ||
            oJsonObject.value(QStringLiteral("segmentSize")).toInt() != stMetadata.m_nSegmentSize ||
            oJsonObject.value(QStringLiteral("totalPacketCount")).toInt() != nPacketCount ||
            oJsonObject.value(QStringLiteral("laneCount")).toInt() != stMetadata.m_nLaneCount ||
            oJsonObject.value(QStringLiteral("sha256")).toString() != stMetadata.m_strSha256)
        {
            strErrorMessage = QStringLiteral("续传状态与当前文件不匹配，原文件已保留。");
            return false;
        }

        //【本次修改3】恢复接收位图，并以实际置位数量重新统计进度，不直接相信 JSON 中的计数。
        const QByteArray arrBase64 =
            oJsonObject.value(QStringLiteral("receivedPacketBitmap"))
                .toString().toLatin1();
        const QByteArray arrBytes = QByteArray::fromBase64(arrBase64);

        //QBitArray 序列化数据的前四字节是位数；反序列化前先校验，避免异常长度导致大量分配。
        QDataStream oCountStream(arrBytes);
        oCountStream.setVersion(QDataStream::Qt_5_12);
        quint32 nSavedBitCount = 0;
        oCountStream >> nSavedBitCount;

        if (oCountStream.status() != QDataStream::Ok ||
            nSavedBitCount != static_cast<quint32>(nPacketCount))
        {
            strErrorMessage = QStringLiteral("续传位图长度不匹配，拒绝恢复。");
            return false;
        }

        QBitArray oReceivedPacketMap;
        QDataStream oDataStream(arrBytes);
        oDataStream.setVersion(QDataStream::Qt_5_12);
        oDataStream >> oReceivedPacketMap;

        if (arrBytes.toBase64() != arrBase64 ||
            oDataStream.status() != QDataStream::Ok ||
            !oDataStream.atEnd() ||
            oReceivedPacketMap.size() != nPacketCount)
        {
            strErrorMessage = QStringLiteral("续传位图损坏，拒绝恢复。");
            return false;
        }

        m_stState.m_oReceivedPacketMap = oReceivedPacketMap;
        //【位图计数统一】直接使用 Qt 接口，统计结果不改变位图。
        m_stState.m_nReceivedPacketCount =
            static_cast<quint32>(oReceivedPacketMap.count(true));
        m_stState.m_nUnsavedPacketCount = 0;
    }
    else
    {
        //没有历史状态，仍按首次接收初始化空位图和计数。
        m_stState.m_oReceivedPacketMap = QBitArray(nPacketCount, false);
        m_stState.m_nReceivedPacketCount = 0;
        m_stState.m_nUnsavedPacketCount = 0;
    }

    //为每个数据通道创建自己的临时文件
    for (int nLaneIndex = 0;
         nLaneIndex < stMetadata.m_nLaneCount;
         ++nLaneIndex)
    {
        //【通道范围提取】创建和恢复通道文件时，统一取得该通道的实际字节数。
        const SReliableLaneRange stLaneRange =
            GetReliableLaneRange(stMetadata, nLaneIndex);

        QFile oLaneFile(GetLaneFilePath(nLaneIndex));

        //【本次修改4】恢复时只检查通道文件，不创建或调整大小，避免破坏历史接收数据。
        if (bHasStateFile)
        {
            if (!oLaneFile.exists() || oLaneFile.size() != stLaneRange.m_nByteSize)
            {
                strErrorMessage =
                    QStringLiteral("通道 %1 临时文件缺失或大小不匹配，原文件已保留。")
                        .arg(nLaneIndex);
                return false;
            }

            continue;
        }

        if (!oLaneFile.open(QIODevice::ReadWrite) ||
            !oLaneFile.resize(stLaneRange.m_nByteSize))
        {//这里只是预分配这么多的地址
            strErrorMessage =
                QStringLiteral("通道 %1 临时文件创建失败：%2")
                    .arg(nLaneIndex)
                    .arg(oLaneFile.errorString());
            return false;
        }

        oLaneFile.close();
    }

    //【本次修改5】恢复时也按当前输出目录重新选择最终路径，不直接使用状态文件中保存的绝对路径。
    m_stState.m_strFinalFilePath =
        BuildAvailableFilePath(
            QDir(m_strOutputDirectory)
                .filePath(stMetadata.m_strFileName));

    if (m_stState.m_strFinalFilePath.isEmpty())
    {
        strErrorMessage =
            QStringLiteral("无法生成可用文件路径：重名候选已耗尽。");
        return false;
    }

    //当前已经持有会话锁，调用不重复加锁的内部版本
    return PersistSessionStateLocked(strErrorMessage);
}

QByteArray ReliableReceiveSession::BuildResumeStatePacket()
{
    QMutexLocker oLocker(&m_oMutex);

    const QList<SReliablePacketRange> arrMissingRangeList =
        BuildMissingRangesLocked();

    return ReliablePacketCodec::BuildResumeStatePacket(
        m_stState.m_stMetadata,
        arrMissingRangeList,
                m_stState.m_nReceivedPacketCount);
}



bool ReliableReceiveSession::AcceptDataPacket(
    const SReliablePacketHeader &stHeader,
    const QByteArray &arrPayload,
    int nLaneIndex,
    bool &bAccepted,
    bool &bNewPacket,
    QString &strWarningMessage,
    QString &strErrorMessage)
{
//    | 情况 | `bAccepted` | `bNewPacket` |
//    |---|---|---|
//    | 新分片写入成功 | `true` | `true` |
//    | 分片之前已收到 | `true` | `false` |
//    | 校验或写入失败 | `false` | `false` |

    bAccepted = false;
    bNewPacket = false;
    strWarningMessage.clear();
    strErrorMessage.clear();

    const SReliableFileMetadata &stMetadata =
        m_stState.m_stMetadata;

    const quint64 nSequence = stHeader.m_nPacketSequence;

    //第一段：校验分片序号、通道归属和包头信息。
    if (nSequence >= stMetadata.m_nTotalPacketCount)
    {
        strErrorMessage = QStringLiteral("分片序号越界。");
        return false;
    }

    if (nLaneIndex < 0 || nLaneIndex >= stMetadata.m_nLaneCount)
    {
        strErrorMessage = QStringLiteral("数据通道编号非法，控制连接不能发送数据分片。");
        return false;
    }

    //当前通道负责的全局分片区间：[nFirstSequence, nEndSequence)。
    //【通道范围提取】通道编号已通过上方检查，这里复用统一计算结果。
    const SReliableLaneRange stLaneRange =
            GetReliableLaneRange(stMetadata, nLaneIndex);

    if (nSequence < static_cast<quint64>(stLaneRange.m_nFirstSequence) ||
        nSequence >= static_cast<quint64>(stLaneRange.m_nEndSequence))
    {
        strErrorMessage = QStringLiteral("当前通道不能写入其他通道的分片。");
        return false;
    }

    //根据分片序号计算期望偏移，不能直接相信发送端给出的偏移。
    //【公共计算提取】末片长度与发送端共用同一个计算函数。
    const SReliablePacketLayout stLayout =
        GetReliablePacketLayout(stMetadata, nSequence);

    const qint64 nExpectedOffset = stLayout.m_nPayloadOffset;
    const int nExpectedSize = stLayout.m_nPayloadSize;

    if (stHeader.m_oTransferId != stMetadata.m_oTransferId ||
        stHeader.m_nPayloadOffset !=
            static_cast<quint64>(nExpectedOffset) ||
        nExpectedSize <= 0 ||
        arrPayload.size() != nExpectedSize ||
        stHeader.m_nPayloadSize !=
            static_cast<quint32>(nExpectedSize) ||
        stHeader.m_nFileSize !=
            static_cast<quint64>(stMetadata.m_nFileSize) ||
        stHeader.m_nTotalPacketCount !=
            stMetadata.m_nTotalPacketCount)
    {
        strErrorMessage =
            QStringLiteral("分片偏移、长度或会话信息不一致。");
        return false;
    }

    const int nPacketIndex = static_cast<int>(nSequence);

    //第二段：在锁内判断是否已经收到，重复分片直接允许确认。
    {
        QMutexLocker oLocker(&m_oMutex);

        if (m_stState.m_oReceivedPacketMap.testBit(nPacketIndex))
        {
            bAccepted = true;
            return true;
        }
    } //离开作用域，释放会话锁。

    //第三段：在锁外写当前通道自己的临时文件。
    //全局文件偏移需要转换成通道文件内部的局部偏移。
    const qint64 nLocalOffset =
        nExpectedOffset - stLaneRange.m_nStartOffset;

    QFile oLaneFile(GetLaneFilePath(nLaneIndex));

    //ExistingOnly：临时文件必须已经存在，不能在这里偷偷新建。
    const bool bWritten =
        oLaneFile.open(
            QIODevice::ReadWrite | QIODevice::ExistingOnly) &&
        oLaneFile.seek(nLocalOffset) &&
        oLaneFile.write(arrPayload) == arrPayload.size() &&
        oLaneFile.flush();//既要写入完整的分片，也要成功刷新缓冲区，才认为这次文件写入成功
//    oLaneFile.flush();
//    就是要求 Qt 把剩余的缓冲数据提交出去：
//    - 返回 true：刷新成功。
//    - 返回 false：刷新失败。

    const QString strFileError = oLaneFile.errorString();
    oLaneFile.close();

    if (!bWritten)
    {
        strErrorMessage =
            QStringLiteral("分片写入失败：%1").arg(strFileError);
        return false;
    }

    //第四段：文件写入成功后，才在锁内提交接收记录。
    QMutexLocker oLocker(&m_oMutex);

    m_stState.m_oReceivedPacketMap.setBit(nPacketIndex, true);
    ++m_stState.m_nReceivedPacketCount;
    ++m_stState.m_nUnsavedPacketCount;

    bAccepted = true;
    bNewPacket = true;

    //累计 32 个新分片后保存状态。
    //状态保存失败作为警告返回，不撤销已经成功写入的分片。
    if (m_stState.m_nUnsavedPacketCount >= 32)
    {
        PersistSessionStateLocked(strWarningMessage);
    }

    return true;
}

quint32 ReliableReceiveSession::GetReceivedPacketCount()
{
    QMutexLocker oLocker(&m_oMutex);

    return m_stState.m_nReceivedPacketCount;
}

bool ReliableReceiveSession::VerifyAndFinalizeSession(
    QList<SReliablePacketRange> &arrMissingRanges,
    QString &strFinalPath,
    QString &strErrorMessage,
    QString *pTimingMessage)
{
    //【合并计时】元信息创建后不变；计时对象先于锁创建，最后析构汇总。
    //【合并流水线】接收端启动脚本设置模式；默认流水线，serial 保留原串行方式。
    const bool bPipeline = qEnvironmentVariable("MYSERVER_MERGE_MODE")
        .compare(QStringLiteral("serial"), Qt::CaseInsensitive) != 0;
    SFinalizeTiming stTiming(m_stState.m_stMetadata, pTimingMessage, bPipeline);
    QMutexLocker oLocker(&m_oMutex);//这里已经是最终合并阶段了，所以锁的范围可以粗一点
    stTiming.m_nWaitLockNs = stTiming.m_oClock.nsecsElapsed();

    arrMissingRanges.clear();
    strFinalPath.clear();
    strErrorMessage.clear();

    //第一段：已经完成时直接返回，避免重复合并。
    //接收端已经生成正式文件，但发送端没有收到“传输成功”的回复，于是再次发送完成请求。
    if (m_stState.m_bCompleted)
    {
        stTiming.m_strResult = QStringLiteral("已完成，跳过合并");
        strFinalPath = m_stState.m_strFinalFilePath;
        return true;
    }

    arrMissingRanges = BuildMissingRangesLocked();

    if (!arrMissingRanges.isEmpty())
    {
        stTiming.m_strResult = QStringLiteral("仍有缺片，未合并");
        strErrorMessage =
            QStringLiteral("接收端仍有缺失或正在写入的分片。");

        QString strPersistError;
        if (!PersistSessionStateLocked(strPersistError))
        {
            strErrorMessage +=
                QStringLiteral(" 状态保存失败：%1")
                    .arg(strPersistError);
        }

        return false;
    }

    const SReliableFileMetadata &stMetadata =
        m_stState.m_stMetadata;

    //第二段：创建合并输出，按通道编号顺序读取。
    QSaveFile oMergedFile(m_stState.m_strPartFilePath);//这里的 .part 是合并后的临时文件，完整性校验成功后，才会改名成为正式文件

    if (!oMergedFile.open(QIODevice::WriteOnly))
    {
        strErrorMessage =
            QStringLiteral("合并临时文件打开失败：%1")
                .arg(oMergedFile.errorString());
        return false;
    }

    //【CNG SHA-256】仍在顺序合并时增量校验，不额外通读文件。
    ReliableSha256 oShaHash;
    qint64 nStageStartNs = stTiming.m_oClock.nsecsElapsed();
    QScopedPointer<ReliableMergeHasher> pMergeHasher;
    if (bPipeline)
    {
        pMergeHasher.reset(new ReliableMergeHasher(m_pStopFlag));
    }
    else
    {
        const bool bHashInitialized = oShaHash.Initialize(strErrorMessage);
        stTiming.m_nHashNs += stTiming.m_oClock.nsecsElapsed() - nStageStartNs;
        if (!bHashInitialized)
        {
            return false;
        }
    }
    SMergeHashJoin oHashJoin{pMergeHasher.data(), stTiming.m_nHashNs};

    //串行复用一个 4 MiB 缓冲；流水线由摘要队列持有三个 4 MiB 缓冲。
    QByteArray arrBuffer;
    if (!bPipeline)
    {
        arrBuffer = QByteArray(4 * 1024 * 1024, '\0');
    }
    qint64 nMergedSize = 0;

    for (int nLaneIndex = 0;
         nLaneIndex < stMetadata.m_nLaneCount;
         ++nLaneIndex)
    {
        //【通道范围提取】合并时复用与创建通道文件相同的字节数计算。
        const SReliableLaneRange stLaneRange =
            GetReliableLaneRange(stMetadata, nLaneIndex);

        QFile oLaneFile(GetLaneFilePath(nLaneIndex));

        if (!oLaneFile.open(QIODevice::ReadOnly) ||
            oLaneFile.size() != stLaneRange.m_nByteSize)
        {
            strErrorMessage =
                QStringLiteral("通道 %1 临时文件无法读取或大小不匹配。")
                    .arg(nLaneIndex);
            return false;
        }

        qint64 nLaneReadSize = 0;

        while (nLaneReadSize < stLaneRange.m_nByteSize)
        {
            //后续停止监听时，会通过这个共享标记通知合并过程退出。
            if (m_pStopFlag->loadAcquire())
            {//表示有其他线程要求停止
                strErrorMessage =
                    QStringLiteral("接收端正在停止，取消合并，保留通道文件。");
                return false;
            }

            int nBufferIndex = -1;
            if (bPipeline)
            {
                nStageStartNs = stTiming.m_oClock.nsecsElapsed();
                const bool bAcquired = pMergeHasher->AcquireBuffer(nBufferIndex, strErrorMessage);
                stTiming.m_nBufferWaitNs += stTiming.m_oClock.nsecsElapsed() - nStageStartNs;
                if (!bAcquired)
                {
                    return false;
                }
            }
            QByteArray &arrCurrentBuffer = bPipeline
                ? pMergeHasher->GetBuffer(nBufferIndex) : arrBuffer;
            nStageStartNs = stTiming.m_oClock.nsecsElapsed();
            const qint64 nReadSize = oLaneFile.read(
                arrCurrentBuffer.data(),
                qMin<qint64>(
                    arrCurrentBuffer.size(),
                    stLaneRange.m_nByteSize - nLaneReadSize));
            stTiming.m_nReadNs += stTiming.m_oClock.nsecsElapsed() - nStageStartNs;

            if (nReadSize <= 0)
            {
                strErrorMessage =
                    QStringLiteral("通道 %1 临时文件读取失败或提前结束。")
                        .arg(nLaneIndex);
                return false;
            }
            //第一个参数 arrCurrentBuffer.constData() 指向缓冲区中原始字节数据的指针
            //第二个参数	nReadSize	本次最多写入多少个字节
            nStageStartNs = stTiming.m_oClock.nsecsElapsed();
            const qint64 nWritten = oMergedFile.write(arrCurrentBuffer.constData(), nReadSize);
            stTiming.m_nWriteNs += stTiming.m_oClock.nsecsElapsed() - nStageStartNs;
            if (nWritten != nReadSize)
            {
                strErrorMessage =
                    QStringLiteral("合并文件写入失败：%1")
                        .arg(oMergedFile.errorString());
                return false;
            }

            //同一批数据既用于合并，也用于计算摘要，避免再次通读文件。
            if (bPipeline)
            {
                //提交后不再访问该缓冲，摘要线程处理完才归还。
                if (!pMergeHasher->SubmitBuffer(nBufferIndex, static_cast<int>(nReadSize), strErrorMessage))
                {
                    return false;
                }
            }
            else
            {
                nStageStartNs = stTiming.m_oClock.nsecsElapsed();
                const bool bHashUpdated = oShaHash.AddData(
                    arrCurrentBuffer.constData(),
                    static_cast<int>(nReadSize),
                    strErrorMessage);
                stTiming.m_nHashNs += stTiming.m_oClock.nsecsElapsed() - nStageStartNs;
                if (!bHashUpdated)
                {
                    return false;
                }
            }

            nLaneReadSize += nReadSize;//当前通道已经读了多少字节；换通道时重新从 0 开始
            nMergedSize += nReadSize;//整个文件累计合并了多少字节；跨通道持续累加
            stTiming.m_nMergedBytes = nMergedSize;
        }
    }

    //第三段：校验合并后的总大小和 SHA-256。
    QByteArray arrDigest;
    nStageStartNs = stTiming.m_oClock.nsecsElapsed();
    const bool bHashFinished = bPipeline
        ? pMergeHasher->Finish(arrDigest, strErrorMessage)
        : oShaHash.Finish(arrDigest, strErrorMessage);
    if (bPipeline)
    {
        stTiming.m_nDrainNs += stTiming.m_oClock.nsecsElapsed() - nStageStartNs;
    }
    else
    {
        stTiming.m_nHashNs += stTiming.m_oClock.nsecsElapsed() - nStageStartNs;
    }
    if (!bHashFinished)
    {
        return false;
    }
    const QString strActualSha256 = QString::fromLatin1(arrDigest.toHex());

    if (bPipeline && pMergeHasher->GetHashedByteCount() != nMergedSize)
    {
        strErrorMessage = QStringLiteral("摘要处理字节数与合并大小不一致，保留通道文件和状态。");
        return false;
    }

    if (nMergedSize != stMetadata.m_nFileSize ||
        strActualSha256 != stMetadata.m_strSha256)
    {
        strErrorMessage =
            QStringLiteral("最终文件大小或 SHA-256 不匹配，保留通道文件和状态。");
        return false;
    }

    //校验通过后，才提交生成 assembled.part。
    //commit()把临时写入的数据提交到最终目标文件
    if (m_pStopFlag->loadAcquire())
    {
        strErrorMessage = QStringLiteral("接收端正在停止，取消合并，保留通道文件。");
        return false;
    }
    nStageStartNs = stTiming.m_oClock.nsecsElapsed();
    const bool bCommitted = oMergedFile.commit();
    stTiming.m_nCommitNs += stTiming.m_oClock.nsecsElapsed() - nStageStartNs;
    if (!bCommitted)
    {
        strErrorMessage =
            QStringLiteral("合并临时文件提交失败：%1")
                .arg(oMergedFile.errorString());
        return false;
    }

    //第四段：再次选择可用路径，避免合并期间出现同名文件。
    nStageStartNs = stTiming.m_oClock.nsecsElapsed();
    const QString strAvailablePath =
        BuildAvailableFilePath(
            QDir(m_strOutputDirectory)
                .filePath(stMetadata.m_strFileName));

    const bool bRenamed = !strAvailablePath.isEmpty()
        && QFile::rename(
            m_stState.m_strPartFilePath,
            strAvailablePath);
    stTiming.m_nRenameNs += stTiming.m_oClock.nsecsElapsed() - nStageStartNs;
    if (!bRenamed)
    {
        strErrorMessage =
            QStringLiteral("正式文件生成失败，合并文件和通道文件已保留。");
        return false;
    }

    m_stState.m_strFinalFilePath = strAvailablePath;
    m_stState.m_bCompleted = true;
    strFinalPath = strAvailablePath;

    //正式文件成功生成后，再清理本次会话的已知临时文件。
    nStageStartNs = stTiming.m_oClock.nsecsElapsed();
    for (int nLaneIndex = 0;
         nLaneIndex < stMetadata.m_nLaneCount;
         ++nLaneIndex)
    {
        QFile::remove(GetLaneFilePath(nLaneIndex));
    }

    QFile::remove(m_stState.m_strStateFilePath);

    //只删除空目录，避免递归删除会话目录里的未知文件。
    QDir().rmdir(m_stState.m_strSessionDirectory);
    stTiming.m_nCleanupNs += stTiming.m_oClock.nsecsElapsed() - nStageStartNs;
    stTiming.m_strResult = QStringLiteral("完成");

    return true;
}

QList<SReliablePacketRange> ReliableReceiveSession::BuildMissingRangesLocked() const
{
    QList<SReliablePacketRange> arrMissingRangeList;

    //-1 表示当前还没有进入一个连续缺失区间。
    int nStartSequence = -1;

    const int nPacketCount =
        m_stState.m_oReceivedPacketMap.size();

    for (int nIndex = 0; nIndex < nPacketCount; ++nIndex)
    {
        const bool bReceived =
            m_stState.m_oReceivedPacketMap.testBit(nIndex);

        if (!bReceived && nStartSequence < 0)
        {
            //遇到一个缺失分片，记录缺失区间的起点。
            nStartSequence = nIndex;
        }
        else if (bReceived && nStartSequence >= 0)
        {
            //遇到已收到的分片，说明前面的缺失区间结束了。
            //当前分片已收到，所以终点应当是 nIndex - 1。
            arrMissingRangeList.append(
                SReliablePacketRange(
                    static_cast<quint64>(nStartSequence),
                    static_cast<quint64>(nIndex - 1)));

            //结束本段，等待寻找下一个缺失区间。
            nStartSequence = -1;
        }
    }

    //如果最后一个分片仍然缺失，循环中就没有机会结束该区间。
    //这里补上一直延伸到位图末尾的缺失区间。
    if (nStartSequence >= 0)
    {
        arrMissingRangeList.append(
            SReliablePacketRange(
                static_cast<quint64>(nStartSequence),
                static_cast<quint64>(nPacketCount - 1)));
    }

    return arrMissingRangeList;
}
