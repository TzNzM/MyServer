#include "ReliableTransferIo.h"
#include "ReliablePacketCodec.h"

#include <QElapsedTimer>
#include <QTcpSocket>

bool ReliableTransferIo::WritePacket(QTcpSocket &oSocket, const QByteArray &arrPacket, const ReliableCancelCheck &fnCancelled, QString &strErrorMessage)
{
    //第一段：记录本次写包开始的时间。
    QElapsedTimer oTimer;
    oTimer.start();

    //已经交给 socket 的字节数。
    qint64 nOffset = 0;

    //协议包尚未全部交给 socket，
    //或者 Qt 内部仍有待发送数据时，继续处理。
    while (nOffset < arrPacket.size() || oSocket.bytesToWrite())
    {
        //第二段：检查取消请求及连接状态
        if(fnCancelled())
        {
            strErrorMessage = QStringLiteral("发送任务已取消。");
            return false;
        }

        if(oSocket.state() != QAbstractSocket::ConnectedState)
        {
            strErrorMessage = QStringLiteral("连接已断开：%1").arg(oSocket.errorString());
            return false;
        }

        //第三段：将尚未写入的部分交给 socket。
        if(nOffset < arrPacket.size())
        {
            //write(从哪个内存地址开始读取, 要写入多少字节);
            const qint64 nWritten = oSocket.write(arrPacket.constData() + nOffset,arrPacket.size() - nOffset);
            if (nWritten < 0)
            {
                strErrorMessage = oSocket.errorString();
                return false;
            }

            nOffset += nWritten;
        }

        //第四段：限制整个写包过程的等待时间。
        const qint64 nRemainingMs = g_nReliableControlWaitTimeoutMs - oTimer.elapsed();
        if(nRemainingMs <= 0)
        {
            strErrorMessage = QStringLiteral("等待网络写出超时。");
            return false;
        }

        //每次短暂等待，之后重新检查取消、断线和总超时
        if(oSocket.bytesToWrite() > 0)
        {
            oSocket.waitForBytesWritten(static_cast<int>(qMin<qint64>(g_nReliableAckPollIntervalMs,nRemainingMs)));
        }

    }
    return true;
}

bool ReliableTransferIo::ReadPacket(QTcpSocket &oSocket, QByteArray &arrBuffer, SReliablePacketHeader &stHeader, QByteArray &arrPayload, int nTimeoutMs, const ReliableCancelCheck &fnCancelled, QString &strErrorMessage)
{
    QElapsedTimer oTimer;
    oTimer.start();

    while (!fnCancelled())
    {
        //第一段：优先从已有缓存中取一个完整包。
        bool bReady = false;
        if(!ReliablePacketCodec::TryTakePacket(arrBuffer,stHeader,arrPayload,bReady,strErrorMessage))
        {
            return false;
        }

        if(bReady)
        {
            return true;
        }

        //第二段：尚未取得完整包，检查剩余等待时间。
        const qint64 nRemainingMs = nTimeoutMs - oTimer.elapsed();
        if(nRemainingMs <= 0)
        {
            strErrorMessage = QStringLiteral("等待协议回包超时。");
            return false;
        }

        //第三段：有可读数据时，追加到缓存，再尝试拆包。
        if(oSocket.bytesAvailable() > 0)
        {
            arrBuffer.append(oSocket.readAll());
            continue;
        }

        //没有完整包，也没有更多可读数据时，检查连接。
        if(oSocket.state() != QAbstractSocket::ConnectedState)
        {
            strErrorMessage = QStringLiteral("等待回包时连接断开：%1")
                                .arg(oSocket.errorString());
                        return false;
        }

        //第四段：暂时没有数据，短暂等待后重新检查。
        oSocket.waitForReadyRead(static_cast<int>(qMin<qint64>(g_nReliableAckPollIntervalMs,nRemainingMs)));
    }

    strErrorMessage = QStringLiteral("发送任务已取消。");
    return false;
}

bool ReliableTransferIo::ConnectAndHandshake(QTcpSocket &oSocket, QByteArray &arrBuffer, const SReliableFileMetadata &stMetadata, const QString &strTargetIp, int nPort, int nWindowSize, QList<SReliablePacketRange> &arrMissingRanges, quint32 &nReceivedCount, const ReliableCancelCheck &fnCancelled, QString &strErrorMessage, int nLaneIndex)
{
    //第一段：清理旧连接及其残留数据。
    oSocket.abort();
    arrBuffer.clear();
    arrMissingRanges.clear();
    nReceivedCount = 0;
    strErrorMessage.clear();

    if(fnCancelled())
    {
        strErrorMessage = QStringLiteral("发送任务已取消。");
        return false;
    }

    oSocket.connectToHost(strTargetIp,static_cast<quint16>(nPort));
    if(!oSocket.waitForConnected(g_nReliableControlWaitTimeoutMs))
    {
        strErrorMessage = QStringLiteral("连接失败：%1").arg(oSocket.errorString());
        return false;
    }

    //第三段：发送文件元信息，声明当前连接的通道身份。
    const QByteArray arrMetadataPacket = ReliablePacketCodec::BuildMetadataPacket(stMetadata,nWindowSize,nLaneIndex);
    if(!WritePacket(oSocket,arrMetadataPacket,fnCancelled,strErrorMessage))
    {
        return false;
    }

    //第四段：等待接收端返回一个完整协议包。
    SReliablePacketHeader stHeader;
    QByteArray arrPayload;
    if(!ReadPacket(oSocket,arrBuffer,stHeader,arrPayload,g_nReliableControlWaitTimeoutMs,fnCancelled,strErrorMessage))
    {
        return false;
    }

    //第五段：检查回复是否属于当前任务。
    if (stHeader.m_oTransferId != stMetadata.m_oTransferId)
    {
        strErrorMessage = QStringLiteral("握手回包的任务 ID 不匹配。");
        return false;
    }

    if(stHeader.m_nMessageType == static_cast<quint16>(EReliableMessageType::ErrorPacket))
    {
        strErrorMessage = QString::fromUtf8(arrPayload);
        return false;
    }

    if (stHeader.m_nMessageType !=
            static_cast<quint16>(EReliableMessageType::ResumeStatePacket) ||
        stHeader.m_nFileSize !=
            static_cast<quint64>(stMetadata.m_nFileSize) ||
        stHeader.m_nTotalPacketCount != stMetadata.m_nTotalPacketCount)
    {
        strErrorMessage = QStringLiteral("握手回包类型或文件信息不匹配。");
        return false;
    }

    if(!ReliablePacketCodec::ParseResumeStatePayload(arrPayload,arrMissingRanges,nReceivedCount,strErrorMessage))
    {
        return false;
    }

    //【临时调整】按当前学习阶段的要求，暂时停用握手数量一致性校验。
    //本校验只在握手时执行，不在逐个接收数据分片的路径上执行。
    //恢复时需同时恢复计数变量、范围累计和下方的总数比较。
    //quint64 nMissingCount = 0;
    for(const SReliablePacketRange &stRange : arrMissingRanges)
    {
        if (stRange.m_nEndSequence >= stMetadata.m_nTotalPacketCount)
        {
            strErrorMessage = QStringLiteral("缺失范围超出文件分片数量。");
            return false;
        }

        //缺失范围是闭区间，两端都包含。
        //nMissingCount += stRange.m_nEndSequence - stRange.m_nStartSequence + 1;
    }

    //if(nMissingCount + nReceivedCount != stMetadata.m_nTotalPacketCount)
    //{
    //    strErrorMessage =
    //                QStringLiteral("缺失分片数与已接收数量之和不等于总分片数。");
    //            return false;
    //}

    return true;
}
