#include "ReliableMergeHasher.h"
#include "ReliableSha256.h"

#include <QElapsedTimer>
#include <QMutexLocker>

ReliableMergeHasher::ReliableMergeHasher(const QSharedPointer<QAtomicInt> &pStopFlag)
    : m_pStopFlag(pStopFlag)
{
    setObjectName(QStringLiteral("receiver-merge-sha256"));
    for (int nIndex = 0; nIndex < static_cast<int>(m_arrBuffers.size()); ++nIndex)
    {
        m_arrBuffers[nIndex].m_arrData.resize(4 * 1024 * 1024);
        m_arrFreeBuffers.enqueue(nIndex);
    }
    start();
}

ReliableMergeHasher::~ReliableMergeHasher()
{
    CancelAndWait();
}

bool ReliableMergeHasher::CheckStoppedLocked(QString &strErrorMessage)
{
    if (!m_strErrorMessage.isEmpty())
        strErrorMessage = m_strErrorMessage;
    else if (m_bCancelled || m_pStopFlag->loadAcquire())
        strErrorMessage = QStringLiteral("接收端正在停止，取消合并，保留通道文件。");
    else
        return false;
    return true;
}

bool ReliableMergeHasher::AcquireBuffer(int &nBufferIndex, QString &strErrorMessage)
{
    QMutexLocker oLocker(&m_oMutex);
    while (true)
    {
        if (CheckStoppedLocked(strErrorMessage))
            return false;
        if (!m_arrFreeBuffers.isEmpty())
        {
            nBufferIndex = m_arrFreeBuffers.dequeue();
            return true;
        }
        m_oChanged.wait(&m_oMutex, 40);
    }
}

QByteArray &ReliableMergeHasher::GetBuffer(int nBufferIndex)
{
    return m_arrBuffers[nBufferIndex].m_arrData;
}

bool ReliableMergeHasher::SubmitBuffer(int nBufferIndex, int nDataSize, QString &strErrorMessage)
{
    QMutexLocker oLocker(&m_oMutex);
    if (CheckStoppedLocked(strErrorMessage))
        return false;
    m_arrBuffers[nBufferIndex].m_nDataSize = nDataSize;
    m_arrReadyBuffers.enqueue(nBufferIndex);
    m_oChanged.wakeAll();
    return true;
}

void ReliableMergeHasher::SetError(const QString &strErrorMessage)
{
    QMutexLocker oLocker(&m_oMutex);
    m_strErrorMessage = strErrorMessage;
    m_oChanged.wakeAll();
}

bool ReliableMergeHasher::Finish(QByteArray &arrDigest, QString &strErrorMessage)
{
    {
        QMutexLocker oLocker(&m_oMutex);
        m_bInputFinished = true;
        m_oChanged.wakeAll();
    }
    //工作线程自身在条件等待和每批摘要之间检查停止标记。
    wait();
    QMutexLocker oLocker(&m_oMutex);
    if (CheckStoppedLocked(strErrorMessage))
        return false;
    arrDigest = m_arrDigest;
    return true;
}

void ReliableMergeHasher::CancelAndWait()
{
    {
        QMutexLocker oLocker(&m_oMutex);
        m_bCancelled = true;
        m_oChanged.wakeAll();
    }
    wait();
}

qint64 ReliableMergeHasher::GetHashTimeNs() const
{
    return m_nHashTimeNs;
}

qint64 ReliableMergeHasher::GetHashedByteCount() const
{
    return m_nHashedByteCount;
}

void ReliableMergeHasher::run()
{
    ReliableSha256 oSha;
    QString strErrorMessage;
    QElapsedTimer oClock;
    oClock.start();
    const bool bInitialized = oSha.Initialize(strErrorMessage);
    m_nHashTimeNs += oClock.nsecsElapsed();
    if (!bInitialized)
    {
        SetError(strErrorMessage);
        return;
    }
    while (true)
    {
        int nBufferIndex = -1;
        {
            QMutexLocker oLocker(&m_oMutex);
            while (m_arrReadyBuffers.isEmpty() && !m_bInputFinished)
            {
                if (CheckStoppedLocked(strErrorMessage))
                    return;
                m_oChanged.wait(&m_oMutex, 40);
            }
            if (CheckStoppedLocked(strErrorMessage))
                return;
            if (m_arrReadyBuffers.isEmpty())
                break; //输入结束后，必须先处理完已提交的所有数据。
            nBufferIndex = m_arrReadyBuffers.dequeue();
        }
        const SBuffer &stBuffer = m_arrBuffers[nBufferIndex];
        oClock.restart();
        const bool bAdded = oSha.AddData(stBuffer.m_arrData.constData(), stBuffer.m_nDataSize, strErrorMessage);
        m_nHashTimeNs += oClock.nsecsElapsed();
        if (!bAdded)
        {
            SetError(strErrorMessage);
            return;
        }
        m_nHashedByteCount += stBuffer.m_nDataSize;
        {
            QMutexLocker oLocker(&m_oMutex);
            m_arrFreeBuffers.enqueue(nBufferIndex);
            m_oChanged.wakeAll();
        }
    }
    oClock.restart();
    const bool bFinished = oSha.Finish(m_arrDigest, strErrorMessage);
    m_nHashTimeNs += oClock.nsecsElapsed();
    if (!bFinished)
        SetError(strErrorMessage);
}
