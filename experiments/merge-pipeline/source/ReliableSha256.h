#ifndef RELIABLESHA256_H
#define RELIABLESHA256_H

#include <QByteArray>
#include <QScopedPointer>
#include <QString>

//【CNG SHA-256】Windows 使用系统 CNG。
//【Linux OpenSSL】Linux 改用 OpenSSL EVP，其余平台保留 Qt 后端。
//每个文件计算创建独立对象，同一对象由调用线程顺序使用。
class ReliableSha256
{
public:
    /**
     * @brief ReliableSha256
     * @param 无
     * @return 无
     * @details 创建摘要对象，使用前调用 Initialize。
     */
    ReliableSha256();

    /**
     * @brief ~ReliableSha256
     * @param 无
     * @return 无
     * @details 自动释放摘要状态、Windows CNG 句柄或 Linux EVP 上下文。
     */
    ~ReliableSha256();

    /**
     * @brief Initialize
     * @param strErrorMessage 输出初始化失败的原因。
     * @return 初始化成功返回 true，否则返回 false。
     * @details 清理上一轮状态，开始计算新的标准 SHA-256。
     */
    bool Initialize(QString &strErrorMessage);

    /**
     * @brief AddData
     * @param pData 本批数据地址，长度为 0 时允许为空。
     * @param nDataSize 本批数据字节数，必须大于等于 0。
     * @param strErrorMessage 输出计算失败的原因。
     * @return 追加成功返回 true，否则返回 false。
     * @details 按文件顺序增量更新摘要，不保存整份文件。
     */
    bool AddData(const char *pData, int nDataSize, QString &strErrorMessage);

    /**
     * @brief Finish
     * @param arrDigest 输出 32 字节原始摘要，不是十六进制字符串。
     * @param strErrorMessage 输出计算失败的原因。
     * @return 完成计算返回 true，否则返回 false。
     * @details 完成后不再接受新数据，重复调用返回缓存结果；
     *          如需计算另一份数据，应重新 Initialize。
     */
    bool Finish(QByteArray &arrDigest, QString &strErrorMessage);

private:
    Q_DISABLE_COPY(ReliableSha256)
    struct SHashState;
    QScopedPointer<SHashState> m_pState; // 独占摘要状态，不跨对象共享原生句柄
};

#endif // RELIABLESHA256_H
