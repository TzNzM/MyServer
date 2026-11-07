#include "ReliableReceiveSessionStore.h"

#include <QDir>
#include <QMutexLocker>
#include <QDebug>

//构造函数创建的停止标记初始为 0。后面创建每个 Session 时，都传入同一个 m_pStopFlag
ReliableReceiveSessionStore::ReliableReceiveSessionStore(const QString &strOutputDirectory)
    :m_strOutputDirectory(QDir(strOutputDirectory).absolutePath()),m_pStopFlag(new QAtomicInt(0))
{

}

//这里相当于是在如何申请连接通道
QSharedPointer<ReliableReceiveSession> ReliableReceiveSessionStore::AttachSession(const SReliableFileMetadata &stMetadata, int nLaneIndex, QString &strErrorMessage)
{
    strErrorMessage.clear();
    //保护两个会话登记表，以及每个会话的通道占用记录。
    QMutexLocker oLocker(&m_oMutex);

    if (IsStopping())
    {//如果有其他线程修改终止，就停止
        strErrorMessage = QStringLiteral("接收任务正在停止。");
        return {};
    }

    //第一段：根据当前传输 ID 查找已经登记的会话。
    auto oEntry = m_mapSessions.find(stMetadata.m_oTransferId);

    //这个是find找不到时的判断，如果等于m_mapSessions.end()，就表明当前传输ID还未登记
    if(oEntry != m_mapSessions.end())
    {
        const SReliableFileMetadata stExistingMetadata = oEntry->m_pSession->GetMetadata();//通过共享指针调用类对象

        //同一个任务的连接，必须携带相同的续传身份。
        if (stExistingMetadata.m_strSessionKey !=
            stMetadata.m_strSessionKey)
        {
            strErrorMessage =
                QStringLiteral("同一传输 ID 的文件信息发生变化。");
            return {};
        }

        //同一个通道不能同时由两条连接占用。
        if (oEntry->m_setActiveLanes.contains(nLaneIndex))
        {
            strErrorMessage =
                QStringLiteral("当前通道编号已被另一条连接占用。");
            return {};
        }
        //如果该通道还没被占用，就再加
        oEntry->m_setActiveLanes.insert(nLaneIndex);

        //返回同一个 Session 的共享指针，不复制 Session 对象。
        return oEntry->m_pSession;
    }

     //第二段：当前 UUID 尚未登记，检查续传目录是否被其他任务占用。
    if (m_mapSessionKeys.contains(stMetadata.m_strSessionKey))
    {
        strErrorMessage =
            QStringLiteral("另一个任务正在占用相同的续传文件。");
        return {};
    }

    //第三段：创建会话对象，并传入 Store 共享的停止标记。
    QSharedPointer<ReliableReceiveSession> pSession(
                new ReliableReceiveSession(stMetadata,m_strOutputDirectory,m_pStopFlag));

    //创建首次接收状态，或从磁盘恢复历史状态
    if(!pSession->LoadOrCreateSession(strErrorMessage))
    {
        return {};
    }

    //第四段：初始化成功后，再正式加入登记表。
    SSessionEntry stEntry;
    stEntry.m_pSession = pSession;
    stEntry.m_setActiveLanes.insert(nLaneIndex);

    m_mapSessions.insert(stMetadata.m_oTransferId,stEntry);
    m_mapSessionKeys.insert(stMetadata.m_strSessionKey,stMetadata.m_oTransferId);

    return pSession;
}

void ReliableReceiveSessionStore::DetachSession(const QSharedPointer<ReliableReceiveSession> &pSession, int nLaneIndex)
{
    //连接可能尚未成功加入任何会话。
    if (pSession.isNull())
    {
        return;
    }

    QMutexLocker oLocker(&m_oMutex);

    const SReliableFileMetadata stMetadata = pSession->GetMetadata();

    auto oEntry = m_mapSessions.find(stMetadata.m_oTransferId);

    //会话可能已经解除登记。
    if (oEntry == m_mapSessions.end())
    {
        return;
    }

    //只处理当前登记的这个会话对象，避免旧对象影响新的登记。
    if (oEntry->m_pSession != pSession)
    {
        return;
    }

    //移除当前连接占用的通道编号。
    //如果没有这个编号，说明没有需要解除的登记。
    if (!oEntry->m_setActiveLanes.remove(nLaneIndex))
    {
        return;
    }

    //还有其他连接使用会话，仅归还当前通道，不移除整个会话。
    if (!oEntry->m_setActiveLanes.isEmpty())
    {
        return;
    }

    //最后一条连接退出，尝试保存剩余的接收记录。
    QString strErrorMessage;

    if(!pSession->PersistSessionState(strErrorMessage))
    {//把当前的状态给最后持久化
        qWarning().noquote()
                    << QStringLiteral("[TCP-Reliable] 会话退出时保存状态失败：%1")
                           .arg(strErrorMessage);
    }

    //解除续传目录的占用，允许后续任务恢复这份历史数据。
    m_mapSessionKeys.remove(stMetadata.m_strSessionKey);
    //移除当前传输 ID 对应的会话登记。
    m_mapSessions.erase(oEntry);

}

void ReliableReceiveSessionStore::RequestStop()
{
    m_pStopFlag->storeRelease(1);
}

bool ReliableReceiveSessionStore::IsStopping() const
{
    return m_pStopFlag->loadAcquire() != 0;
}
