#ifndef RELIABLESENDERWORKER_H
#define RELIABLESENDERWORKER_H

#include "ReliableTransferTypes.h"
#include "ReliableSendTaskState.h"

#include <QSharedPointer>
#include <QThread>
#include <limits>
#include <QBitArray>
#include <QFile>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>

class QTcpSocket;

class ReliableSenderWorker : public QObject
{
    Q_OBJECT

public:

    explicit ReliableSenderWorker(QObject *pParent = nullptr);


signals:
    /**
     * @brief SignalLogMessage
     * @param strMessage 需要输出的日志消息。
     * @return 无
     * @details 通知接收对象输出发送过程中的运行状态、重传提示或错误信息。
     */
    void SignalLogMessage(const QString &strMessage);

    /**
     * @brief SignalSendProgress
     * @param nCompletedPacketCount 已确认完成的分片数量。
     * @param nTotalPacketCount 当前文件的总分片数量。
     * @return 无
     * @details 通知接收对象更新发送进度；完成数量依据业务 ACK 或接收端返回的续传状态统计。
     */
    void SignalSendProgress(int nCompletedPacketCount, int nTotalPacketCount);

    /**
     * @brief SignalSendFinished
     * @param bSuccess 发送任务是否成功。
     * @param strMessage 任务结束时的结果描述或错误原因。
     * @return 无
     * @details 通知接收对象发送任务已结束，用于显示结果和触发线程退出等后续处理；
     *          成功以接收端返回成功的 Result 为依据。
     */
    void SignalSendFinished(bool bSuccess, const QString &strMessage);

public slots:
    /**
     * @brief ProcessSend
     * @param strFilePath 待发送源文件的路径。
     * @param strTargetIp 接收端的 IP 地址。
     * @param nPort 接收端监听端口，范围为 1～65535。
     * @param nThreadCount 请求的数据发送线程数，范围为 1～32；
     *                     实际数量不超过文件分片总数，不包含协调线程。
     * @param nWindowSize 每条数据连接允许同时存在的未确认分片数量，
     *                    范围为 1～128，不是发送线程数。
     * @param nSegmentSize 请求的分片大小，单位：字节；
     *                     实际分片大小至少为 8 KiB，最大为 4 MiB。
     * @return 无；任务最终结果通过 SignalSendFinished 通知。
     * @details 在后台协调线程中组织一个文件的完整发送流程：
     *          1. 调用 BuildMetadata，计算一次文件 SHA-256，
     *             确定分片数量、实际通道数及任务身份。
     *          2. 建立控制连接，通过元信息握手获取接收端的续传状态。
     *          3. 为各数据通道分配连续的分片区间，启动对应的发送线程，
     *             由各通道处理数据发送、业务 ACK、超时重传和断线恢复。
     *          4. 汇总各通道的日志及全任务已确认分片数，
     *             通过 SignalLogMessage 和 SignalSendProgress 向外通知。
     *          5. 数据通道完成后，通过控制连接请求接收端合并并校验文件；
     *             只有收到成功的 Result，才报告整个发送任务成功。
     *          6. 执行期间检查取消请求，结束前等待并回收各数据发送线程。
     *          7. 控制连接写出或读取失败、最终检查仍有缺片时，有限轮次重新握手恢复；
     *             元信息和 UUID 保持不变，每轮开始前旧数据线程均已退出并回收。
     *             数据通道最终失败引发的共享取消不在下一轮清零。
     */
    void ProcessSend(const QString &strFilePath,
                     const QString &strTargetIp,
                     int nPort,
                     int nThreadCount,
                     int nWindowSize,
                     int nSegmentSize);
private:
    /**
     * @brief BuildMetadata
     * @param strFilePath 源文件路径。
     * @param nSegmentSize 请求的分片大小，单位：字节。
     * @param nThreadCount 请求的数据发送线程数。
     * @param stMetadata 输出的文件元信息。
     * @param strErrorMessage 输出失败原因。
     * @return 成功返回 true，否则返回 false。
     * @details 计算分片总数和实际通道数，增量计算 SHA-256，
     *          生成稳定的续传会话键及本次任务独立的 UUID。
     */
    bool BuildMetadata(const QString &strFilePath,
                       int nSegmentSize,
                       int nThreadCount,
                       SReliableFileMetadata &stMetadata,
                       QString &strErrorMessage);

    /**
     * @brief EmitTaskSnapshot
     * @param pTaskState 当前文件任务的共享状态，必须非空。
     * @param nTotalPacketCount 整个文件的分片总数。
     * @param nLastCompletedCount 上次已报告的完成数量，报告后更新。
     * @return 无
     * @details 由协调线程调用，锁内读取进度并取走日志，
     *          锁外发出日志信号；完成数量变化时发出进度信号。
     */
    void EmitTaskSnapshot(
        const QSharedPointer<SReliableSendTaskState> &pTaskState,
        int nTotalPacketCount,
        int &nLastCompletedCount);

};


#endif // RELIABLESENDERWORKER_H
