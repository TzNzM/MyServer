#ifndef RELIABLERECEIVESESSIONSTORE_H
#define RELIABLERECEIVESESSIONSTORE_H

#include "ReliableReceiveSession.h"

#include <QHash>
#include <QSet>

class ReliableReceiveSessionStore
{
public:
    /**
     * @brief ReliableReceiveSessionStore
     * @param strOutputDirectory 本次监听使用的接收输出目录。
     * @return 无
     * @details 初始化会话登记表，并创建所有会话共享的停止标记。
     *          每次重新启动监听时创建新的 Store。
     */
    explicit ReliableReceiveSessionStore(
        const QString &strOutputDirectory);

    /**
     * @brief AttachSession
     * @param stMetadata 当前连接携带的文件元信息。
     * @param nLaneIndex 当前连接的通道编号；-1 表示控制连接。
     * @param strErrorMessage 输出创建或加入会话失败的原因。
     * @return 已准备好的共享会话；失败返回空指针。
     * @details 同一传输任务共享一个会话，
     *          同一通道编号不能同时被两条连接占用。
     */
    QSharedPointer<ReliableReceiveSession> AttachSession(
        const SReliableFileMetadata &stMetadata,
        int nLaneIndex,
        QString &strErrorMessage);

    /**
     * @brief DetachSession
     * @param pSession 当前连接关联的会话。
     * @param nLaneIndex 当前连接占用的通道编号。
     * @return 无
     * @details 连接结束时解除通道登记；
     *          最后一条连接离开时尝试保存状态并移除登记。
     */
    void DetachSession(
        const QSharedPointer<ReliableReceiveSession> &pSession,
        int nLaneIndex);

    /**
     * @brief RequestStop
     * @param 无
     * @return 无
     * @details 设置共享停止标记，通知耗时合并过程主动退出。
     */
    void RequestStop();

    /**
     * @brief IsStopping
     * @param 无
     * @return 已请求停止返回 true，否则返回 false。
     * @details 原子读取停止标记，不获取会话登记表的互斥锁。
     */
    bool IsStopping() const;

private:
    struct SSessionEntry
    {
        //这个文件对应的共享接收会话。
        QSharedPointer<ReliableReceiveSession> m_pSession;

        //当前已占用的通道编号。这里用set的好处就是只需要知道当前通道编号是否被占用
        //-1 为控制连接，0..N-1 为数据连接。
        QSet<int> m_setActiveLanes;
    };

    QMutex m_oMutex; // 保护会话登记表和通道占用记录

    QHash<QUuid, SSessionEntry> m_mapSessions; // 传输 ID → 会话登记
    QHash<QString, QUuid> m_mapSessionKeys;   // 续传会话键 → 当前占用它的传输 ID

    QString m_strOutputDirectory; // 本次监听固定的输出目录
    QSharedPointer<QAtomicInt> m_pStopFlag; // 所有受管理会话共享的停止标记
};

#endif // RELIABLERECEIVESESSIONSTORE_H
