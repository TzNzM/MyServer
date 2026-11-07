#ifndef RELIABLERECEIVESESSION_H
#define RELIABLERECEIVESESSION_H

#include "ReliableTransferTypes.h"

#include <QAtomicInt>
#include <QMutex>
#include <QSharedPointer>


struct SReceiverSessionState
{
    SReliableFileMetadata m_stMetadata; // 文件元信息：文件名、大小、分片规格、哈希、传输 ID 等

    QString m_strSessionDirectory; // 保存本次文件传输临时数据和状态的目录
    QString m_strPartFilePath; // 合并输出 assembled.part 的完整路径；接收数据阶段写各通道独立的临时文件
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


class ReliableReceiveSession
{
public:
    /**
     * @brief ReliableReceiveSession
     * @param stMetadata 本次文件传输的元信息。
     * @param strOutputDirectory 接收文件的输出根目录。
     * @param pStopFlag 多个接收连接共享的停止标记。
     * @return 无
     * @details 初始化一个文件接收会话；后续多个通道共享位图和进度，
     *          各数据通道写入自己的独立临时文件。
     */
    ReliableReceiveSession(
        const SReliableFileMetadata &stMetadata,
        const QString &strOutputDirectory,
        const QSharedPointer<QAtomicInt> &pStopFlag);

    /**
     * @brief GetMetadata
     * @param 无
     * @return 当前会话的文件元信息副本。
     * @details 元信息在创建会话时确定，之后保持不变，
     *          用于校验分片和构造协议回包。
     */
    SReliableFileMetadata GetMetadata() const;

    /**
     * @brief PersistSessionState
     * @param strErrorMessage 输出保存失败的原因。
     * @return 保存成功返回 true，否则返回 false。
     * @details 先锁住共享会话状态，再调用内部函数保存，
     *          防止其他接收线程在保存期间修改位图和进度。
     */
    bool PersistSessionState(QString &strErrorMessage);

    /**
     * @brief LoadOrCreateSession
     * @param strErrorMessage 输出创建或恢复失败的原因。
     * @return 会话准备成功返回 true，否则返回 false。
     * @details 校验元信息，准备会话目录、通道临时文件和接收位图。
     *          首次接收时创建新状态；存在历史状态时校验并恢复接收进度。
     */
    bool LoadOrCreateSession(QString &strErrorMessage);

    /**
     * @brief BuildResumeStatePacket
     * @param 无
     * @return 包含缺失分片范围和已接收数量的续传状态包。
     * @details 持有会话锁读取接收状态，保证缺失范围和计数相互一致。
     */
    QByteArray BuildResumeStatePacket();

    /**
     * @brief AcceptDataPacket
     * @param stHeader 当前数据分片的包头。
     * @param arrPayload 当前分片的文件内容。
     * @param nLaneIndex 当前连接对应的数据通道编号。
     * @param bAccepted 输出该分片是否可以回复 ACK。
     * @param bNewPacket 输出是否新增了一个已接收分片。
     * @param strWarningMessage 输出状态持久化失败的警告。
     * @param strErrorMessage 输出分片校验或文件写入失败的原因。
     * @return 分片有效且处理成功返回 true，否则返回 false。
     * @details 每个通道由一个接收线程独占，锁外写自己的临时文件，
     *          写入成功后在会话锁内更新共享位图和计数。
     */
    bool AcceptDataPacket(
        const SReliablePacketHeader &stHeader,
        const QByteArray &arrPayload,
        int nLaneIndex,
        bool &bAccepted,
        bool &bNewPacket,
        QString &strWarningMessage,
        QString &strErrorMessage);

    /**
     * @brief GetReceivedPacketCount
     * @param 无
     * @return 当前已接收的不同分片数量。
     * @details 持有会话锁读取共享计数，供外部更新接收进度。
     */
    quint32 GetReceivedPacketCount();

    /**
     * @brief VerifyAndFinalizeSession
     * @param arrMissingRanges 输出仍然缺失的分片范围。
     * @param strFinalPath 输出成功生成的正式文件路径。
     * @param strErrorMessage 输出处理失败的原因。
     * @param pTimingMessage 可选输出本次合并各阶段的累计耗时。
     * @return 分片齐全、合并校验通过且正式文件生成成功返回 true。
     * @details 按通道编号顺序合并，使用固定大小缓冲区，
     *          默认流水线读写与顺序 SHA-256 重叠；MYSERVER_MERGE_MODE=serial 使用原串行方式。
     *          两种方式均在完整摘要校验通过后提交文件，成功后清理会话临时文件。
     *          已完成的会话再次调用时直接返回成功。
     *          计时使用单调时钟累计，返回时汇总，不在分片循环中输出日志。
     */
    bool VerifyAndFinalizeSession(
        QList<SReliablePacketRange> &arrMissingRanges,
        QString &strFinalPath,
        QString &strErrorMessage,
        QString *pTimingMessage = nullptr);

private:
    //【公共计算提取】通道范围类型和计算已移到 ReliableTransferUtils.h。

    /**
     * @brief BuildMissingRangesLocked
     * @param 无
     * @return 缺失分片的闭区间列表，区间包含起点和终点。
     * @details 调用者必须已经持有 m_oMutex。
     *          遍历接收位图，将连续未收到的分片合并为一个范围。
     */
    QList<SReliablePacketRange> BuildMissingRangesLocked() const;

    /**
     * @brief GetLaneFilePath
     * @param nLaneIndex 数据通道编号，从 0 开始。
     * @return 当前通道的独立临时文件路径。
     * @details 在会话目录已经确定后调用，按通道编号生成文件名。
     */
    QString GetLaneFilePath(int nLaneIndex) const;

    /**
     * @brief BuildAvailableFilePath
     * @param strPreferredFilePath 希望使用的最终文件路径。
     * @return 可用的候选路径；编号候选耗尽时返回空字符串。
     * @details 避免覆盖已有文件；这里只寻找路径，不创建或占用文件。
     */
    static QString BuildAvailableFilePath(
        const QString &strPreferredFilePath);

    /**
     * @brief PersistSessionStateLocked
     * @param strErrorMessage 输出保存失败的原因。
     * @return 保存成功返回 true，否则返回 false。
     * @details 调用者必须已经持有 m_oMutex。
     *          使用 QSaveFile 保存状态，成功后清零未保存分片数。
     */
    bool PersistSessionStateLocked(QString &strErrorMessage);

    QMutex m_oMutex; // 保护多个接收线程共享的位图、计数和会话状态
    SReceiverSessionState m_stState; // 当前文件的接收状态
    QString m_strOutputDirectory; // 接收输出根目录
    QSharedPointer<QAtomicInt> m_pStopFlag; // 共享停止标记，后续用于中止耗时合并
};

#endif // RELIABLERECEIVESESSION_H
