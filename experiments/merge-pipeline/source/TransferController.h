#ifndef TRANSFERCONTROLLER_H
#define TRANSFERCONTROLLER_H

#include <QObject>
#include <QString>

class ReliableTransferManager;

class TransferController : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief TransferController
     * @param pParent Qt 父对象。
     * @return 无
     * @details 创建 Manager，并连接需要转发的信号。
     */
    explicit TransferController(QObject *pParent = nullptr);

    /**
    * @brief ~TransferController
    * @param 无
    * @return 无
    * @details Manager 作为子对象，由 Qt 自动释放；
    *          Manager 析构时负责停止并回收后台任务。
    */
    ~TransferController() override;

    /**
     * @brief SetOutputDirectory
     * @param strOutputDirectory 接收文件的输出根目录。
     * @return 无
     * @details 转交 Manager，新的目录配置用于后续启动的监听。
     */
    void SetOutputDirectory(const QString &strOutputDirectory);

    /**
     * @brief StartReceiver
     * @param strBindIp 监听 IP，空字符串使用默认监听地址。
     * @param nPort 监听端口。
     * @param strErrorMessage 输出启动失败原因。
     * @return 监听启动成功返回 true，否则返回 false。
     * @details 调用 Manager 启动接收监听。
     */
    bool StartReceiver(
        const QString &strBindIp,
        int nPort,
        QString &strErrorMessage);

    /**
     * @brief StopReceiver
     * @param 无
     * @return 无
     * @details 调用 Manager 停止监听并回收接收线程。
     */
    void StopReceiver();

    /**
     * @brief StartSend
     * @param strFilePath 源文件路径。
     * @param strTargetIp 接收端 IP。
     * @param nPort 接收端端口。
     * @param nThreadCount 请求的数据发送线程数。
     * @param nWindowSize 每条数据连接的发送窗口。
     * @param nSegmentSize 分片大小，单位：字节。
     * @param strErrorMessage 输出启动失败原因。
     * @return 发送任务已启动返回 true，否则返回 false。
     * @details 转交 Manager 启动后台任务；
     *          最终结果通过 SignalSendFinished 通知。
     */
    bool StartSend(
        const QString &strFilePath,
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
     * @details 调用 Manager 请求取消发送，并等待后台任务退出。
     */
    void StopSending();
signals:
    /**
     * @brief SignalLogMessage
     * @param strMessage 日志内容。
     * @return 无
     * @details 转发 Manager 的日志，供界面显示。
     */
    void SignalLogMessage(const QString &strMessage);

    /**
     * @brief SignalSendProgress
     * @param nCompletedPacketCount 已确认的不同分片数量。
     * @param nTotalPacketCount 整个文件的分片总数。
     * @return 无
     * @details 转发发送进度，不在 Controller 中重复统计。
     */
    void SignalSendProgress(
        int nCompletedPacketCount,
        int nTotalPacketCount);

    /**
     * @brief SignalSendFinished
     * @param bSuccess 文件发送是否成功。
     * @param strMessage 最终结果或错误原因。
     * @return 无
     * @details 转发任务结果，供界面恢复按钮并显示提示。
     */
    void SignalSendFinished(
        bool bSuccess,
        const QString &strMessage);

    /**
     * @brief SignalReceiveProgress
     * @param strFileName 正在接收的文件名。
     * @param nCompletedPacketCount 已接收的不同分片数量。
     * @param nTotalPacketCount 整个文件的分片总数。
     * @return 无
     * @details 转发接收进度，供界面更新状态。
     */
    void SignalReceiveProgress(
        const QString &strFileName,
        int nCompletedPacketCount,
        int nTotalPacketCount);

    /**
     * @brief SignalReceiveFinished
     * @param strFilePath 接收完成后的正式文件路径。
     * @return 无
     * @details 文件合并校验成功后，转发完成通知。
     */
    void SignalReceiveFinished(const QString &strFilePath);

private:
    //实际传输管理对象，以当前 Controller 为 Qt 父对象。
    ReliableTransferManager* m_pReliableTransferManager;

};

#endif // TRANSFERCONTROLLER_H
