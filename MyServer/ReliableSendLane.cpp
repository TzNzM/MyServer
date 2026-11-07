#include "ReliableSendLane.h"
#include "ReliableTransferUtils.h"
#include "ReliablePacketCodec.h"
#include "ReliableTransferIo.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTcpSocket>
#include <QMutexLocker>
#include <QHash>

ReliableSendLane::ReliableSendLane(const QString &strFilePath, const QString &strTargetIp, int nPort, const SReliableFileMetadata &stMetadata, int nLaneIndex, quint64 nFirstSequence, quint64 nEndSequence, int nWindowSize, const QSharedPointer<SReliableSendTaskState> &pTaskState)
    : QThread(nullptr),
      m_strFilePath(strFilePath),
      m_strTargetIp(strTargetIp),
      m_nPort(nPort),
      m_stMetadata(stMetadata),
      m_nLaneIndex(nLaneIndex),
      m_nFirstSequence(nFirstSequence),
      m_nEndSequence(nEndSequence),
      m_nWindowSize(nWindowSize),
      m_pTaskState(pTaskState),
      m_bSuccess(false)
{
    setObjectName(QStringLiteral("sender-lane-%1").arg(nLaneIndex));
}

bool ReliableSendLane::WasSuccessful() const
{
    return m_bSuccess;
}

void ReliableSendLane::run()
{
    QString strErrorMessage;
    //第一段：首次尝试加上最多指定次数的重连。
    //nAttempt 为 0 表示首次尝试，1～3 表示后续重试。
    for(int nAttempt = 0; nAttempt <= g_nReliableDefaultReconnectCount && !IsCancelled(); ++nAttempt)
    {
        if(nAttempt > 0)
        {
            //记录上次失败原因及当前重连次数。
            QMutexLocker oLocker(&m_pTaskState->m_oMutex);
            m_pTaskState->m_arrLogMessages.append(
                            QStringLiteral("通道 %1，第 %2 次重连，原因：%3")
                                .arg(m_nLaneIndex)
                                .arg(nAttempt)
                                .arg(strErrorMessage));
        }
        //第二段：重试前逐步增加等待时间，避免立即连续重连。
        //当前间隔依次约为 0、200、400、600 毫秒。
        //拆成多次短等待，期间能够检查取消请求。
        for (int nDelay = 0;
             nDelay < nAttempt * 5 && !IsCancelled();
             ++nDelay)
        {
            QThread::msleep(g_nReliableAckPollIntervalMs);
        }

        if (IsCancelled())
        {
            return;
        }

        //第三段：执行本通道的一轮发送。
        if (SendPartition(strErrorMessage))
        {
            m_bSuccess = true;
            return;
        }
    }
    //第四段：主动取消时直接结束，不把取消覆盖成新的错误。
    if (IsCancelled())
    {
        return;
    }

    //重试仍未成功，记录失败并通知其他通道合作取消。
    QMutexLocker oLocker(&m_pTaskState->m_oMutex);

    if (m_pTaskState->m_strFirstError.isEmpty())
    {
        m_pTaskState->m_strFirstError =
            QStringLiteral("通道 %1 发送失败：%2")
                .arg(m_nLaneIndex)
                .arg(strErrorMessage);
    }

    m_pTaskState->m_nCancelled.storeRelease(1);

}

bool ReliableSendLane::SendPartition(QString &strErrorMessage)
{
    //第一段：创建本次连接使用的文件、socket 和接收缓存。
    //后续由 run 调用，因此这些对象在当前数据线程中创建。
    QFile oFile(m_strFilePath);
    QTcpSocket oSocket;
    QByteArray arrBuffer;

    QList<SReliablePacketRange> arrMissingRanges;
    quint32 nReceivedCount = 0;
    const ReliableCancelCheck fnCancelled = [this]()
    {
        return  IsCancelled();
    };

    if(!oFile.open(QIODevice::ReadOnly) || oFile.size() != m_stMetadata.m_nFileSize)
    {
        strErrorMessage = QStringLiteral("源文件无法读取或大小已变化。");
        return false;
    }

    //第二段：声明本连接的数据通道身份，取得接收端续传状态。
    if(!ReliableTransferIo::ConnectAndHandshake(oSocket,arrBuffer,m_stMetadata,m_strTargetIp,m_nPort,m_nWindowSize,arrMissingRanges,nReceivedCount,fnCancelled,strErrorMessage,m_nLaneIndex))
    {
        return false;
    }

    //第三段：建立仅覆盖本通道区间的确认位图。
    //先全部标记为已确认，再将接收端报告的缺失分片改成 false。
    QBitArray oLocalAcked(static_cast<int>(m_nEndSequence - m_nFirstSequence),true);
    for (const SReliablePacketRange &stRange : arrMissingRanges)
    {
        //取“全文件缺失范围”与“本通道负责区间”的交集。
        const quint64 nStart = qMax(m_nFirstSequence,stRange.m_nStartSequence);
        //缺失范围是闭区间，+1 后转换为左闭右开区间。
        const quint64 nEnd = qMin(m_nEndSequence,stRange.m_nEndSequence + 1);
        for(quint64 nSequence = nStart; nSequence < nEnd; ++nSequence)
        {
            oLocalAcked.setBit(static_cast<int>(nSequence - m_nFirstSequence),false);
        }
    }

    //第四段：将本通道的恢复结果同步到全任务共享状态。
    {
        QMutexLocker oLocker(&m_pTaskState->m_oMutex);

        //只更新自己负责的区间，不覆盖其他通道的确认记录
        for(quint64 nSequence = m_nFirstSequence;nSequence < m_nEndSequence;++nSequence)
        {
            const bool bAcked = oLocalAcked.testBit(static_cast<int>(nSequence - m_nFirstSequence));
            //共享位图使用全局分片序号作为下标。
            const int nIndex = static_cast<int>(nSequence);
            //状态发生变化时，才调整共享位图和总计数。
            if(bAcked != m_pTaskState->m_oAckedPacketMap.testBit(nIndex))
            {
                m_pTaskState->m_oAckedPacketMap.setBit(nIndex,bAcked);
                m_pTaskState->m_nConfirmedCount += bAcked ? 1:-1;
            }

        }
        m_pTaskState->m_arrLogMessages.append(
                QStringLiteral("通道=%1，线程=%2，负责区间=[%3, %4)，窗口=%5")
                    .arg(m_nLaneIndex)
                    .arg(objectName())
                    .arg(m_nFirstSequence)
                    .arg(m_nEndSequence)
                    .arg(m_nWindowSize));
    }
    //第五段：启动本轮发送的计时器。
    //同一轮发送中的所有分片共用这个时钟。
    QElapsedTimer oClock;
    oClock.start();

    //已经写出、尚未收到业务 ACK 的分片记录。
    QHash<quint64,SReliablePendingPacket> mapInFightPacket;
    //下一个准备检查并发送的全局分片序号。
    quint64 nNextSequence = m_nFirstSequence;
    while (!IsCancelled())
    {
        //窗口还有空位，并且还有分片需要检查时，继续填充窗口。
        while (mapInFightPacket.size() < m_nWindowSize &&
               nNextSequence < m_nEndSequence && !IsCancelled())
        {
            const quint64 nSequence = nNextSequence++;
            //续传时已经确认的分片，不再发送。
            if(oLocalAcked.testBit(static_cast<int>(nSequence - m_nFirstSequence)))
            {
                continue;
            }
            SReliablePendingPacket stPending;

            if(!SendDataPacket(oSocket,oFile,nSequence,stPending,oClock,strErrorMessage))
            {
                return false;
            }

            //成功写出后，登记为在途分片，等待业务 ACK。
            mapInFightPacket.insert(nSequence,stPending);
        }
        //所有序号都已检查，且没有等待 ACK 的分片，通道才算完成。
        //续传的情况下，可能该分片确实没有任何要传的东西，所以可以提前判断退出
        if(nNextSequence == m_nEndSequence && mapInFightPacket.isEmpty())
        {
            return true;
        }

        //第六段：读取当前可用数据，处理缓存中的完整回包。当没有缓存包时，就暂时退出该循环
        arrBuffer.append(oSocket.readAll());
        while (true)
        {
            SReliablePacketHeader stHeader;
            QByteArray arrPayload;
            bool bReady = false;
            if(!ReliablePacketCodec::TryTakePacket(arrBuffer,stHeader,arrPayload,bReady,strErrorMessage))
            {
                return false;
            }

            if(!bReady)
            {
                break;//半包继续保留，后面检查超时，再等待新数据。
            }

            if (stHeader.m_oTransferId != m_stMetadata.m_oTransferId)
            {
                strErrorMessage = QStringLiteral("回包的任务 ID 不匹配。");
                return false;
            }

            if (stHeader.m_nMessageType ==
                static_cast<quint16>(EReliableMessageType::ErrorPacket))
            {
                strErrorMessage = QString::fromUtf8(arrPayload);
                return false;
            }

            const quint64 nSequence = stHeader.m_nPacketSequence;
            //数据连接应收到属于本通道、本文件任务的 ACK。
            if (stHeader.m_nMessageType !=
                    static_cast<quint16>(EReliableMessageType::AckPacket) ||
                nSequence < m_nFirstSequence ||
                nSequence >= m_nEndSequence ||
                stHeader.m_nFileSize !=
                    static_cast<quint64>(m_stMetadata.m_nFileSize) ||
                stHeader.m_nTotalPacketCount != m_stMetadata.m_nTotalPacketCount)
            {
                strErrorMessage = QStringLiteral("ACK 类型、序号或文件信息非法。");
                return false;
            }

            const int nLocalIndex = static_cast<int>(nSequence - m_nFirstSequence);
            if(mapInFightPacket.remove(nSequence) > 0)
            {
                //找到对应在途记录：确认完成，移除后窗口空出一个位置。
                oLocalAcked.setBit(nLocalIndex,true);

                QMutexLocker oLocker(&m_pTaskState->m_oMutex);
                const int nGlobalIndex = static_cast<int>(nSequence);

                //共享进度只在首次确认时增加。
                if(!m_pTaskState->m_oAckedPacketMap.testBit(nGlobalIndex))
                {
                    m_pTaskState->m_oAckedPacketMap.setBit(nGlobalIndex, true);
                    ++m_pTaskState->m_nConfirmedCount;
                }
            }
            else if(!oLocalAcked.testBit(nLocalIndex))
            {
                //既不在途，也未被确认，说明当前连接尚未发送这个分片。
                strErrorMessage = QStringLiteral("收到尚未发送分片的 ACK。");
                return false;
            }
            //不在途，但本地已经确认过：重复 ACK，直接忽略。
        }

        //处理 ACK 后，再判断一次是否已经全部完成。
        if (nNextSequence == m_nEndSequence && mapInFightPacket.isEmpty())
        {
            return true;
        }

        if (oSocket.state() != QAbstractSocket::ConnectedState)
        {
            strErrorMessage = QStringLiteral("数据连接已断开。");
            return false;
        }

        //第七段：检查仍在等待 ACK 的分片，必要时补传。
        for(auto oIt = mapInFightPacket.begin(); oIt != mapInFightPacket.end();++oIt)
        {
            const qint64 nWaitedMs = oClock.elapsed() - oIt->m_nLastSendTimestampMs;
            if(nWaitedMs < g_nReliableDefaultPacketTimeoutMs)
            {
                continue;//尚未达到分片超时阈值。
            }

            if(oIt->m_nRetryCount >= g_nReliableDefaultMaxRetryCount)
            {
                strErrorMessage =
                    QStringLiteral("分片 %1 的补传次数超限。").arg(oIt.key());
                return false;
            }
            //使用原来的在途记录，保留之前累计的补传次数。
            if(!SendDataPacket(oSocket,oFile,oIt.key(),oIt.value(),oClock,strErrorMessage))
            {
                return false;
            }

            ++oIt->m_nRetryCount;
        }

        //第八段：暂时没有新数据时短暂等待，避免外层循环空转。
        if (oSocket.bytesAvailable() == 0)
        {
            oSocket.waitForReadyRead(g_nReliableAckPollIntervalMs);
        }
    }

    strErrorMessage = QStringLiteral("发送任务已取消。");
    return false;

}

bool ReliableSendLane::IsCancelled() const
{
    return m_pTaskState->m_nCancelled.loadAcquire() != 0 || isInterruptionRequested();
}

bool ReliableSendLane::SendDataPacket(QTcpSocket &oSocket, QFile &oFile, quint64 nSequence, SReliablePendingPacket &stPending, QElapsedTimer &oClock, QString &strErrorMessage)
{
    //第一段：根据全局分片序号计算文件偏移和实际长度。
    //【公共计算提取】发送读取与接收校验共用偏移和末片长度计算。
    const SReliablePacketLayout stLayout =
        GetReliablePacketLayout(m_stMetadata, nSequence);

    const qint64 nOffset = stLayout.m_nPayloadOffset;
    const int nPayloadSize = stLayout.m_nPayloadSize;

    //第二段：定位并读取当前分片。
    if(nPayloadSize <= 0 || !oFile.seek(nOffset))
    {
        strErrorMessage = QStringLiteral("源文件分片定位失败。");
        return false;
    }

    const QByteArray arrPayload = oFile.read(nPayloadSize);
    if(arrPayload.size() != nPayloadSize)
    {
        strErrorMessage = QStringLiteral("源文件分片读取失败或文件提前结束：%1").arg(oFile.errorString());
        return false;
    }

    //第三段：封装为完整协议包，并通过当前连接写出。
    const QByteArray arrPacket = ReliablePacketCodec::BuildDataPacket(m_stMetadata,nSequence,nOffset,arrPayload);

    const ReliableCancelCheck fnCancelled = [this]()
    {
        return IsCancelled();
    };

    if(!ReliableTransferIo::WritePacket(oSocket,arrPacket,fnCancelled,strErrorMessage))
    {
        return false;
    }

    //第四段：写出成功后，更新在途记录。
    //补传次数由调用方管理，这里不将它清零。
    //写出之后要记录，等到收到了再来说ACK的事情
    stPending.m_nPacketSequence = nSequence;
    stPending.m_nPayloadOffset = nOffset;
    stPending.m_nPayloadSize = nPayloadSize;
    stPending.m_nLastSendTimestampMs = oClock.elapsed();

    return true;

}
