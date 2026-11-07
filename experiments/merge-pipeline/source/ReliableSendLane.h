#ifndef RELIABLESENDLANE_H
#define RELIABLESENDLANE_H

#include "ReliableTransferTypes.h"
#include "ReliableSendTaskState.h"

#include <QSharedPointer>
#include <QThread>

class QFile;
class QTcpSocket;
class QElapsedTimer;

class ReliableSendLane : public QThread
{
public:
    /**
     * @brief ReliableSendLane
     * @param strFilePath 源文件路径。
     * @param strTargetIp 接收端 IP。
     * @param nPort 接收端端口。
     * @param stMetadata 整个文件任务的元信息。
     * @param nLaneIndex 当前数据通道编号，从 0 开始。
     * @param nFirstSequence 本通道负责的起始分片序号，包含。
     * @param nEndSequence 本通道负责的结束分片序号，不包含。
     * @param nWindowSize 当前数据连接的发送窗口。
     * @param pTaskState 全任务共享状态，必须非空。
     * @return 无
     * @details 保存通道配置和共享状态；
     *          文件句柄与 socket 后续在后台执行时创建。
     */
    ReliableSendLane(
        const QString &strFilePath,
        const QString &strTargetIp,
        int nPort,
        const SReliableFileMetadata &stMetadata,
        int nLaneIndex,
        quint64 nFirstSequence,
        quint64 nEndSequence,
        int nWindowSize,
        const QSharedPointer<SReliableSendTaskState> &pTaskState);
    /**
     * @brief WasSuccessful
     * @param 无
     * @return 当前通道负责的全部分片已确认完成返回 true。
     * @details 由协调线程在 wait() 成功返回后读取；
     *          通道成功不等于整个文件已通过最终校验。
     */
    bool WasSuccessful() const;
protected:
    /**
     * @brief run
     * @param 无
     * @return 无
     * @details 在线程启动后执行本通道的发送及重连流程，
     *          只处理分配给本通道的分片区间。
     */
    void run() override;

private:
    /**
     * @brief SendPartition
     * @param strErrorMessage 输出本轮发送失败的原因。
     * @return 本通道负责的全部分片确认完成返回 true，否则返回 false。
     * @details 建立本通道连接并握手，根据续传状态恢复本通道进度，
     *          按发送窗口发送分片，处理 ACK 和超时补传。
     *          断线后由外层 run 决定是否重新执行本函数。
     */
    bool SendPartition(QString &strErrorMessage);

    /**
     * @brief IsCancelled
     * @param 无
     * @return 整个任务或当前数据线程已请求取消返回 true。
     * @details 同时检查共享原子取消标记和本线程中断请求。
     */
    bool IsCancelled() const;

    /**
     * @brief SendDataPacket
     * @param oSocket 当前数据线程拥有的连接。
     * @param oFile 当前数据线程打开的源文件。
     * @param nSequence 要发送的全局分片序号。
     * @param stPending 输出本次发送记录，保留已有的补传次数。
     * @param oClock 当前通道用于统计超时的单调计时器。
     * @param strErrorMessage 输出失败原因。
     * @return 分片读取并写出成功返回 true，否则返回 false。
     * @details 根据序号定位文件偏移，读取一个分片并封装 DataPacket；
     *          写出后记录发送时间，业务确认由后续 ACK 处理完成。
     */
    bool SendDataPacket(
        QTcpSocket &oSocket,
        QFile &oFile,
        quint64 nSequence,
        SReliablePendingPacket &stPending,
        QElapsedTimer &oClock,
        QString &strErrorMessage);

    QString m_strFilePath; //源文件路径
    QString m_strTargetIp; //接收端 IP
    int m_nPort; //接收端端口
    SReliableFileMetadata m_stMetadata; //本通道保存的任务元信息副本
    int m_nLaneIndex; //当前数据通道编号
    quint64 m_nFirstSequence; //负责区间的起点，包含
    quint64 m_nEndSequence; //负责区间的终点，不包含
    int m_nWindowSize; //本数据连接的发送窗口
    QSharedPointer<SReliableSendTaskState> m_pTaskState; //全任务共享状态
    bool m_bSuccess; //当前通道是否确认完成

};

#endif // RELIABLESENDLANE_H
