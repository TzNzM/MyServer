#include "ReliableSenderWorker.h"
#include "ReliableSha256.h"
#include "ReliableTransferUtils.h"
#include "ReliablePacketCodec.h"
#include "ReliableTransferIo.h"
#include "ReliableSendLane.h"

#include <QList>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QTcpSocket>
#include <QString>
#include <QMutexLocker>

ReliableSenderWorker::ReliableSenderWorker(QObject *pParent)
    : QObject(pParent)
{

}

void ReliableSenderWorker::ProcessSend(const QString &strFilePath, const QString &strTargetIp, int nPort, int nThreadCount, int nWindowSize, int nSegmentSize)
{
    //第一段：检查目标地址、端口和每条连接的发送窗口。
    QHostAddress oTargetAddress;

    if (!oTargetAddress.setAddress(strTargetIp.trimmed()) ||
            nPort < 1 || nPort > 65535 ||
            nWindowSize < 1 ||
            nWindowSize > g_nReliableMaxWindowSize)
    {
        emit SignalSendFinished(
            false,
            QStringLiteral("目标 IP、端口或发送窗口非法。"));
        return;
    }

    //第二段：准备文件元信息。
    //文件、分片大小和线程数的检查由 BuildMetadata 完成。
    QString strErrorMessage;
    SReliableFileMetadata stMetadata;

    if (!BuildMetadata(
            strFilePath,
            nSegmentSize,
            nThreadCount,
            stMetadata,
            strErrorMessage))
    {
        emit SignalSendFinished(false, strErrorMessage);
        return;
    }

    //第三段：创建整个文件任务的共享状态。
    const int nTotalPacketCount = static_cast<int>(stMetadata.m_nTotalPacketCount);
    QSharedPointer<SReliableSendTaskState> pTaskState(new SReliableSendTaskState(nTotalPacketCount));

    //协调线程收到中断请求时，将其转为整个任务的共享取消标记。
    //数据通道通过同一个共享状态检查是否需要退出。
    const ReliableCancelCheck fnCancelled = [pTaskState]()
    {
        if(QThread::currentThread()->isInterruptionRequested())
        {
            pTaskState->m_nCancelled.storeRelease(1);
        }

        return pTaskState->m_nCancelled.loadAcquire() != 0;
    };

    if (fnCancelled())
    {
        emit SignalSendFinished(
            false,
            QStringLiteral("发送任务已取消。"));
        return;
    }

    //第四段：报告任务配置及初始进度。
    emit SignalLogMessage(
        QStringLiteral(
            "准备发送：文件=%1，数据通道=%2，每通道窗口=%3。")
            .arg(stMetadata.m_strFileName)
            .arg(stMetadata.m_nLaneCount)
            .arg(nWindowSize));

    //设为 -1，确保第一次快照能够报告初始进度 0。
    int nLastCompletedCount = -1;

    EmitTaskSnapshot(pTaskState,nTotalPacketCount,nLastCompletedCount);

    //第五段：创建控制连接及其接收缓存。
    //这些对象在执行 ProcessSend 的后台协调线程中创建
    QTcpSocket oControlSocket;
    QByteArray arrControlBuffer;
    //【协调恢复1】一次元信息准备对应多轮恢复，UUID 和共享状态保持不变。
    //每轮只根据接收端的最新缺失范围发送，取消标记不在轮次之间清零。
    for (int nRound = 0;
        nRound <= g_nReliableDefaultReconnectCount && !fnCancelled();
        ++nRound)
    {
        if (nRound > 0)
        {
            emit SignalLogMessage(
                QStringLiteral("开始第 %1 次协调恢复，原因：%2")
                    .arg(nRound)
                    .arg(strErrorMessage));
        }

        QList<SReliablePacketRange> arrMissingRanges;
        quint32 nReceivedCount = 0;
        bool bConnected = false;

        //首次尝试一次，失败后最多再重试指定次数。
        for (int nAttempt = 0;
            nAttempt <= g_nReliableDefaultReconnectCount &&
            !fnCancelled();
            ++nAttempt)
        {
            //重试前分段等待，期间检查取消。
            for (int nDelay = 0;
                nDelay < nAttempt * 5 && !fnCancelled();
                ++nDelay)
            {
                QThread::msleep(g_nReliableAckPollIntervalMs);
            }

            //-1 表示控制连接，不属于任何数据通道。
            //ConnectAndHandshake 内部也会先检查取消。
            if(ReliableTransferIo::ConnectAndHandshake(oControlSocket,arrControlBuffer,stMetadata,oTargetAddress.toString(),nPort,nWindowSize,arrMissingRanges,nReceivedCount,fnCancelled,strErrorMessage,-1))
            {
                bConnected = true;
                break;
            }
        }
        if(!bConnected)
        {
            emit SignalSendFinished(
                        false,
                        fnCancelled()
                            ? QStringLiteral("发送任务已取消。")
                            : strErrorMessage);
                    return;
        }

        emit SignalLogMessage(
            QStringLiteral("控制连接握手完成：接收端已收到 %1 / %2 个分片。")
                .arg(nReceivedCount)
                .arg(nTotalPacketCount));
        //第六段：根据接收端的缺失范围，恢复全文件确认位图。
        {
            QMutexLocker oLocker(&pTaskState->m_oMutex);
            //先假设全部已收到，再将缺失范围改为未确认。
            pTaskState->m_oAckedPacketMap.fill(true);

            for (const SReliablePacketRange &stRange : arrMissingRanges)
            {
                //缺失范围是闭区间，起止序号都包含。
                for (quint64 nSequence = stRange.m_nStartSequence;
                    nSequence <= stRange.m_nEndSequence;
                    ++nSequence)
                {
                    pTaskState->m_oAckedPacketMap.setBit(
                        static_cast<int>(nSequence),
                        false);
                }
            }

            //从位图统计已确认数量，保证计数与位图一致。
            pTaskState->m_nConfirmedCount = pTaskState->m_oAckedPacketMap.count(true);
        }

        EmitTaskSnapshot(pTaskState,nTotalPacketCount,nLastCompletedCount);
        //第七段：划分连续的分片区间，创建并启动各数据通道。
        const int nLaneCount = stMetadata.m_nLaneCount;
        QList<ReliableSendLane *> arrLanes;
        for(int nLaneIndex = 0; nLaneIndex < nLaneCount;++nLaneIndex)
        {
            //【公共计算提取】与接收端创建文件、检查归属和合并共用同一规则。
            const SReliableLaneRange stLaneRange =
                GetReliableLaneRange(stMetadata, nLaneIndex);

            ReliableSendLane *pLane = new ReliableSendLane(
                        strFilePath,
                        oTargetAddress.toString(),
                        nPort,
                        stMetadata,
                        nLaneIndex,
                        stLaneRange.m_nFirstSequence,
                        stLaneRange.m_nEndSequence,
                        nWindowSize,
                        pTaskState);
            //保存线程对象，后续统一等待和回收。
            arrLanes.append(pLane);

            //启动后台线程，由 Qt 在新线程中执行 run()。
            pLane->start();
        }

        //第八段：等待各通道退出，同时检查取消并报告进度。
        while (true)
        {
            bool bAllFinished = true;
            for(ReliableSendLane *pLane : arrLanes)
            {
                //协调线程收到中断请求时，通知所有数据通道取消。
                fnCancelled();

                //只短暂等待，避免一直停留在某个通道上。
                if(!pLane->wait(10))
                {
                    bAllFinished = false;
                }
            }
            //趁检查线程的间隙，读取实际发生的进度变化。
            EmitTaskSnapshot(
                        pTaskState,
                        nTotalPacketCount,
                        nLastCompletedCount);

            if (bAllFinished)
            {
                break;
            }
        }

        //第九段：所有通道都已退出，读取结果并回收线程对象。
        bool bAllSuccessful = true;
        for (ReliableSendLane *pLane : arrLanes)
        {
            if (!pLane->WasSuccessful())
            {
                bAllSuccessful = false;
            }

            delete pLane;
        }

        if (!bAllSuccessful || fnCancelled())
        {
            //优先报告造成任务失败的原始原因。
            {
                QMutexLocker oLocker(&pTaskState->m_oMutex);
                strErrorMessage = pTaskState->m_strFirstError;
            }

            emit SignalSendFinished(
                false,
                strErrorMessage.isEmpty()
                    ? QStringLiteral("发送任务已取消。")
                    : strErrorMessage);
            return;
        }

        //第十段：所有数据通道成功后，请求接收端合并并校验文件。
        const QByteArray arrCompleteRequest = ReliablePacketCodec::BuildCompleteRequestPacket(stMetadata);

        if(!ReliableTransferIo::WritePacket(oControlSocket,arrCompleteRequest,fnCancelled,strErrorMessage))
        {
            //【协调恢复】数据线程已回收，下一轮重新建立控制连接并恢复缺失分片。
            continue;
        }

        emit SignalLogMessage(
                QStringLiteral("数据通道已完成，等待接收端合并及 SHA-256 校验。"));

        //第十一段：通过原控制连接，等待一个完整的最终回复包。
        //包头与缺失范围是不同类型；ReadPacket 的第三个参数输出包头。
        SReliablePacketHeader stResultHeader;
        QByteArray arrResultPayload;

        if(!ReliableTransferIo::ReadPacket(oControlSocket,arrControlBuffer,stResultHeader,arrResultPayload,g_nReliableResultWaitTimeoutMs,fnCancelled,strErrorMessage))
        {
            //【协调恢复】结果未能读到时重新握手；取消由外层循环条件处理。
            continue;
        }

        //第十二段：检查最终回复属于当前任务。
        if (stResultHeader.m_oTransferId != stMetadata.m_oTransferId)
        {
            emit SignalSendFinished(
                false,
                QStringLiteral("最终回复的任务 ID 不匹配。"));
            return;
        }

        //Error 的载荷是错误文本，不按 Result JSON 解析。
        if(stResultHeader.m_nMessageType == static_cast<quint16>(EReliableMessageType::ErrorPacket))
        {
            emit SignalSendFinished(
                        false,
                        QString::fromUtf8(arrResultPayload));
                    return;
        }

        //正常的最终回复必须是 Result，且文件信息一致。
        if (stResultHeader.m_nMessageType !=
                static_cast<quint16>(EReliableMessageType::ResultPacket) ||
            stResultHeader.m_nFileSize !=
                static_cast<quint64>(stMetadata.m_nFileSize) ||
            stResultHeader.m_nTotalPacketCount !=
                stMetadata.m_nTotalPacketCount)
        {
            emit SignalSendFinished(
                false,
                QStringLiteral("最终回复类型或文件信息不匹配。"));
            return;
        }

        //第十三段：解析接收端返回的最终结果。
        bool bSuccess = false;
        QString strResultMessage;
        if(!ReliablePacketCodec::ParseResultPayload(arrResultPayload,bSuccess,strResultMessage,arrMissingRanges,strErrorMessage))
        {
            emit SignalSendFinished(false,strErrorMessage);
            return;
        }

        //成功结果不应同时声明仍有缺失分片。
        if(bSuccess && !arrMissingRanges.isEmpty())
        {
            emit SignalSendFinished(
                false,
                QStringLiteral("成功结果包仍声明存在缺失分片。"));
            return;
        }

        //成功以接收端完成合并和校验后的 Result 为依据。
        if (bSuccess)
        {
            emit SignalSendFinished(true, strResultMessage);
            return;
        }

        //【协调恢复2】没有缺片却失败，保留接收端给出的校验或文件操作错误。
        if (arrMissingRanges.isEmpty())
        {
            emit SignalSendFinished(false, strResultMessage);
            return;
        }

        //【协调恢复3】仍有缺片时进入下一轮，以重新握手的恢复状态为准。
        strErrorMessage = strResultMessage;
        emit SignalLogMessage(
            QStringLiteral("最终检查仍有缺失分片，准备重新握手并补传。"));
    }

    //【协调恢复4】循环只会因取消或恢复轮数耗尽而到达这里。
    emit SignalSendFinished(
        false,
        fnCancelled()
            ? QStringLiteral("发送任务已取消。")
            : QStringLiteral("协调恢复次数超限：%1").arg(strErrorMessage));
}

bool ReliableSenderWorker::BuildMetadata(const QString &strFilePath, int nSegmentSize, int nThreadCount, SReliableFileMetadata &stMetadata, QString &strErrorMessage)
{
    //第一段：检查源文件并以只读方式打开。
    QFileInfo oFileInfo(strFilePath);

    if (!oFileInfo.exists() || !oFileInfo.isFile())
    {
        strErrorMessage = QStringLiteral("待发送文件不存在。");
        return false;
    }

    QFile oInputFile(strFilePath);

    if (!oInputFile.open(QIODevice::ReadOnly))
    {
        strErrorMessage = oInputFile.errorString();
        return false;
    }

    //当前协议暂不支持空文件。
    if (oInputFile.size() <= 0 ||
        nSegmentSize <= 0 ||
        nSegmentSize > g_nReliableMaxSegmentSize ||
        nThreadCount < 1 ||
        nThreadCount > g_nReliableMaxThreadCount)
    {
        strErrorMessage =
            QStringLiteral("源文件为空，或分片大小、线程数非法。");
        return false;
    }

    //第二段：记录文件信息，计算分片总数。
    stMetadata.m_strFileName = oFileInfo.fileName();
    stMetadata.m_nFileSize = oInputFile.size();

    //分片大小至少为 8 KiB。
    stMetadata.m_nSegmentSize = qMax(8 * 1024, nSegmentSize);

    //整除部分加上不足一个分片的尾部，避免加法形式可能溢出。
    //【公共计算提取】元信息准备和协议校验共用总分片数计算。
    const qint64 nPacketCount = GetReliablePacketCount(
        stMetadata.m_nFileSize,
        stMetadata.m_nSegmentSize);
     //Qt 5 的 QBitArray 使用 int 表示位数。
    if(nPacketCount > std::numeric_limits<int>::max())
    {
        strErrorMessage =
                    QStringLiteral("分片数量超过位图容量，请增大分片大小。");
                return false;
    }
    stMetadata.m_nTotalPacketCount = static_cast<quint32>(nPacketCount);
    stMetadata.m_nLaneCount = qMin(nThreadCount,static_cast<int>(nPacketCount));

    //第三段：跟之前一样，分批读取文件，增量计算摘要。
    //【CNG SHA-256】只替换摘要后端，保留读取块大小和取消检查。
    ReliableSha256 oShaHash;
    if (!oShaHash.Initialize(strErrorMessage))
    {
        return false;
    }
    qint64 nReadByteCount = 0;

    while (!oInputFile.atEnd())
    {
       //配合 Manager 的 requestInterruption() 取消耗时哈希计算。
       //这里的轮询是合适的，因为它不是一直空转检查，而是每处理一块文件数据时顺便检查一次。
       if (QThread::currentThread()->isInterruptionRequested())
       {
           strErrorMessage = QStringLiteral("发送任务已取消。");
           return false;
       }

       const QByteArray arrBuffer = oInputFile.read(1024 * 1024);
       if(oInputFile.error() != QFile::NoError)
       {
           strErrorMessage = QStringLiteral("计算哈希时读取文件失败：%1").arg(oInputFile.errorString());
           return false;
       }
       nReadByteCount += arrBuffer.size();
       if (!oShaHash.AddData(arrBuffer.constData(), arrBuffer.size(), strErrorMessage))
       {
           return false;
       }
    }

    //检查实际读取量是否与开始记录的文件大小一致。
    if(nReadByteCount != stMetadata.m_nFileSize)
    {
        strErrorMessage = QStringLiteral("计算摘要期间源文件大小发生变化。");
        return false;
    }

    QByteArray arrDigest;
    if (!oShaHash.Finish(arrDigest, strErrorMessage))
    {
        return false;
    }
    stMetadata.m_strSha256 = QString::fromLatin1(arrDigest.toHex());

    //第四段：生成续传身份和本次任务身份。
    stMetadata.m_strSessionKey = ReliablePacketCodec::BuildSessionKey(stMetadata);

    stMetadata.m_oTransferId = QUuid::createUuid();

    return ReliablePacketCodec::ValidateMetadata(stMetadata,strErrorMessage);
}

void ReliableSenderWorker::EmitTaskSnapshot(const QSharedPointer<SReliableSendTaskState> &pTaskState, int nTotalPacketCount, int &nLastCompletedCount)
{
    int nCompletedPacketCount = 0;
    QStringList arrLogMessages;

    //第一段：锁内读取当前进度，取走待报告的日志
    {
        QMutexLocker oLocker(&pTaskState->m_oMutex);

        nCompletedPacketCount = pTaskState->m_nConfirmedCount;

        //本地列表初始为空，交换后共享列表变为空，
        //原有日志交给当前函数处理，避免下次重复输出。
        arrLogMessages.swap(pTaskState->m_arrLogMessages);//把共享日志列表中的日志一次性取出来，同时清空共享日志列表，供后续新日志写入。(交换列表内容)
    }
    //离开上面的大括号，oLocker 析构，自动释放互斥锁。

    //第二段：锁外报告日志。
    for(auto strMessage : arrLogMessages)
    {
        emit SignalLogMessage(strMessage);
    }

    //第三段：完成数量变化时，再通知界面更新进度。
    if(nCompletedPacketCount != nLastCompletedCount)
    {
        nLastCompletedCount = nCompletedPacketCount;
        emit SignalSendProgress(nCompletedPacketCount,nTotalPacketCount);
    }
}
