#ifndef RELIABLESENDTASKSTATE_H
#define RELIABLESENDTASKSTATE_H

#include <QAtomicInt>
#include <QBitArray>
#include <QMutex>
#include <QString>
#include <QStringList>

/**
 * @brief SReliableSendTaskState
 * @details 保存一个文件发送任务的共享运行状态。
 *          协调线程和各数据发送线程通过共享指针访问同一个对象；
 *          位图、计数、错误和日志由互斥锁保护，
 *          取消标记通过原子操作访问。
 */
struct SReliableSendTaskState
{
    QMutex m_oMutex;//保护下面的位图、计数、错误和日志
    //true 表示已通过业务 ACK 或续传状态确认完成。
    //全文件确认位图：每一位对应一个全局分片序号；
    QBitArray m_oAckedPacketMap;
    int m_nConfirmedCount;//全任务已确认的不同分片数量，与确认位图保持一致。
    QString m_strFirstError;//保存第一个导致任务失败的原因，避免被后续取消信息覆盖。
    QStringList m_arrLogMessages;//数据线程追加日志，协调线程取走后统一发出日志信号。
    //共享取消标记：0 表示未请求取消，1 表示已请求取消；
    //不表示所有线程已经退出。
    QAtomicInt m_nCancelled;

    /**
     * @brief SReliableSendTaskState
     * @param nPacketCount 整个文件的分片总数。
     * @return 无
     * @details 初始化全文件确认位图及任务运行状态；
     *          初始分片均未确认，后续根据续传状态和 ACK 更新。
     */
    explicit SReliableSendTaskState(int nPacketCount)
        : m_oMutex(),
          m_oAckedPacketMap(nPacketCount, false),
          m_nConfirmedCount(0),
          m_strFirstError(),
          m_arrLogMessages(),
          m_nCancelled(0)
    {
    }

};

#endif // RELIABLESENDTASKSTATE_H
