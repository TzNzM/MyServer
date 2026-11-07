#include "ReliableTransferManager.h"

#include "ReliableReceiveSessionStore.h"
#include "ReliableReceiverWorker.h"
#include "ReliableTcpServer.h"
#include "ReliableSenderWorker.h"

#include <QFileInfo>
#include <QDir>
#include <QHostAddress>
#include <QPointer>
#include <QTcpSocket>
#include <QThread>

ReliableTransferManager::ReliableTransferManager(QObject *pParent)
    : QObject(pParent),
      m_strOutputDirectory(QDir::currentPath()),
      m_pTcpServer(new ReliableTcpServer(this)),
      m_pSessionStore(),
      m_arrReceiverThreads(),
      m_mapReceiveProgress(),
      m_setCompletedSessions(),
      m_pSenderThread(nullptr),
      m_pReliableSenderWorker(nullptr)
{
    //注册类型名称，供涉及该类型的跨线程队列传递使用。
     qRegisterMetaType<qintptr>("qintptr");

    //监听对象只负责交出连接描述符，
    //由 Manager 创建接收线程和 Worker。
    connect(m_pTcpServer,&ReliableTcpServer::SignalConnectionAccepted,this,&ReliableTransferManager::OnIncomingConnection);
}

ReliableTransferManager::~ReliableTransferManager()
{
    StopSending();
    StopListening();
}

void ReliableTransferManager::SetOutputDirectory(const QString &strOutputDirectory)
{
    if(!strOutputDirectory.isEmpty())
    {
        m_strOutputDirectory = QDir(strOutputDirectory).absolutePath();
    }
}

bool ReliableTransferManager::StartListening(const QString &strBindIp, int nPort, QString &strErrorMessage)
{
    //第一段：检查端口和监听地址。
    if (nPort < 1 || nPort > 65535)
    {
        strErrorMessage = QStringLiteral("监听端口必须在 1～65535 之间。");
        return false;
    }

    const QString strEffectiveBindIp =
        strBindIp.trimmed().isEmpty()
            ? QStringLiteral("0.0.0.0")
            : strBindIp.trimmed();

    QHostAddress oBindAddress;

    if(!oBindAddress.setAddress(strEffectiveBindIp))
    {
        strErrorMessage =
            QStringLiteral("监听 IP 非法：%1").arg(strBindIp);
        return false;
    }

    //第二段：停止上一轮监听，等待旧接收线程退出。
    StopListening();

    //第三段：为本轮监听创建新的共享会话管理对象。
    m_pSessionStore.reset(new ReliableReceiveSessionStore(m_strOutputDirectory));

    //第四段：启动监听；失败时清理刚创建的本轮状态。
    if(!m_pTcpServer->listen(oBindAddress,static_cast<quint16>(nPort)))
    {
        strErrorMessage = m_pTcpServer->errorString();

        StopListening();
        return false;
    }

    emit SignalLogMessage(
            QStringLiteral("[TCP-Reliable] 已开始监听 %1:%2")
                .arg(oBindAddress.toString())
                .arg(nPort));

    return true;

}

void ReliableTransferManager::StopListening()
{
    //第一段：使上一轮监听产生的排队进度、完成通知失效。
    ++m_nReceiverGeneration;

    //关闭监听端口，停止接受新连接。
    //已经建立的连接由各接收线程中的 Worker 清理。
    m_pTcpServer->close();

    //第二段：通过共享停止标记，通知接收和合并过程退出。
    if (!m_pSessionStore.isNull())
    {
        m_pSessionStore->RequestStop();
    }

    //第三段：先向所有接收线程请求退出事件循环。
    for(QThread* pThread : m_arrReceiverThreads)
    {
        pThread->quit();
    }

    //再逐个等待线程结束，最后释放 QThread 对象。
    for (QThread *pThread : m_arrReceiverThreads)
    {
        pThread->wait();
        delete pThread;
    }

    //第四段：线程全部退出后，清理本轮监听记录。
    m_arrReceiverThreads.clear();
    m_pSessionStore.clear();

    m_nNextConnectionIndex = 0;
    m_mapReceiveProgress.clear();
    m_setCompletedSessions.clear();

}

bool ReliableTransferManager::StartSend(const QString &strFilePath, const QString &strTargetIp, int nPort, int nThreadCount, int nWindowSize, int nSegmentSize, QString &strErrorMessage)
{
    strErrorMessage.clear();

    //第一段：一个 Manager 同时只管理一个文件发送任务。
    if(m_pSenderThread != nullptr && m_pSenderThread->isRunning())
    {
        strErrorMessage = QStringLiteral("当前已有文件发送任务。");
        return false;
    }

    //第二段：进行启动前的基本检查。
    //文件读取、SHA-256 计算等耗时操作仍由后台 Worker 执行。
    const QFileInfo oFileInfo(strFilePath);
    QHostAddress oTargetAddress;

    if (!oFileInfo.isFile() ||
        !oTargetAddress.setAddress(strTargetIp.trimmed()) ||
        nPort < 1 || nPort > 65535 ||
        nThreadCount < 1 ||
        nThreadCount > g_nReliableMaxThreadCount ||
        nWindowSize < 1 ||
        nWindowSize > g_nReliableMaxWindowSize ||
        nSegmentSize <= 0 ||
        nSegmentSize > g_nReliableMaxSegmentSize)
    {
        strErrorMessage =
            QStringLiteral(
                "源文件、目标地址、端口、线程数、发送窗口或分片大小非法。");
        return false;
    }

    //第三段：回收上一任务结束后保留的协调线程对象。
    StopSending();

    m_pSenderThread = new QThread(this);
    m_pSenderThread->setObjectName(QStringLiteral("sender-coordinator"));

    m_pReliableSenderWorker = new ReliableSenderWorker;
    ReliableSenderWorker *pWorker = m_pReliableSenderWorker;

    pWorker->moveToThread(m_pSenderThread);

    //第四段：转发 Worker 的日志、进度和最终结果。
    connect(
            pWorker, &ReliableSenderWorker::SignalLogMessage,
            this, &ReliableTransferManager::SignalLogMessage);
    connect(
            pWorker, &ReliableSenderWorker::SignalSendProgress,
            this, &ReliableTransferManager::SignalSendProgress);
    connect(
            pWorker, &ReliableSenderWorker::SignalSendFinished,
            this, &ReliableTransferManager::SignalSendFinished);

    //第五段：任务结束后，请求协调线程退出。
    //quit() 是线程安全的，直接调用可避免退出请求等待主线程处理。
    connect(
        pWorker, &ReliableSenderWorker::SignalSendFinished,
        m_pSenderThread, &QThread::quit,
        Qt::DirectConnection);

    //线程结束时，在所属线程中安排释放 Worker。
    connect(
            m_pSenderThread, &QThread::finished,
            pWorker, &QObject::deleteLater);

    //第六段：在线程启动后，由后台 Worker 执行发送流程。
    //按值捕获参数，避免 StartSend 返回后引用失效。
    const QString strAbsoluteFilePath = oFileInfo.absoluteFilePath();
    const QString strEffectiveTargetIp = oTargetAddress.toString();

    connect(m_pSenderThread,&QThread::started,pWorker,[pWorker,
            strAbsoluteFilePath,
            strEffectiveTargetIp,
            nPort,
            nThreadCount,
            nWindowSize,
            nSegmentSize]()
    {
        pWorker->ProcessSend(strAbsoluteFilePath,
                             strEffectiveTargetIp,
                             nPort,
                             nThreadCount,
                             nWindowSize,
                             nSegmentSize);
    });

    m_pSenderThread->start();

    //仅表示已发起后台任务，最终结果通过信号通知。
    return true;
}


void ReliableTransferManager::StopSending()
{
    //第一段：尚未创建发送线程，无需清理。
    if (m_pSenderThread == nullptr)
    {
        return;
    }

    //第二段：设置中断请求，供发送代码主动检查并退出。
    //发出“请停止”的请求，发送代码主动检查后退出
    m_pSenderThread->requestInterruption();

    //请求发送线程的事件循环退出
    m_pSenderThread->quit();

    //第三段：等待发送工作及线程清理结束，再释放线程对象。
    m_pSenderThread->wait();

    delete m_pSenderThread;
    m_pSenderThread = nullptr;
    m_pReliableSenderWorker = nullptr;
}

void ReliableTransferManager::OnIncomingConnection(qintptr nSocketDescriptor)
{
    //第一段：检查本轮监听状态及接收线程总数。
    if (m_pSessionStore.isNull() ||
            m_pSessionStore->IsStopping() ||
            m_arrReceiverThreads.size() >= g_nReliableMaxReceiverThreadCount)
    {
        //本连接不再交给 Worker，临时接管后立即关闭。
        QTcpSocket oSocket;
        oSocket.setSocketDescriptor(nSocketDescriptor);
        oSocket.abort();
        return;
    }

    //保存建立连接时的监听轮次，供后续过滤旧通知。
    const quint64 nGeneration = m_nReceiverGeneration;

    //第二段：创建接收线程及其工作对象。
    QThread *pThread = new QThread(this);

    pThread->setObjectName(QStringLiteral("receiver-connection-%1").arg(m_nNextConnectionIndex++));
    ReliableReceiverWorker *pWorker = new ReliableReceiverWorker(m_pSessionStore);

    //将 Worker 的线程归属改为新接收线程。
    pWorker->moveToThread(pThread);

    //第三段：线程启动后，由 Worker 在接收线程中接管连接。
    connect(pThread,&QThread::started,pWorker,[pWorker,nSocketDescriptor](){pWorker->AcceptConnection(nSocketDescriptor);});
    //线程结束时，在接收线程中安排释放 Worker。
    connect(pThread,&QThread::finished,pWorker,&QObject::deleteLater);
    //连接工作结束时，请求配套线程退出。 //quit() 是线程安全的，直接调用可避免退出请求排在主线程队列中。
    connect(pWorker,&ReliableReceiverWorker::SignalWorkerFinished,pThread,&QThread::quit,Qt::DirectConnection);
    //第四段：将 Worker 的日志转交给 Manager。
    connect(pWorker,&ReliableReceiverWorker::SignalLogMessage,this,&ReliableTransferManager::SignalLogMessage);

    //转发接收进度，过滤旧轮次通知，并避免进度倒退。
    connect(pWorker,&ReliableReceiverWorker::SignalReceiveProgress,this,
            [this,nGeneration](const QUuid &oTransferId, const QString &strFileName,int nCompletedPacketCount,int nTotalPacketCount)
    {
        //停止监听后，不再处理上一轮的进度通知。
        //已经报告完成的任务，也不再更新进度。
        if (nGeneration != m_nReceiverGeneration ||
            m_setCompletedSessions.contains(oTransferId))
        {
            return;
        }

        int &nLatestCompletedCount = m_mapReceiveProgress[oTransferId];
        nLatestCompletedCount = qMax(nLatestCompletedCount,nCompletedPacketCount);
        emit SignalReceiveProgress(strFileName,nLatestCompletedCount,nTotalPacketCount);
    });

    //转发接收完成通知，同一个任务只向外通知一次。
    connect(pWorker,&ReliableReceiverWorker::SignalReceiveFinished,this,[this,nGeneration]
            (const QUuid &oTransferId,const QString &strFilePath)
    {
        if (nGeneration != m_nReceiverGeneration ||
            m_setCompletedSessions.contains(oTransferId))
        {
            return;
        }

        //先登记完成，再向外通知。
        m_setCompletedSessions.insert(oTransferId);

        //任务已完成，不再需要保存它的进度记录。
        m_mapReceiveProgress.remove(oTransferId);

        emit SignalReceiveFinished(strFilePath);

    });



    //第五段：线程自然结束时，从管理列表移除并安排释放。
    const QPointer<QThread> pGuard(pThread);

    connect(pThread,&QThread::finished,this,[this,pGuard]()
    {
       //StopListening() 可能已经等待并删除了这个线程对象。
        if(pGuard.isNull())
       {
           return;
       }
        m_arrReceiverThreads.removeAll(pGuard.data());
        pGuard->deleteLater();
    });

    //全部连接建立好后，再登记并启动线程。
    m_arrReceiverThreads.append(pThread);
    pThread->start();
}

