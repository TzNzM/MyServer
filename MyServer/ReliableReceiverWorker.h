#ifndef RELIABLERECEIVERWORKER_H
#define RELIABLERECEIVERWORKER_H

#include "ReliableReceiveSessionStore.h"
#include "ReliablePacketCodec.h"

#include <QByteArray>
#include <QObject>

class QTcpSocket;

class ReliableReceiverWorker : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief ReliableReceiverWorker
     * @param pStore 本次监听共享的会话管理对象。
     * @return 无
     * @details 一个 Worker 管理一条连接；
     *          构造时只保存 Store，socket 后续在接收线程中创建。
     */
    explicit ReliableReceiverWorker(const QSharedPointer<ReliableReceiveSessionStore> &pStore);
    /**
     * @brief ~ReliableReceiverWorker
     * @param 无
     * @return 无
     * @details 在所属接收线程中清理 socket，并解除会话关联。
     */
    ~ReliableReceiverWorker() override;

signals:
    /**
     * @brief SignalLogMessage
     * @param strMessage 日志内容。
     * @return 无
     * @details 将连接状态、接收错误等日志交给 Manager 转发。
     */
    void SignalLogMessage(const QString &strMessage);

    /**
     * @brief SignalWorkerFinished
     * @param 无
     * @return 无
     * @details 通知 Manager 当前连接的工作已经结束，
     *          由配套的线程管理逻辑安排退出和回收。
     */
    void SignalWorkerFinished();

    /**
     * @brief SignalReceiveProgress
     * @param oTransferId 文件传输任务 ID。
     * @param strFileName 当前文件名。
     * @param nCompletedPacketCount 已接收的不同分片数量。
     * @param nTotalPacketCount 文件总分片数量。
     * @return 无
     * @details 报告共享会话的接收进度；
     *          Manager 后续根据任务 ID 汇总并处理乱序通知。
     */
    void SignalReceiveProgress(
        const QUuid &oTransferId,
        const QString &strFileName,
        int nCompletedPacketCount,
        int nTotalPacketCount);

    /**
     * @brief SignalReceiveFinished
     * @param oTransferId 文件传输任务 ID。
     * @param strFilePath 正式文件的完整路径。
     * @return 无
     * @details 文件合并、校验和正式文件生成成功后发出，
     *          Manager 后续根据任务 ID 避免重复完成通知。
     */
    void SignalReceiveFinished(
        const QUuid &oTransferId,
        const QString &strFilePath);
public slots:
    /**
     * @brief Stop
     * @param 无
     * @return 无
     * @details 在所属接收线程中关闭连接、解除通道登记，
     *          释放本 Worker 持有的 socket。
     */
    void Stop();

    /**
     * @brief AcceptConnection
     * @param nSocketDescriptor 已接受连接的原生套接字描述符。
     * @return 无
     * @details 在所属接收线程中创建 QTcpSocket 并接管连接，
     *          关联数据可读和连接断开处理，并检查已有可读数据。
     *          一个 Worker 只接管一条连接。
     */
    void AcceptConnection(qintptr nSocketDescriptor);

private slots:
    /**
     * @brief OnSocketReadyRead
     * @param 无
     * @return 无
     * @details 读取当前连接的可读字节，追加到独立缓存，
     *          循环取出完整协议包并交给业务函数处理。
     */
    void OnSocketReadyRead();


    /**
     * @brief OnSocketDisconnected
     * @param 无
     * @return 无
     * @details 对端连接断开后解除会话关联，
     *          发出工作结束通知，由线程清理流程回收对象。
     */
    void OnSocketDisconnected();

private:
    struct SConnectionState
    {
        QByteArray m_arrReceiveBuffer;// 本连接尚未解析完的字节
        QSharedPointer<ReliableReceiveSession> m_pSession; // 本连接加入的共享文件会话
        int m_nLaneIndex; // -2 未握手，-1 控制连接，0..N-1 数据通道
        SConnectionState()
            :m_arrReceiveBuffer(),
              m_pSession(),
              m_nLaneIndex(-2)
        {

        }
    };

    /**
     * @brief HandlePacket
     * @param pSocket 当前线程拥有的连接。
     * @param stConnection 当前连接的缓存、会话和通道状态。
     * @param stHeader 已解析的协议包头。
     * @param arrPayload 当前完整协议包的载荷。
     * @param strErrorMessage 输出处理失败的原因。
     * @return 消息已按协议处理返回 true；
     *         校验失败或需要外层终止连接的处理错误返回 false。
     * @details Metadata 登记会话并回复 ResumeState；
     *          Data 调用 Session 接收分片，报告进度并回复 ACK；
     *          CompleteRequest 由控制连接请求合并校验并回复 Result。
     *          文件最终成功与否由 Result 的 success 表达，
     *          不等同于本函数的返回值。
     */
    bool HandlePacket(
        QTcpSocket *pSocket,
        SConnectionState &stConnection,
        const SReliablePacketHeader &stHeader,
        const QByteArray &arrPayload,
        QString &strErrorMessage);
    QTcpSocket *m_pSocket;// 由当前 Worker 在所属接收线程中创建和使用
    SConnectionState m_stConnection; // 当前这条连接的状态
    QSharedPointer<ReliableReceiveSessionStore> m_pStore; // 本次监听共享的会话管理对象
};

#endif // RELIABLERECEIVERWORKER_H
