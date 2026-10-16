#include "ReliableTransferManager.h"

#include "ReliablePacketCodec.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

ReliableTransferManager::ReliableTransferManager(QObject *pParent)
    : QObject(pParent),
      m_strOutputDirectory(QDir::currentPath()),
      m_pTcpServer(new QTcpServer(this)),
      m_mapReceiverBuffer(),
      m_mapReceiverSession(),
      m_pSenderThread(nullptr),
      m_pReliableSenderWorker(nullptr)
{
    connect(m_pTcpServer,&QTcpServer::newConnection,this,&ReliableTransferManager::OnNewConnection);
}

ReliableTransferManager::~ReliableTransferManager()
{
    StopListening();

    if(m_pSenderThread != nullptr)//如果当前的传送的线程没有关闭
    {
        m_pSenderThread->quit();
        m_pSenderThread->wait();
        delete m_pSenderThread;
        m_pSenderThread = nullptr;
        m_pReliableSenderWorker = nullptr;
    }
}

void ReliableTransferManager::SetOutputDirectory(const QString &strOutputDirectory)
{
    if(!strOutputDirectory.isEmpty())
    {
        m_strOutputDirectory = strOutputDirectory;
    }
}

bool ReliableTransferManager::StartListening(const QString &strBindIp, int nPort, QString &strErrorMessage)
{
    QHostAddress oBindAddress;
    const QString strEffectiveBindIp = strBindIp.trimmed().isEmpty() ? QStringLiteral("0.0.0.0") : strBindIp.trimmed();
    if(!oBindAddress.setAddress(strEffectiveBindIp)){
        strErrorMessage = QStringLiteral("监听 IP 非法：%1").arg(strBindIp);
        return false;
    }

    StopListening();//这里是为了防止之前的没有断干净
    if(!m_pTcpServer->listen(oBindAddress,static_cast<quint16>(nPort)))
    {
        strErrorMessage = m_pTcpServer->errorString();
        return false;
    }

    emit SignalLogMessage(QStringLiteral("[TCP-Reliable] 已开始监听 %1:%2")
                          .arg(oBindAddress.toString())
                          .arg(nPort));
    return true;
}

void ReliableTransferManager::StopListening()
{
    const QList<QTcpSocket*> arrSocketList = m_mapReceiverBuffer.keys();//获取当前仍在缓存区中的socket
    for(QTcpSocket* pSocket : arrSocketList)
    {
        if(pSocket != nullptr)
        {
            pSocket->disconnect();
            pSocket->close();
            pSocket->deleteLater();
        }
    }

    m_mapReceiverBuffer.clear();

    //持久化当前内容（可能会有传输错误导致的失败，所以这里能够为后面的重传做准备）
    for(SReceiverSessionState &stSessionState : m_mapReceiverSession)
    {
        QString strIgnoreErrorMessage;
        PersistSessionState(stSessionState,strIgnoreErrorMessage);
    }

    if(m_pTcpServer->isListening())
    {
        m_pTcpServer->close();
    }
}

bool ReliableTransferManager::StartSend(const QString &strFilePath, const QString &strTargetIp, int nPort, int nWindowSize, int nSegmentSize, QString &strErrorMessage)
{
    //To do
}

void ReliableTransferManager::OnNewConnection()
{
    while (m_pTcpServer->hasPendingConnections())//只要还有等待取出的新连接，就继续处理
    {
        QTcpSocket* pSocket = m_pTcpServer->nextPendingConnection();
        if(pSocket == nullptr)
        {//表示这次没有取到有效的连接对象，无法继续对它操作
            continue;
        }

        m_mapReceiverBuffer.insert(pSocket,QByteArray());
        connect(pSocket,&QTcpSocket::readyRead,this,&ReliableTransferManager::OnSocketReadyRead);
        connect(pSocket,&QTcpSocket::disconnected,this,&ReliableTransferManager::OnSocketDisconnected);
        emit SignalLogMessage(QStringLiteral("[TCP-Reliable] 新连接：%1").arg(pSocket->peerAddress().toString()));
    }
}

void ReliableTransferManager::OnSocketReadyRead()
{
    QTcpSocket *pSocket = qobject_cast<QTcpSocket *>(sender());//谁调用这个slots的
    if(pSocket == nullptr || !m_mapReceiverBuffer.contains(pSocket))
    {
        //如果不存在，则返回
        return;
    }

    QByteArray &arrReceiverBuffer = m_mapReceiverBuffer[pSocket];
    arrReceiverBuffer.append(pSocket->readAll());//一次读所有

    while (arrReceiverBuffer.size() >= ReliablePacketCodec::GetHeaderLength())
    {//如果剩下来的大于完整表头的长度
        SReliablePacketHeader stPacketHeader;
        QString strErrorMessage;
        if(!ReliablePacketCodec::ParsePacketHeader(arrReceiverBuffer.left(ReliablePacketCodec::GetHeaderLength()),stPacketHeader,strErrorMessage))
        {//如果解析失败，证明不是我们的包
            emit SignalLogMessage(QStringLiteral("[TCP-Reliable] 包头解析失败：%1").arg(strErrorMessage));
            pSocket->disconnectFromHost();
            return;
        }
        const int nTotalPacketLength = ReliablePacketCodec::GetHeaderLength() + static_cast<int>(stPacketHeader.m_nPayloadSize);
        if(arrReceiverBuffer.size() < nTotalPacketLength)
        {
            break;//如果现在这个包还没完整传过来，则结束
        }
        const QByteArray arrPayload = arrReceiverBuffer.mid(
            ReliablePacketCodec::GetHeaderLength(),          // 从哪里开始取
            static_cast<int>(stPacketHeader.m_nPayloadSize)  // 取多少字节
        );//这一步是去取数据的
        arrReceiverBuffer.remove(0,nTotalPacketLength);

        switch (static_cast<EReliableMessageType>(stPacketHeader.m_nMessageType))
        {
        case EReliableMessageType::MetadataPacket:
                HandleMetadataPacket(pSocket,stPacketHeader,arrPayload);
            break;
        case EReliableMessageType::DataPacket:
                HandleDataPacket(pSocket,stPacketHeader,arrPayload);
            break;
        case EReliableMessageType::CompleteRequestPacket:
                HandleCompleteRequestPacket(pSocket, stPacketHeader);
            break;
        default:
            break;
        }
    }

}

void ReliableTransferManager::OnSocketDisconnected()
{
    //在槽函数里，找出本次发出信号的那个 socket，并把它转换成 QTcpSocket *。
    //sender()能取得发出信号的对象。但它返回的是通用类型：QObject *
    QTcpSocket *pSocket = qobject_cast<QTcpSocket *>(sender());

    if(pSocket == nullptr)
    {
        return;
    }

    //持久化操作
    for(SReceiverSessionState &stSessionState : m_mapReceiverSession){
        QString strIgnoreErrorMessage;
        PersistSessionState(stSessionState, strIgnoreErrorMessage);
    }

    m_mapReceiverBuffer.remove(pSocket);
    pSocket->deleteLater();
}

void ReliableTransferManager::HandleMetadataPacket(QTcpSocket *pSocket, const SReliablePacketHeader &stPacketHeader, const QByteArray &arrPayload)
{
    QString strErrorMessage;
    SReliableFileMetadata stMetadata;
    if(!ReliablePacketCodec::ParseMetadataPayload(arrPayload,stMetadata,strErrorMessage)){
        const QByteArray arrErrorPacket = ReliablePacketCodec::BuildErrorPacket(SReliableFileMetadata(),QStringLiteral("元信息解析失败：%1").arg(strErrorMessage));
        pSocket->write(arrErrorPacket);
        pSocket->waitForBytesWritten(g_nReliableControlWaitTimeoutMs);
        emit SignalLogMessage(QStringLiteral("[TCP-Reliable] 元信息解析失败：%1").arg(strErrorMessage));
        return;
    }

    stMetadata.m_oTransferId = stPacketHeader.m_oTransferId;

    SReceiverSessionState stSessionState;
    if(!LoadOrCreateSession(stMetadata,stSessionState,strErrorMessage))
    {//如果创建失败
        const QByteArray arrErrorPacket = ReliablePacketCodec::BuildErrorPacket(stMetadata,strErrorMessage);
        pSocket->write(arrErrorPacket);
        pSocket->waitForBytesWritten(g_nReliableControlWaitTimeoutMs);
        emit SignalLogMessage(QStringLiteral("[TCP-Reliable] 会话恢复失败：%1").arg(strErrorMessage));
        return;
    }

    m_mapReceiverSession[stMetadata.m_oTransferId] = stSessionState;
    const QList<SReliablePacketRange> arrMissingRangeList = BuildMissingRanges(stSessionState);
    const QByteArray arrResumeStatePacket = ReliablePacketCodec::BuildResumeStatePacket(stSessionState.m_stMetadata,arrMissingRangeList,stSessionState.m_nReceivedPacketCount);//如果是新文件就是全没发过
    pSocket->write(arrResumeStatePacket);
    pSocket->waitForBytesWritten(g_nReliableControlWaitTimeoutMs);

    emit SignalLogMessage(QStringLiteral("[TCP-Reliable] 会话已准备完成：%1，缺失范围数：%2")
                              .arg(stMetadata.m_strFileName)
                              .arg(arrMissingRangeList.size()));

}

void ReliableTransferManager::HandleDataPacket(QTcpSocket *pSocket, const SReliablePacketHeader &stPacketHeader, const QByteArray &arrPayload)
{
    if(!m_mapReceiverSession.contains(stPacketHeader.m_oTransferId))
    {//接收到未知的分片
        SReliableFileMetadata stMetadata;
        stMetadata.m_oTransferId = stPacketHeader.m_oTransferId;
        const QByteArray arrErrorPacket = ReliablePacketCodec::BuildErrorPacket(stMetadata,QStringLiteral("收到未知会话分片。"));
        pSocket->write(arrErrorPacket);
        pSocket->waitForBytesWritten(g_nReliableControlWaitTimeoutMs);
        emit SignalLogMessage(QStringLiteral("[TCP-Reliable] 收到未知会话分片。"));
        return;
    }

    QString strErrorMessage;
    SReceiverSessionState &stSessionState = m_mapReceiverSession[stPacketHeader.m_oTransferId]; //因为后面需要直接修改 m_mapReceiverSession 中保存的会话状态，所以这里取引用
    const int nPacketIndex = static_cast<int>(stPacketHeader.m_nPacketSequence);

    if(nPacketIndex < 0 || nPacketIndex >= stSessionState.m_oReceivedPacketMap.size())
    {
        const QByteArray arrErrorPacket = ReliablePacketCodec::BuildErrorPacket(stSessionState.m_stMetadata,
                                                                                QStringLiteral("分片序号越界：%1").arg(stPacketHeader.m_nPacketSequence));
        pSocket->write(arrErrorPacket);
        pSocket->waitForBytesWritten(g_nReliableControlWaitTimeoutMs);
        emit SignalLogMessage(QStringLiteral("[TCP-Reliable] 分片序号越界：%1").arg(stPacketHeader.m_nPacketSequence));
        return;
    }

    if(!stSessionState.m_oReceivedPacketMap.testBit(nPacketIndex))
    {//这里接收到的是之前没接收到的分片的时候
        QFile oPartFile(stSessionState.m_strPartFilePath);
        if(!oPartFile.open(QIODevice::ReadWrite))
        {//如果临时文件打不开
            const QByteArray arrErrorPacket = ReliablePacketCodec::BuildErrorPacket(stSessionState.m_stMetadata,
                                                                                    QStringLiteral("临时文件打开失败：%1").arg(oPartFile.errorString()));
            pSocket->write(arrErrorPacket);
            pSocket->waitForBytesWritten(g_nReliableAckPollIntervalMs);
            emit SignalLogMessage(QStringLiteral("[TCP-Reliable] 临时文件打开失败：%1").arg(oPartFile.errorString()));
            return;
        }

        if(!oPartFile.seek(static_cast<qint64>(stPacketHeader.m_nPayloadOffset)) ||
            oPartFile.write(arrPayload) != arrPayload.size()) //这里已经进行了partfile文件的写入
        {
            const QByteArray arrErrorPacket = ReliablePacketCodec::BuildErrorPacket(stSessionState.m_stMetadata,
                                                                                    QStringLiteral("分片写入失败。"));
            pSocket->write(arrErrorPacket);
            pSocket->waitForBytesWritten(g_nReliableControlWaitTimeoutMs);
            emit SignalLogMessage(QStringLiteral("[TCP-Reliable] 分片写入失败。"));
            return;
        }

        //做一些接收到该数据的逻辑调整
        stSessionState.m_oReceivedPacketMap.setBit(nPacketIndex,true);
        ++stSessionState.m_nReceivedPacketCount;
        ++stSessionState.m_nUnsavedPacketCount;

        if(stSessionState.m_nUnsavedPacketCount >= 32)
        {
            if(PersistSessionState(stSessionState,strErrorMessage))
            {//做持久化数据状态的操作，防止多次操作，所以累计着操作
                stSessionState.m_nUnsavedPacketCount = 0;
            }else
            {
                emit SignalLogMessage(QStringLiteral("[TCP-Reliable] 状态保存失败：%1").arg(strErrorMessage));
            }
        }
        //发送进度消息
        emit SignalReceiveProgress(stSessionState.m_stMetadata.m_strFileName,static_cast<int>(stSessionState.m_nReceivedPacketCount),stSessionState.m_stMetadata.m_nTotalPacketCount);
    }

    const QByteArray arrAckPacket = ReliablePacketCodec::BuildAckPacket(stSessionState.m_stMetadata,stPacketHeader.m_nPacketSequence);

    pSocket->write(arrAckPacket);
    pSocket->waitForBytesWritten(g_nReliableControlWaitTimeoutMs);
}

void ReliableTransferManager::HandleCompleteRequestPacket(QTcpSocket *pSocket, const SReliablePacketHeader &stPacketHeader)
{
    if(!m_mapReceiverSession.contains(stPacketHeader.m_oTransferId))
    {
        SReliableFileMetadata stMetadata;
        stMetadata.m_oTransferId = stPacketHeader.m_oTransferId;
        const QByteArray arrErrorPacket = ReliablePacketCodec::BuildErrorPacket(stMetadata,
                                                                                QStringLiteral("完成请求对应的会话不存在。"));
        pSocket->write(arrErrorPacket);
        pSocket->waitForBytesWritten(g_nReliableControlWaitTimeoutMs);
        return;
    }

    QString strErrorMessage;
    SReceiverSessionState &stSessionState = m_mapReceiverSession[stPacketHeader.m_oTransferId];
    const QList<SReliablePacketRange> arrMissingRangeList = BuildMissingRanges(stSessionState);
    if(!arrMissingRangeList.isEmpty())
    {//还有一部分缺失
        const QByteArray arrResultPacket = ReliablePacketCodec::BuildResultPacket(stSessionState.m_stMetadata,
                                                                                  false,
                                                                                  QStringLiteral("接收端仍有缺失分片。"),
                                                                                  arrMissingRangeList);
        pSocket->write(arrResultPacket);
        pSocket->waitForBytesWritten(g_nReliableAckPollIntervalMs);
        PersistSessionState(stSessionState, strErrorMessage);
        return;
    }

    if(!VerifyAndFinalizeSession(stSessionState, strErrorMessage))
    {//校验完整接收数据是否正确
        const QByteArray arrResultPacket = ReliablePacketCodec::BuildResultPacket(stSessionState.m_stMetadata,
                                                                                  false,
                                                                                  strErrorMessage,
                                                                                  QList<SReliablePacketRange>());
        pSocket->write(arrResultPacket);
        pSocket->waitForBytesWritten(g_nReliableAckPollIntervalMs);
        PersistSessionState(stSessionState, strErrorMessage);
        return;
    }

    const QByteArray arrResultPacket = ReliablePacketCodec::BuildResultPacket(stSessionState.m_stMetadata,
                                                                              true,
                                                                              QStringLiteral("文件接收、校验与落盘完成。"),
                                                                              QList<SReliablePacketRange>());
    pSocket->write(arrResultPacket);
    pSocket->waitForBytesWritten(g_nReliableAckPollIntervalMs);
    emit SignalReceiveFinished(stSessionState.m_strFinalFilePath);
    emit SignalLogMessage(QStringLiteral("[TCP-Reliable] 文件接收完成：%1").arg(stSessionState.m_strFinalFilePath));
    m_mapReceiverSession.remove(stPacketHeader.m_oTransferId);//当前文件任务已完成故而移除
}

bool ReliableTransferManager::VerifyAndFinalizeSession(ReliableTransferManager::SReceiverSessionState &stSessionState, QString &strErrorMessage)
{
    QFile oPartFile(stSessionState.m_strPartFilePath);
    if(!oPartFile.open(QIODevice::ReadOnly))
    {
        strErrorMessage = QStringLiteral("临时文件打开失败：%1").arg(oPartFile.errorString());
        return false;
    }

    QCryptographicHash oMd5Hash(QCryptographicHash::Md5);
    QCryptographicHash oShaHash(QCryptographicHash::Sha256);

    while (!oPartFile.atEnd())
    {
        const QByteArray arrBuffer = oPartFile.read(1024 * 1024);
        oMd5Hash.addData(arrBuffer);
        oShaHash.addData(arrBuffer);
    }

    const QString strCalculatedMd5 = QString::fromLatin1(oMd5Hash.result().toHex());
    const QString strCalculatedSha = QString::fromLatin1(oShaHash.result().toHex());
    if (strCalculatedMd5.compare(stSessionState.m_stMetadata.m_strMd5, Qt::CaseInsensitive) != 0 ||
        strCalculatedSha.compare(stSessionState.m_stMetadata.m_strSha256, Qt::CaseInsensitive) != 0)
    {
        strErrorMessage = QStringLiteral("接收端文件校验失败，MD5 或 SHA256 不匹配。");
        PersistSessionState(stSessionState, strErrorMessage);
        return false;
    }
    //校验成功之后，开始真正处理文件
    oPartFile.close();
    stSessionState.m_strFinalFilePath = BuildAvailableFilePath(stSessionState.m_strFinalFilePath);
    if(stSessionState.m_strFinalFilePath.isEmpty())
    {
        strErrorMessage = QStringLiteral("无法生成可用文件路径：重名候选已耗尽。");
        return false;
    }

    if(!QFile::rename(stSessionState.m_strPartFilePath,stSessionState.m_strFinalFilePath))
    {
        strErrorMessage = QStringLiteral("最终文件生成失败。");
        return false;
    }

    QFile::remove(stSessionState.m_strStateFilePath);
    QDir(stSessionState.m_strSessionDirectory).removeRecursively();//递归删除 stSessionState.m_strSessionDirectory 指定的目录及其内部所有文件和子目录。
    stSessionState.m_bCompleted = true;
    return true;
}

bool ReliableTransferManager::LoadOrCreateSession(const SReliableFileMetadata &stMetadata, ReliableTransferManager::SReceiverSessionState &stSessionState, QString &strErrorMessage)
{
    QDir oOutputDirectory(m_strOutputDirectory);
    if(!oOutputDirectory.exists() && !oOutputDirectory.mkpath(QStringLiteral("."))){//确保输出目录存在，如果不存在就创建它,"."：指 oOutputDirectory 自己表示的目录
        strErrorMessage =  QStringLiteral("输出目录创建失败：%1").arg(m_strOutputDirectory);
        return false;
    }

    //这里只是在构建文件地址，并未真正开始创建文件
    stSessionState.m_stMetadata = stMetadata;
    stSessionState.m_strSessionDirectory = oOutputDirectory.filePath("session_%1").arg(stMetadata.m_strSessionKey.left(16));//以sessionKey来命名的文件夹
    stSessionState.m_strStateFilePath = QDir(stSessionState.m_strSessionDirectory).filePath(QStringLiteral("state.json"));
    stSessionState.m_strPartFilePath = QDir(stSessionState.m_strSessionDirectory).filePath(QStringLiteral("payload.part"));

    if(!QDir().mkpath(stSessionState.m_strSessionDirectory)){
        strErrorMessage = QStringLiteral("会话目录创建失败：%1").arg(stSessionState.m_strSessionDirectory);
        return false;
    }

    if(QFileInfo::exists(stSessionState.m_strStateFilePath))
    {//如果存在，证明这里是断点重续(或者已经传输完毕)
        QFile oStateFile(stSessionState.m_strStateFilePath);
        if(!oStateFile.open(QIODevice::ReadOnly)){
            strErrorMessage = QStringLiteral("状态文件打开失败：%1").arg(oStateFile.errorString());
            return false;
        }
        const QJsonDocument oJsonDocument = QJsonDocument::fromJson(oStateFile.readAll());
        const QJsonObject oJsonObject = oJsonDocument.object();
        const qint64 nStateFileSize = oJsonObject.value(QStringLiteral("fileSize")).toString().toLongLong();
        const int nStateSegmentSize = oJsonObject.value(QStringLiteral("segmentSize")).toInt();
        const quint32 nStatePacketCount = static_cast<quint32>(oJsonObject.value(QStringLiteral("totalPacketCount")).toInt());
        const QString strStateMd5 = oJsonObject.value(QStringLiteral("md5")).toString();
        const QString strStateSha256 = oJsonObject.value(QStringLiteral("sha256")).toString();
        // 判断能不能进行断点重续
        const bool bCanResume = QFileInfo::exists(stSessionState.m_strPartFilePath) &&
                                nStateFileSize == stMetadata.m_nFileSize &&
                                nStateSegmentSize == stMetadata.m_nSegmentSize &&
                                nStatePacketCount == stMetadata.m_nTotalPacketCount &&
                                strStateMd5.compare(stMetadata.m_strMd5,Qt::CaseInsensitive) == 0 && //忽略字母大小写进行比较
                                strStateSha256.compare(stMetadata.m_strSha256, Qt::CaseInsensitive) == 0;
        if(bCanResume){//断点重续的必要数据的填写
            stSessionState.m_strFinalFilePath = oJsonObject.value(QStringLiteral("finalFilePath")).toString();
            stSessionState.m_oReceivedPacketMap = DecodeBitArray(oJsonObject.value(QStringLiteral("receivedPacketBitmap")).toString(),static_cast<int>(stMetadata.m_nTotalPacketCount));
            stSessionState.m_nReceivedPacketCount = static_cast<quint32>(CountTrueBits(stSessionState.m_oReceivedPacketMap));
            stSessionState.m_stMetadata.m_oTransferId = stMetadata.m_oTransferId;
            stSessionState.m_stMetadata.m_nFileSize = stMetadata.m_nFileSize;
            stSessionState.m_stMetadata.m_nSegmentSize = stMetadata.m_nSegmentSize;
            stSessionState.m_stMetadata.m_nTotalPacketCount = stMetadata.m_nTotalPacketCount;
            stSessionState.m_stMetadata.m_strMd5 = stMetadata.m_strMd5;
            stSessionState.m_stMetadata.m_strSha256 = stMetadata.m_strSha256;
            stSessionState.m_stMetadata.m_strFileName = stMetadata.m_strFileName;
            stSessionState.m_stMetadata.m_strSessionKey = stMetadata.m_strSessionKey;
            return true;
        }

        //如果对不上的话，就证明原来的这些文件没用了，直接删除掉了
        QFile::remove(stSessionState.m_strStateFilePath);
        QFile::remove(stSessionState.m_strPartFilePath);
    }
    //这里就是直接创建的时候要做的事情了
    const QString strAvailablePath  = BuildAvailableFilePath(oOutputDirectory.filePath(stMetadata.m_strFileName));
    if(strAvailablePath.isEmpty())
    {
        strErrorMessage = QStringLiteral("无法生成可用文件路径：重名候选已耗尽。");
        return false;
    }
    stSessionState.m_strFinalFilePath = strAvailablePath;
    stSessionState.m_oReceivedPacketMap.resize(static_cast<int>(stMetadata.m_nTotalPacketCount));
    stSessionState.m_oReceivedPacketMap.fill(false);

    QFile oPartFile(stSessionState.m_strPartFilePath);
    if(!oPartFile.open(QIODevice::ReadWrite)){
        strErrorMessage = QStringLiteral("临时文件创建失败：%1").arg(oPartFile.errorString());
        return false;
    }

    if(!oPartFile.resize(stMetadata.m_nFileSize)){//临时先预分配文件（防止过大分不下来）
        strErrorMessage = QStringLiteral("临时文件预分配失败。");
        return false;
    }
    return PersistSessionState(stSessionState,strErrorMessage);
}

bool ReliableTransferManager::PersistSessionState(const ReliableTransferManager::SReceiverSessionState &stSessionState, QString &strErrorMessage) const
{
    QJsonObject oJsonObject;
    oJsonObject.insert(QStringLiteral("sessionKey"), stSessionState.m_stMetadata.m_strSessionKey);
    oJsonObject.insert(QStringLiteral("fileName"), stSessionState.m_stMetadata.m_strFileName);
    oJsonObject.insert(QStringLiteral("fileSize"), QString::number(stSessionState.m_stMetadata.m_nFileSize));
    oJsonObject.insert(QStringLiteral("segmentSize"), stSessionState.m_stMetadata.m_nSegmentSize);
    oJsonObject.insert(QStringLiteral("totalPacketCount"), static_cast<int>(stSessionState.m_stMetadata.m_nTotalPacketCount));
    oJsonObject.insert(QStringLiteral("md5"), stSessionState.m_stMetadata.m_strMd5);
    oJsonObject.insert(QStringLiteral("sha256"), stSessionState.m_stMetadata.m_strSha256);
    oJsonObject.insert(QStringLiteral("finalFilePath"), stSessionState.m_strFinalFilePath);
    oJsonObject.insert(QStringLiteral("receivedPacketCount"), static_cast<int>(stSessionState.m_nReceivedPacketCount));
    oJsonObject.insert(QStringLiteral("receivedPacketBitmap"), EncodeBitArray(stSessionState.m_oReceivedPacketMap));

    QFile oStateFile(stSessionState.m_strStateFilePath);
    if(!oStateFile.open(QIODevice::WriteOnly | QIODevice::Truncate)){//QIODevice::Truncate：清空原有内容
        strErrorMessage =  QStringLiteral("状态文件写入失败：%1").arg(oStateFile.errorString());
        return false;
    }

    oStateFile.write(QJsonDocument(oJsonObject).toJson(QJsonDocument::Compact));
    return true;
}

QList<SReliablePacketRange> ReliableTransferManager::BuildMissingRanges(const ReliableTransferManager::SReceiverSessionState &stSessionState) const
{
    QList<SReliablePacketRange> arrMissingRangeList;
    const int nBitCount = stSessionState.m_oReceivedPacketMap.size();

    int nRangeStartIndex = -1;
    for(int nIndex = 0; nIndex < nBitCount; ++nIndex)
    {
        const bool bReceived = stSessionState.m_oReceivedPacketMap.testBit(nIndex);
        if(!bReceived && nRangeStartIndex < 0)
        {//当没有收到时，且StartIndex还未赋值时
            nRangeStartIndex = nIndex;
        }else if (bReceived && nRangeStartIndex >= 0)
        {//当第一次有收到时
            arrMissingRangeList.append(SReliablePacketRange(static_cast<quint64>(nRangeStartIndex),static_cast<quint64>(nIndex - 1)));
            nRangeStartIndex = -1;
        }
    }

    //如果到最后还有一部分没有收到，则加上
    if(nRangeStartIndex >= 0){
        arrMissingRangeList.append(SReliablePacketRange(static_cast<quint64>(nRangeStartIndex),static_cast<quint64>(nBitCount - 1)));
    }

    return arrMissingRangeList;
}

QString ReliableTransferManager::BuildAvailableFilePath(const QString &strPreferredFilePath) const
{
    if(!QFileInfo::exists(strPreferredFilePath)){//如果该文件路径不存在，直接返回就行
        return strPreferredFilePath;
    }

    //如果存在，就必须特殊处理,找到一个不存在的，然后返回它
    QFileInfo oFileInfo(strPreferredFilePath);
    const QString strBaseName = oFileInfo.completeBaseName();
    const QString strSuffix = oFileInfo.completeSuffix();
    const QString strDirectory = oFileInfo.absolutePath();

    for(int nIndex = 1; nIndex < 10000; ++nIndex){
        const QString strCandidatePath = QDir(strDirectory).filePath(strSuffix.isEmpty() ? QStringLiteral("%1_%2").arg(strBaseName).arg(nIndex) :  QStringLiteral("%1_%2.%3").arg(strBaseName).arg(nIndex).arg(strSuffix));
        if(!QFileInfo::exists(strCandidatePath)){
            return strCandidatePath;
        }
    }

    return QString();//失败时返回空字符串。

}

QString ReliableTransferManager::EncodeBitArray(const QBitArray &oBitArray) const
{
    QByteArray arrBytes;// 创建空的字节数组，保存序列化结果
    QDataStream oDataStream(&arrBytes,QIODevice::WriteOnly);// 将数据流绑定到 arrBytes，以只写模式写入
    oDataStream << oBitArray;// 位图 → 二进制字节
//    QByteArray base64Bytes = arrBytes.toBase64(); // 得到 Base64 文本的字节
//    QString text = QString::fromLatin1(base64Bytes); // 转成 QString,按 Latin-1 编码
    return QString::fromLatin1(arrBytes.toBase64());// 字节 → Base64 字符串
}

QBitArray ReliableTransferManager::DecodeBitArray(const QString &strBase64, int nBitCount) const
{
    QByteArray arrBytes = QByteArray::fromBase64(strBase64.toLatin1());
    QDataStream oDataStream(&arrBytes,QIODevice::ReadOnly);
    QBitArray oBitArray;
    oDataStream >> oBitArray;
    oBitArray.resize(nBitCount);
    return oBitArray;
}


