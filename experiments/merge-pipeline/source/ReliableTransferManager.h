#ifndef RELIABLETRANSFERMANAGER_H
#define RELIABLETRANSFERMANAGER_H

#include "ReliableTransferTypes.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QSharedPointer>

class QThread;
class ReliableTcpServer;
class ReliableReceiveSessionStore;
class ReliableSenderWorker;


class ReliableTransferManager  : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief ReliableTransferManager
     * @param pParent Qt 父对象指针。
     * @return 无
     * @details 创建企业级 TCP 可靠传输管理器。
     */
    ReliableTransferManager(QObject *pParent = nullptr);

    /**
     * @brief ~ReliableTransferManager
     * @param 无
     * @return 无
     * @details 析构时关闭监听并回收后台发送线程。
     */
    ~ReliableTransferManager() override;

    /**
     * @brief SetOutputDirectory
     * @param strOutputDirectory 接收端输出目录。
     * @return 无
     * @details 设置可靠接收模式下的文件输出根目录。
     */
    void SetOutputDirectory(const QString &strOutputDirectory);

    /**
     * @brief ReliableTransferManager::StartListening
     * @param strBindIp 监听 IP；空字符串使用 0.0.0.0。
     * @param nPort 监听端口，范围为 1～65535。
     * @param strErrorMessage 输出启动失败的原因。
     * @return 监听成功返回 true，否则返回 false。
     * @details 校验监听参数，清理上一轮接收任务，
     *          创建本轮共享会话管理对象，然后启动监听。
     */
    bool StartListening(const QString &strBindIp, int nPort, QString &strErrorMessage);

    /**
     * @brief ReliableTransferManager::StopListening
     * @param 无
     * @return 无
     * @details 停止接受新连接，通知接收任务停止，
     *          等待全部接收线程退出后，清理本轮监听状态。
     */
    void StopListening();

    /**
     * @brief StartSend
     * @param strFilePath 源文件路径。
     * @param strTargetIp 目标 IP。
     * @param nPort 目标端口。
     * @param nThreadCount 数据发送线程数。
     * @param nWindowSize 每条数据连接的发送窗口大小。
     * @param nSegmentSize 分片大小，单位：字节。
     * @param strErrorMessage 输出启动失败的原因。
     * @return 任务成功启动返回 true，否则返回 false。
     * @details 一次管理一个文件发送任务，通过后台协调者组织多通道发送；
     *          返回 true 仅表示任务已启动，最终结果由 SignalSendFinished 通知。
     */
    bool StartSend(const QString &strFilePath,
                   const QString &strTargetIp,
                   int nPort,
                   int nThreadCount,
                   int nWindowSize,
                   int nSegmentSize,
                   QString &strErrorMessage);

    /**
     * @brief StopSending
     * @param 无
     * @return 无
     * @details 请求发送任务合作取消，等待发送线程退出后，
     *          释放线程对象并清空发送相关指针。
     */
    void StopSending();

signals:
    /**
     * @brief SignalLogMessage
     * @param strMessage 需要输出的日志消息。
     * @return 无
     * @details 通知界面追加传输日志，包括运行状态、重传提示和错误信息。
     */
    void SignalLogMessage(const QString &strMessage);

    /**
     * @brief SignalSendProgress
     * @param nCompletedPacketCount 已确认完成的分片数量。
     * @param nTotalPacketCount 当前文件的总分片数量。
     * @return 无
     * @details 通知界面更新发送进度；完成数量依据业务确认或续传状态统计，
     *          不以 socket 的本地写入字节数作为完成依据。
     */
    void SignalSendProgress(int nCompletedPacketCount, int nTotalPacketCount);

    /**
     * @brief SignalSendFinished
     * @param bSuccess 发送任务是否成功。
     * @param strMessage 任务结束时的结果描述或错误原因。
     * @return 无
     * @details 通知界面发送任务已结束，用于恢复按钮状态并显示结果；
     *          成功以接收端返回成功的 Result 为依据。
     */
    void SignalSendFinished(bool bSuccess, const QString &strMessage);

    /**
     * @brief SignalReceiveProgress
     * @param strFileName 当前正在接收的文件名。
     * @param nCompletedPacketCount 已接收并成功写入临时文件的不同分片数量。
     * @param nTotalPacketCount 当前文件的总分片数量。
     * @return 无
     * @details 通知界面更新指定文件的接收进度，重复分片不重复计入完成数量。
     */
    void SignalReceiveProgress(const QString &strFileName,
                               int nCompletedPacketCount,
                               int nTotalPacketCount);

    /**
     * @brief SignalReceiveFinished
     * @param strFilePath 接收完成后正式文件的完整路径。
     * @return 无
     * @details 在分片完整、最终哈希校验通过且正式文件生成成功后发出，
     *          通知界面显示文件接收完成及保存位置。
     */
    void SignalReceiveFinished(const QString &strFilePath);

private slots:
    /**
     * @brief ReliableTransferManager::OnIncomingConnection
     * @param nSocketDescriptor 已接受连接的原生套接字描述符。
     * @return 无
     * @details 检查当前是否允许接入，为连接创建接收线程和 Worker，
     *          由 Worker 在接收线程中创建 socket 并接管连接；
     *          连接结束后安排线程退出和对象释放。
     */
    void OnIncomingConnection(qintptr nSocketDescriptor);

private:
    QString m_strOutputDirectory; // 接收文件的输出根目录
    ReliableTcpServer *m_pTcpServer; // 接受连接并转交描述符的监听对象
    QSharedPointer<ReliableReceiveSessionStore> m_pSessionStore;//本次监听共享的会话管理对象，启动新一轮监听时重新创建。
    QList<QThread *> m_arrReceiverThreads;//当前管理的接收线程，包括数据连接线程和控制连接线程。
    int m_nNextConnectionIndex = 0;//给新连接的接收线程生成名称时使用，不是数据通道编号。
    quint64 m_nReceiverGeneration = 0;//区分不同轮次的监听，过滤停止或重启前遗留的排队通知。
    QHash<QUuid, int> m_mapReceiveProgress;//按任务 ID 保存已经转发的最新进度，避免多线程通知使进度倒退。
    QSet<QUuid> m_setCompletedSessions;//记录已通知完成的任务，避免重复完成请求触发重复界面通知。
    QThread *m_pSenderThread; // 后台发送线程，避免发送过程阻塞界面
    ReliableSenderWorker *m_pReliableSenderWorker;//发送工作对象，发送端迁移时再调整对应调度。
};

#endif // RELIABLETRANSFERMANAGER_H
