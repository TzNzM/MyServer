#ifndef RELIABLEMERGEHASHER_H
#define RELIABLEMERGEHASHER_H

#include <QAtomicInt>
#include <QByteArray>
#include <QMutex>
#include <QQueue>
#include <QSharedPointer>
#include <QString>
#include <QThread>
#include <QWaitCondition>
#include <array>

//【合并流水线】合并线程负责读写文件，唯一的摘要线程按提交顺序计算 SHA-256。
//三个固定缓冲区轮流交接所有权，队列满时等待，不随文件大小增加内存占用。
class ReliableMergeHasher : public QThread
{
public:
    /**
     * @brief ReliableMergeHasher
     * @param pStopFlag 会话共享停止标记，非空。
     * @return 无
     * @details 分配三个固定缓冲区并启动后台摘要线程。
     */
    explicit ReliableMergeHasher(const QSharedPointer<QAtomicInt> &pStopFlag);
    /**
     * @brief ~ReliableMergeHasher
     * @param 无
     * @return 无
     * @details 提前退出时也唤醒并等待线程，不让线程访问已释放的缓冲区。
     */
    ~ReliableMergeHasher() override;

    /**
     * @brief AcquireBuffer
     * @param nBufferIndex 输出空闲缓冲区编号。
     * @param strErrorMessage 输出取消或摘要失败原因。
     * @return 取得缓冲区返回 true。
     * @details 仅合并线程调用；等待期间定期检查停止标记。
     */
    bool AcquireBuffer(int &nBufferIndex, QString &strErrorMessage);

    /**
     * @brief GetBuffer
     * @param nBufferIndex AcquireBuffer 刚取得的编号。
     * @return 当前合并线程独占的缓冲区引用。
     * @details 提交后由摘要线程独占，合并线程禁止继续读写。
     */
    QByteArray &GetBuffer(int nBufferIndex);
    /**
     * @brief SubmitBuffer
     * @param nBufferIndex 已取得、尚未提交的缓冲区编号。
     * @param nDataSize 有效字节数，范围为 1 到缓冲区容量。
     * @param strErrorMessage 输出取消或摘要失败原因。
     * @return 成功交接返回 true。
     * @details 仅合并线程按文件顺序调用，交接后不再持有缓冲区使用权。
     */
    bool SubmitBuffer(int nBufferIndex, int nDataSize, QString &strErrorMessage);

    /**
     * @brief Finish
     * @param arrDigest 输出完整文件的原始 SHA-256。
     * @param strErrorMessage 输出失败原因。
     * @return 队列排空且摘要成功返回 true。
     * @details 通知输入结束并等待线程退出；只有成功返回后才允许提交合并文件。
     */
    bool Finish(QByteArray &arrDigest, QString &strErrorMessage);
    /**
     * @brief CancelAndWait
     * @param 无
     * @return 无
     * @details 唤醒等待中的线程并回收；提前退出使用，可重复调用。
     */
    void CancelAndWait();

    /**
     * @brief GetHashTimeNs
     * @param 无
     * @return 摘要后端操作累计纳秒数，不含队列等待。
     * @details 只能在线程已退出后调用；耗时与合并读写存在重叠。
     */
    qint64 GetHashTimeNs() const;
    /**
     * @brief GetHashedByteCount
     * @param 无
     * @return 已进入摘要的字节数。
     * @details 只能在线程已退出后调用，用于检查摘要没有遗漏数据。
     */
    qint64 GetHashedByteCount() const;

protected:
    void run() override;

private:
    Q_DISABLE_COPY(ReliableMergeHasher)
    bool CheckStoppedLocked(QString &strErrorMessage);
    void SetError(const QString &strErrorMessage);

    struct SBuffer
    {
        QByteArray m_arrData;
        int m_nDataSize = 0;
    };
    std::array<SBuffer, 3> m_arrBuffers;
    QQueue<int> m_arrFreeBuffers;
    QQueue<int> m_arrReadyBuffers;
    QSharedPointer<QAtomicInt> m_pStopFlag;
    QMutex m_oMutex;
    QWaitCondition m_oChanged;
    bool m_bInputFinished = false;
    bool m_bCancelled = false;
    QString m_strErrorMessage;
    QByteArray m_arrDigest;
    qint64 m_nHashTimeNs = 0;
    qint64 m_nHashedByteCount = 0;
};

#endif // RELIABLEMERGEHASHER_H
