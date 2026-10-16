#ifndef RELIABLETRANSFERMANAGER_H
#define RELIABLETRANSFERMANAGER_H

#include "ReliablePacketCodec.h"

#include <QBitArray>
#include <QHash>
#include <QObject>

class QTcpServer;
class QTcpSocket;
class QThread;
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
     * @brief StartListening
     * @param strBindIp 监听 IP。
     * @param nPort 监听端口。
     * @param strErrorMessage 输出错误信息。
     * @return 成功返回 true，否则返回 false。
     * @details 启动可靠 TCP 接收端并保持长连接监听。
     */
    bool StartListening(const QString &strBindIp, int nPort, QString &strErrorMessage);

    /**
     * @brief StopListening
     * @param 无
     * @return 无
     * @details 停止可靠 TCP 接收端监听。
     */
    void StopListening();

    /**
     * @brief StartSend
     * @param strFilePath 源文件路径。
     * @param strTargetIp 目标 IP。
     * @param nPort 目标端口。
     * @param nWindowSize 发送窗口大小。
     * @param nSegmentSize 分片大小。
     * @param strErrorMessage 输出错误信息。
     * @return 成功返回 true，否则返回 false。
     * @details 启动后台长连接可靠发送会话。
     */
    bool StartSend(const QString &strFilePath,
                   const QString &strTargetIp,
                   int nPort,
                   int nWindowSize,
                   int nSegmentSize,
                   QString &strErrorMessage);

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
     * @brief OnNewConnection
     * @param 无
     * @return 无
     * @details 接收新的可靠 TCP 长连接。
     */
    void OnNewConnection();

    /**
     * @brief OnSocketReadyRead
     * @param 无
     * @return 无
     * @details 解析长连接上的控制包和数据包。
     */
    void OnSocketReadyRead();

    /**
     * @brief OnSocketDisconnected
     * @param 无
     * @return 无
     * @details 在连接断开时把会话状态持久化。(这里是对socket的断开)
     */
    void OnSocketDisconnected();

private:
    struct SReceiverSessionState
    {
        SReliableFileMetadata m_stMetadata; // 文件元信息：文件名、大小、分片规格、哈希、传输 ID 等

        QString m_strSessionDirectory; // 保存本次文件传输临时数据和状态的目录
        QString m_strPartFilePath;     // 临时文件 payload.part 的完整路径，接收到的分片写入这里
        QString m_strStateFilePath;    // 状态文件 state.json 的完整路径，用于保存断点续传信息
        QString m_strFinalFilePath;    // 接收完成并校验成功后，正式文件的完整路径

        QBitArray m_oReceivedPacketMap; // 接收位图：每一位对应一个分片，true 表示已收到
        quint32 m_nReceivedPacketCount;// 已接收的不同分片数量，用于进度统计
        int m_nUnsavedPacketCount;     // 新收到但其接收记录尚未保存到状态文件的分片数量
        bool m_bCompleted;             // 是否已完成最终校验和正式文件生成
        SReceiverSessionState()
            : m_stMetadata(),
              m_strSessionDirectory(),
              m_strPartFilePath(),
              m_strStateFilePath(),
              m_strFinalFilePath(),
              m_oReceivedPacketMap(),
              m_nReceivedPacketCount(0),
              m_nUnsavedPacketCount(0),
              m_bCompleted(false)
        {
        }
    };

    /**
     * @brief HandleMetadataPacket
     * @param pSocket 当前连接。
     * @param stPacketHeader 包头。
     * @param arrPayload 元信息负载。
     * @return 无
     * @details 初始化或恢复接收会话，并返回缺失分片范围。
     */
    void HandleMetadataPacket(QTcpSocket *pSocket,
                              const SReliablePacketHeader &stPacketHeader,
                              const QByteArray &arrPayload);

    /**
     * @brief HandleDataPacket
     * @param pSocket 当前连接。
     * @param stPacketHeader 包头。
     * @param arrPayload 数据分片。
     * @return 无
     * @details 接收一个可靠分片并立刻返回 ACK。
     */
    void HandleDataPacket(QTcpSocket *pSocket,
                          const SReliablePacketHeader &stPacketHeader,
                          const QByteArray &arrPayload);

    /**
     * @brief HandleCompleteRequestPacket
     * @param pSocket 当前连接。
     * @param stPacketHeader 包头。
     * @return 无
     * @details 接收发送端完成请求后执行缺口检查和哈希校验。
     */
    void HandleCompleteRequestPacket(QTcpSocket *pSocket,
                                     const SReliablePacketHeader &stPacketHeader);

    /**
     * @brief VerifyAndFinalizeSession
     * @param stSessionState 当前会话。
     * @param strErrorMessage 输出错误信息。
     * @return 成功返回 true，否则返回 false。
     * @details 对完整接收的数据执行 MD5 / SHA256 校验并产出最终文件。
     */
    bool VerifyAndFinalizeSession(SReceiverSessionState &stSessionState, QString &strErrorMessage);


    /**
     * @brief LoadOrCreateSession
     * @param stMetadata 文件元信息。
     * @param stSessionState 输出会话状态。
     * @param strErrorMessage 输出错误信息。
     * @return 成功返回 true，否则返回 false。
     * @details 按会话键加载断点信息或首次创建接收会话。
     */
    bool LoadOrCreateSession(const SReliableFileMetadata &stMetadata,
                             SReceiverSessionState &stSessionState,
                             QString &strErrorMessage);

    /**
     * @brief PersistSessionState
     * @param stSessionState 需要持久化的会话状态。
     * @param strErrorMessage 输出错误信息。
     * @return 成功返回 true，否则返回 false。
     * @details 把接收位图和元信息写入状态文件，以支持断点续传。
     */
    bool PersistSessionState(const SReceiverSessionState &stSessionState, QString &strErrorMessage) const;

    /**
     * @brief BuildMissingRanges
     * @param stSessionState 当前会话。
     * @return 缺失分片范围列表。
     * @details 根据接收位图压缩出真正缺失的片段区间。
     */
    QList<SReliablePacketRange> BuildMissingRanges(const SReceiverSessionState &stSessionState) const;

    /**
     * @brief BuildAvailableFilePath
     * @param strPreferredFilePath 希望的最终文件路径。
     * @return 可用文件路径。失败时返回空字符串。
     * @details 避免接收完成时覆盖已有旧文件。
     */
    QString BuildAvailableFilePath(const QString &strPreferredFilePath) const;

    /**
     * @brief EncodeBitArray
     * @param oBitArray 需要编码的位图。
     * @return Base64 字符串。
     * @details 用于把接收位图序列化到 JSON 状态文件中。
     */
    QString EncodeBitArray(const QBitArray &oBitArray) const;

    /**
     * @brief DecodeBitArray
     * @param strBase64 Base64 字符串。
     * @param nBitCount 位图长度。
     * @return 解析后的位图。
     * @details 从 JSON 状态文件中恢复接收位图。
     */
    QBitArray DecodeBitArray(const QString &strBase64, int nBitCount) const;


    QString m_strOutputDirectory;// 接收文件的输出根目录
    QTcpServer *m_pTcpServer;// TCP 监听服务器，用于接受新连接
    QHash<QTcpSocket*,QByteArray> m_mapReceiverBuffer;// 每条连接各自的字节接收缓存，用于处理半包、粘包
    QHash<QUuid,SReceiverSessionState> m_mapReceiverSession;// 按传输 ID 保存各个文件的接收会话状态
    QThread *m_pSenderThread; //后台发送线程，避免发送过程阻塞界面
    ReliableSenderWorker *m_pReliableSenderWorker; // 发送工作对象，负责读取文件、发送分片、处理 ACK 等

};

#endif // RELIABLETRANSFERMANAGER_H
