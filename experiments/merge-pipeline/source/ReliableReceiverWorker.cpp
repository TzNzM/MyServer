#include "ReliablePacketCodec.h"
#include "ReliableReceiverWorker.h"

#include<QTcpSocket>

ReliableReceiverWorker::ReliableReceiverWorker(const QSharedPointer<ReliableReceiveSessionStore> &pStore)
    :QObject(nullptr),m_pSocket(),m_stConnection(),m_pStore(pStore)
{

}

ReliableReceiverWorker::~ReliableReceiverWorker()
{
    Stop();
}

void ReliableReceiverWorker::Stop()
{
    if(m_pSocket == nullptr)
    {
        return;
    }

    //先断开 socket 发往当前 Worker 的信号槽连接，
    //避免下面 abort() 时再次进入断开处理函数。
    m_pSocket->disconnect(this);

    //立即关闭连接，放弃尚未发送完的数据。
    m_pSocket->abort();

    //归还当前连接占用的通道。
    m_pStore->DetachSession(m_stConnection.m_pSession,m_stConnection.m_nLaneIndex);

    //释放当前连接对共享会话的持有。
    m_stConnection.m_pSession.clear();

    //socket 已关闭，释放对象并清空成员指针。
    delete m_pSocket;
    m_pSocket = nullptr;

}

void ReliableReceiverWorker::AcceptConnection(qintptr nSocketDescriptor)
{
    //此函数应在 Worker 所属的接收线程执行。
    //socket 在这里创建，因此也属于该接收线程。
    m_pSocket = new QTcpSocket(this);

    //接管已有连接
    if(!m_pSocket->setSocketDescriptor(nSocketDescriptor) ||
            m_pStore->IsStopping())
    {
        emit SignalLogMessage(
                    QStringLiteral("接管连接失败或接收任务正在停止。"));

        Stop();

        emit SignalWorkerFinished();
        return;
    }

    //限制 QTcpSocket 内部读缓冲区的大小。不一定非得做
    //m_pSocket->setReadBufferSize(static_cast<qint64>(g_nReliableMaxPayloadSize) + g_nReliablePacketHeaderLength);

    connect(m_pSocket,&QTcpSocket::disconnected,this,&ReliableReceiverWorker::OnSocketDisconnected);
    connect(m_pSocket,&QTcpSocket::readyRead,this,&ReliableReceiverWorker::OnSocketReadyRead);


    //接管连接时可能已经有数据，主动检查一次。
    if(m_pSocket->bytesAvailable() > 0)
    {
        OnSocketReadyRead();
    }
}

void ReliableReceiverWorker::OnSocketReadyRead()
{
   if (m_pSocket == nullptr || m_pStore->IsStopping())
   {
       return;
   }

   //读取当前可读字节，接在上次未处理完的字节后面。
   m_stConnection.m_arrReceiveBuffer.append(m_pSocket->readAll());

   while(!m_pStore->IsStopping())
   {
       SReliablePacketHeader stHeader;
       QByteArray arrPayload;
       bool bReady = false;
       QString strErrorMessage;

//       这里要再次区分：
//       TryTakePacket() 返回 false
//       表示包头等协议格式有问题。
//       TryTakePacket() 返回 true，但 bReady 为 false
//       表示数据还没到齐，是正常的半包，不应该断开连接。
       //每次最多取出一个完整包，其余字节仍保留在缓存中。
       if(!ReliablePacketCodec::TryTakePacket(m_stConnection.m_arrReceiveBuffer,stHeader,arrPayload,bReady,strErrorMessage))
       {
            emit SignalLogMessage(QStringLiteral("非法协议包，关闭连接：%1").arg(strErrorMessage));
            m_pSocket->abort();
            return;
       }

       if(!bReady)
       {
           return;//半包，保留缓存，等待下次 readyRead。
       }

       if(!HandlePacket(m_pSocket,m_stConnection,stHeader,arrPayload,strErrorMessage))
       {
           //使用当前包的任务 ID 构造错误回复。
           SReliableFileMetadata stMetadata;
           stMetadata.m_oTransferId = stHeader.m_oTransferId;

           m_pSocket->write(ReliablePacketCodec::BuildErrorPacket(stMetadata,strErrorMessage));
           emit SignalLogMessage(QStringLiteral("接收处理失败：%1").arg(strErrorMessage));
           //通知 Qt：当前 TCP 连接准备关闭，但先把已经排队、等待发送的数据尽可能发送出去，再关闭连接。
           m_pSocket->disconnectFromHost();
           return;
       }
   }

}

//这里暂时不删除 socket，而是通知工作结束
void ReliableReceiverWorker::OnSocketDisconnected()
{
    m_pStore->DetachSession(m_stConnection.m_pSession,m_stConnection.m_nLaneIndex);

    m_stConnection.m_pSession.clear();

    emit SignalWorkerFinished();
}

bool ReliableReceiverWorker::HandlePacket(QTcpSocket *pSocket, ReliableReceiverWorker::SConnectionState &stConnection, const SReliablePacketHeader &stHeader, const QByteArray &arrPayload, QString &strErrorMessage)
{
    strErrorMessage.clear();
    const EReliableMessageType eMessageType = static_cast<EReliableMessageType>(stHeader.m_nMessageType);

    //假设当前包是MetadataPacket
    if (eMessageType == EReliableMessageType::MetadataPacket)
    {
        SReliableFileMetadata stMetadata;
        int nLaneIndex = -2;

        //前面是解析包，这里是解析正文里，如果是stMetadata的情况
        if(!ReliablePacketCodec::ParseMetadataPayload(arrPayload,
                                                      stMetadata,
                                                      strErrorMessage,
                                                      &nLaneIndex))
        {
            return false;
        }

        //Metadata JSON 不携带任务 UUID，从包头补入。
        stMetadata.m_oTransferId = stHeader.m_oTransferId;
        if (stHeader.m_nFileSize !=
                static_cast<quint64>(stMetadata.m_nFileSize) ||
            stHeader.m_nTotalPacketCount !=
                stMetadata.m_nTotalPacketCount ||
            stHeader.m_nWindowSize < 1 ||
            stHeader.m_nWindowSize >
                static_cast<quint32>(g_nReliableMaxWindowSize))
        {
            strErrorMessage =
                QStringLiteral("握手包头与元信息不一致，或发送窗口非法。");
            return false;
        }

        if(stConnection.m_pSession.isNull())
        {
            //当前连接第一次握手，向 Store 申请加入会话。
            stConnection.m_pSession = m_pStore->AttachSession(stMetadata,nLaneIndex,strErrorMessage);

            if(stConnection.m_pSession.isNull())
            {
                return false;
            }

            stConnection.m_nLaneIndex = nLaneIndex;
        }
        else
        {
            //已握手连接允许重复握手，但不能切换文件或通道。
            const SReliableFileMetadata stExistingMetadata =
                        stConnection.m_pSession->GetMetadata();
            if (stConnection.m_nLaneIndex != nLaneIndex ||
                stExistingMetadata.m_oTransferId !=
                    stMetadata.m_oTransferId ||
                stExistingMetadata.m_strSessionKey !=
                    stMetadata.m_strSessionKey)
            {
                strErrorMessage =
                    QStringLiteral("一条连接不能切换文件会话或通道。");
                return false;
            }

        }

        //会话已准备好，告诉发送端哪些分片还缺失。
        pSocket->write(stConnection.m_pSession->BuildResumeStatePacket());

        return true;
    }

    //除 Metadata 外，其他业务消息必须先完成握手，
    //并且包头中的任务 ID 必须属于当前连接加入的会话。
    if(stConnection.m_pSession.isNull() || stHeader.m_oTransferId != stConnection.m_pSession->GetMetadata().m_oTransferId)
    {
        strErrorMessage = QStringLiteral("当前连接尚未加入该文件会话。");
        return false;
    }
    const SReliableFileMetadata stMetadata =  stConnection.m_pSession->GetMetadata();

    if(eMessageType == EReliableMessageType::DataPacket)
    {
        bool bAccepted = false;
        bool bNewPacket = false;
        QString strWarningMessage;

        //具体的通道归属、偏移和长度检查，以及文件写入，
        //全部交给 Session，Worker 不重复实现。
        if (!stConnection.m_pSession->AcceptDataPacket(
                stHeader,
                arrPayload,
                stConnection.m_nLaneIndex,
                bAccepted,
                bNewPacket,
                strWarningMessage,
                strErrorMessage))
        {
            return false;
        }

        //状态保存失败属于警告，分片本身可能已经成功写入。
        if (!strWarningMessage.isEmpty())
        {
            emit SignalLogMessage(strWarningMessage);
        }

        //只有新收到的分片才触发本次进度通知。
        if(bNewPacket)
        {
            emit SignalReceiveProgress(stMetadata.m_oTransferId,stMetadata.m_strFileName,
                                       static_cast<int>(stConnection.m_pSession->GetReceivedPacketCount()),
                                       static_cast<int>(stMetadata.m_nTotalPacketCount));
        }

        //新分片和已收到的重复分片，都可以回复 ACK。
        if(bAccepted)
        {
            pSocket->write(ReliablePacketCodec::BuildAckPacket(stMetadata,stHeader.m_nPacketSequence));
        }
        return true;
    }

    if(eMessageType == EReliableMessageType::CompleteRequestPacket)
    {
        //完成请求由控制连接统一发起，
        //各数据通道只负责自己所属的分片。
        if(stConnection.m_nLaneIndex != -1 ||
                stHeader.m_nTotalPacketCount !=
                    stMetadata.m_nTotalPacketCount ||
                stHeader.m_nFileSize !=
                    static_cast<quint64>(stMetadata.m_nFileSize))
        {
            strErrorMessage =
                QStringLiteral("完成请求的连接身份或文件信息不匹配。");
            return false;
        }

        QList<SReliablePacketRange> arrMissingRanges;
        QString strFinalPath;
        QString strTimingMessage;

        //Session 检查缺失范围，分片齐全时执行合并和 SHA-256 校验。
        const bool bSuccess = stConnection.m_pSession->VerifyAndFinalizeSession(
            arrMissingRanges, strFinalPath, strErrorMessage, &strTimingMessage);
        //【合并计时】Session 只生成汇总，由 Worker 沿既有日志信号转发到接收界面。
        if (!strTimingMessage.isEmpty())
        {
            emit SignalLogMessage(strTimingMessage);
        }
        const QString strResultMessage = bSuccess
            ? QStringLiteral("各通道接收、顺序合并及 SHA-256 校验完成。")
            : strErrorMessage;

        //无论最终成功还是失败，都通过 Result 返回本次完成检查的结果。
        pSocket->write(ReliablePacketCodec::BuildResultPacket(stMetadata,bSuccess,strResultMessage,arrMissingRanges));

        if(bSuccess)
        {
            emit SignalReceiveFinished(stMetadata.m_oTransferId,strFinalPath);
            emit SignalLogMessage(QStringLiteral("接收完成：%1").arg(strFinalPath));
        }

        return true;
    }

    strErrorMessage = QStringLiteral("接收端不接受当前消息类型。");
    return false;

}
