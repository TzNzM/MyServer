#include "ReliableSha256.h"

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#include <bcrypt.h>
#elif defined(Q_OS_LINUX)
//【Linux OpenSSL】只在 Linux 编译此后端，Windows 不引入 OpenSSL 依赖。
#include <openssl/evp.h>
#include <openssl/err.h>
#else
#include <QCryptographicHash>
#endif

struct ReliableSha256::SHashState
{
#ifdef Q_OS_WIN
    BCRYPT_ALG_HANDLE m_hAlgorithm = nullptr; // SHA-256 算法提供者
    BCRYPT_HASH_HANDLE m_hHash = nullptr;     // 当前文件的增量摘要状态

    ~SHashState()
    {
        //先释放摘要对象，再关闭它使用的算法提供者。
        if (m_hHash != nullptr)
        {
            BCryptDestroyHash(m_hHash);
        }
        if (m_hAlgorithm != nullptr)
        {
            BCryptCloseAlgorithmProvider(m_hAlgorithm, 0);
        }
    }
#elif defined(Q_OS_LINUX)
    EVP_MD_CTX *m_pOpenSslContext = nullptr; // 本次文件的增量 SHA-256 上下文

    ~SHashState()
    {
        EVP_MD_CTX_free(m_pOpenSslContext);
    }
#else
    QCryptographicHash m_oHash{QCryptographicHash::Sha256};
#endif
    bool m_bInitialized = false;
    bool m_bFinished = false;
    QByteArray m_arrDigest;
};

#ifdef Q_OS_WIN
/**
 * @brief BuildCngErrorMessage
 * @param strOperation 失败的 CNG 操作名称。
 * @param nStatus CNG 返回的 NTSTATUS 状态码。
 * @return 包含操作和十六进制状态码的错误信息。
 * @details CNG 通过状态码报告失败，不读取 GetLastError。
 */
static QString BuildCngErrorMessage(const QString &strOperation, NTSTATUS nStatus)
{
    return QStringLiteral("Windows CNG %1 失败，状态码：0x%2。")
        .arg(strOperation)
        .arg(static_cast<quint32>(nStatus), 8, 16, QChar('0'));
}
#elif defined(Q_OS_LINUX)
/**
 * @brief BuildOpenSslErrorMessage
 * @param strOperation 失败的 OpenSSL EVP 操作名称。
 * @return 包含当前线程 OpenSSL 错误队列信息的错误描述。
 * @details 每次 EVP 操作前清理旧错误；失败时取出本次错误并清空队列，
 *          队列为空时仍报告失败，不使用 errno 或 GetLastError。
 */
static QString BuildOpenSslErrorMessage(const QString &strOperation)
{
    const unsigned long nErrorCode = ERR_get_error();
    char arrErrorText[256] = {};
    if (nErrorCode != 0)
    {
        ERR_error_string_n(nErrorCode, arrErrorText, sizeof(arrErrorText));
    }
    ERR_clear_error();
    return QStringLiteral("OpenSSL %1 失败：%2。")
        .arg(strOperation)
        .arg(nErrorCode != 0
                 ? QString::fromLatin1(arrErrorText)
                 : QStringLiteral("未提供错误详情"));
}
#endif

ReliableSha256::ReliableSha256()
    : m_pState()
{
}

ReliableSha256::~ReliableSha256() = default;

bool ReliableSha256::Initialize(QString &strErrorMessage)
{
    strErrorMessage.clear();
    m_pState.reset(new SHashState);
#ifdef Q_OS_WIN
    //第一段：打开 SHA-256 提供者，不使用 HMAC。
    NTSTATUS nStatus = BCryptOpenAlgorithmProvider(
        &m_pState->m_hAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (nStatus < 0)
    {
        strErrorMessage = BuildCngErrorMessage(QStringLiteral("打开 SHA-256 提供者"), nStatus);
        return false;
    }

    //第二段：确认输出长度，保持协议中的标准 32 字节摘要。
    ULONG nDigestSize = 0;
    ULONG nResultSize = 0;
    nStatus = BCryptGetProperty(
        m_pState->m_hAlgorithm, BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&nDigestSize), sizeof(nDigestSize), &nResultSize, 0);
    if (nStatus < 0)
    {
        strErrorMessage = BuildCngErrorMessage(QStringLiteral("读取 SHA-256 长度"), nStatus);
        return false;
    }
    if (nResultSize != sizeof(nDigestSize) || nDigestSize != 32)
    {
        strErrorMessage = QStringLiteral("Windows CNG 的 SHA-256 输出长度异常。");
        return false;
    }

    //第三段：CNG 分配摘要状态内存，DestroyHash 时自动回收。
    //该方式从 Windows 7 起支持，不需要额外创建工作线程。
    nStatus = BCryptCreateHash(
        m_pState->m_hAlgorithm, &m_pState->m_hHash, nullptr, 0, nullptr, 0, 0);
    if (nStatus < 0)
    {
        strErrorMessage = BuildCngErrorMessage(QStringLiteral("创建 SHA-256 对象"), nStatus);
        return false;
    }
#elif defined(Q_OS_LINUX)
    //【Linux OpenSSL】EVP 由库选择实现，计算结果仍是标准 SHA-256。
    ERR_clear_error();
    m_pState->m_pOpenSslContext = EVP_MD_CTX_new();
    if (m_pState->m_pOpenSslContext == nullptr)
    {
        strErrorMessage = BuildOpenSslErrorMessage(QStringLiteral("创建 SHA-256 上下文"));
        return false;
    }
    ERR_clear_error();
    if (EVP_DigestInit_ex(m_pState->m_pOpenSslContext, EVP_sha256(), nullptr) != 1)
    {
        strErrorMessage = BuildOpenSslErrorMessage(QStringLiteral("初始化 SHA-256"));
        return false;
    }
    if (EVP_MD_CTX_size(m_pState->m_pOpenSslContext) != 32)
    {
        strErrorMessage = QStringLiteral("OpenSSL 的 SHA-256 输出长度异常。");
        return false;
    }
#endif
    m_pState->m_bInitialized = true;
    return true;
}

bool ReliableSha256::AddData(const char *pData, int nDataSize, QString &strErrorMessage)
{
    strErrorMessage.clear();
    if (m_pState.isNull() || !m_pState->m_bInitialized || m_pState->m_bFinished)
    {
        strErrorMessage = QStringLiteral("SHA-256 尚未初始化、已失败或已完成。");
        return false;
    }
    if (nDataSize < 0 || (nDataSize > 0 && pData == nullptr))
    {
        strErrorMessage = QStringLiteral("SHA-256 输入地址或长度非法。");
        return false;
    }
    if (nDataSize == 0)
    {
        return true;
    }
#ifdef Q_OS_WIN
    //CNG 输入参数未声明 const，但 BCryptHashData 不修改输入内容。
    const NTSTATUS nStatus = BCryptHashData(
        m_pState->m_hHash, reinterpret_cast<PUCHAR>(const_cast<char *>(pData)),
        static_cast<ULONG>(nDataSize), 0);
    if (nStatus < 0)
    {
        m_pState->m_bInitialized = false;
        strErrorMessage = BuildCngErrorMessage(QStringLiteral("更新 SHA-256"), nStatus);
        return false;
    }
#elif defined(Q_OS_LINUX)
    ERR_clear_error();
    if (EVP_DigestUpdate(
            m_pState->m_pOpenSslContext, pData, static_cast<size_t>(nDataSize)) != 1)
    {
        m_pState->m_bInitialized = false;
        strErrorMessage = BuildOpenSslErrorMessage(QStringLiteral("更新 SHA-256"));
        return false;
    }
#else
    m_pState->m_oHash.addData(pData, nDataSize);
#endif
    return true;
}

bool ReliableSha256::Finish(QByteArray &arrDigest, QString &strErrorMessage)
{
    arrDigest.clear();
    strErrorMessage.clear();
    if (m_pState.isNull() || !m_pState->m_bInitialized)
    {
        strErrorMessage = QStringLiteral("SHA-256 尚未初始化或已失败。");
        return false;
    }
    if (!m_pState->m_bFinished)
    {
#ifdef Q_OS_WIN
        m_pState->m_arrDigest.resize(32);
        const NTSTATUS nStatus = BCryptFinishHash(
            m_pState->m_hHash, reinterpret_cast<PUCHAR>(m_pState->m_arrDigest.data()),
            static_cast<ULONG>(m_pState->m_arrDigest.size()), 0);
        if (nStatus < 0)
        {
            m_pState->m_bInitialized = false;
            m_pState->m_arrDigest.clear();
            strErrorMessage = BuildCngErrorMessage(QStringLiteral("完成 SHA-256"), nStatus);
            return false;
        }
#elif defined(Q_OS_LINUX)
        m_pState->m_arrDigest.resize(EVP_MAX_MD_SIZE);
        unsigned int nDigestSize = 0;
        ERR_clear_error();
        if (EVP_DigestFinal_ex(
                m_pState->m_pOpenSslContext,
                reinterpret_cast<unsigned char *>(m_pState->m_arrDigest.data()),
                &nDigestSize) != 1)
        {
            m_pState->m_bInitialized = false;
            m_pState->m_arrDigest.clear();
            strErrorMessage = BuildOpenSslErrorMessage(QStringLiteral("完成 SHA-256"));
            return false;
        }
        if (nDigestSize != 32)
        {
            m_pState->m_bInitialized = false;
            m_pState->m_arrDigest.clear();
            strErrorMessage = QStringLiteral("OpenSSL 的 SHA-256 结果长度异常。");
            return false;
        }
        m_pState->m_arrDigest.resize(static_cast<int>(nDigestSize));
#else
        m_pState->m_arrDigest = m_pState->m_oHash.result();
#endif
        m_pState->m_bFinished = true;
    }
    arrDigest = m_pState->m_arrDigest;
    return true;
}
