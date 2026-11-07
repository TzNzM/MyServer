#ifndef RELIABLEPACKETCODEC_H
#define RELIABLEPACKETCODEC_H

#include "ReliableTransferTypes.h"

class ReliablePacketCodec
{
public:
    /**
     * @brief ReliablePacketCodec
     * @param 无
     * @return 无
     * @details 保留原有构造接口；下面的静态函数仍可直接通过类名调用。
     */
    ReliablePacketCodec();

    /**
     * @brief GetHeaderLength
     * @param 无
     * @return 可靠传输协议固定包头长度。
     * @details 返回当前长连接可靠协议使用的固定头长度。
     */
    static int GetHeaderLength();

    /**
     * @brief BuildPacket
     * @param stPacketHeader 需要编码的包头。
     * @param arrPayload 需要附加的负载数据。
     * @return 完整网络包。
     * @details 把可靠传输头和负载合成为可直接收发的包。
     */
    static QByteArray BuildPacket(const SReliablePacketHeader &stPacketHeader, const QByteArray &arrPayload);

    /**
     * @brief ParsePacketHeader
     * @param arrHeaderData 固定长度包头字节流。
     * @param stPacketHeader 输出的包头结构。
     * @param strErrorMessage 输出错误信息。
     * @return 成功返回 true，否则返回 false。
     * @details 将 62 字节固定包头解析回结构化字段，并校验魔数、版本、类型、传输 ID 和载荷上限。
     */
    static bool ParsePacketHeader(const QByteArray &arrHeaderData, SReliablePacketHeader &stPacketHeader, QString &strErrorMessage);


    /**
     * @brief TryTakePacket
     * @param arrBuffer 当前连接独立维护的接收缓存。
     * @param stHeader 输出解析后的包头。
     * @param arrPayload 输出当前完整包的载荷。
     * @param bReady 输出是否已经取得一个完整包。
     * @param strErrorMessage 输出非法包的原因。
     * @return 缓存中的协议格式有效返回 true，格式非法返回 false。
     * @details true 且 bReady 为 false 表示半包，保留缓存等待后续字节；完整包只移除自身，后续粘连包继续保留。
     */
    static bool TryTakePacket(QByteArray &arrBuffer, SReliablePacketHeader &stHeader,
                              QByteArray &arrPayload, bool &bReady, QString &strErrorMessage);

    /**
     * @brief ValidateMetadata
     * @param stMetadata 待校验的文件元信息。
     * @param strErrorMessage 输出校验失败原因。
     * @return 文件名、大小、分片数、通道数、摘要和会话键合法返回 true。
     * @details 不校验 UUID，因为 Metadata 载荷中不保存 UUID，调用方从包头读取并关联当前任务。
     */
    static bool ValidateMetadata(const SReliableFileMetadata &stMetadata, QString &strErrorMessage);

    /**
     * @brief BuildSessionKey
     * @param stMetadata 已设置文件 SHA-256、分片大小和实际通道数的元信息。
     * @return 用于定位历史续传状态的稳定 SHA-256 会话键。
     * @details 对 v3、文件名、文件大小、分片大小、实际通道数和文件 SHA-256 拼接后的 UTF-8 字节计算摘要。
     */
    static QString BuildSessionKey(const SReliableFileMetadata &stMetadata);

    /**
     * @brief BuildMetadataPacket
     * @param stMetadata 文件元信息。
     * @param nWindowSize 发送端声明的每条数据连接的窗口大小。
     * @param nLaneIndex 本连接的通道编号；-1 为控制连接，0..N-1 为数据连接。
     * @return 元信息控制包。
     * @details 构造会话建立或断点续传恢复握手包。
     */
    static QByteArray BuildMetadataPacket(const SReliableFileMetadata &stMetadata, int nWindowSize, int nLaneIndex = -1);


    /**
     * @brief ParseMetadataPayload
     * @param arrPayload 元信息 JSON。
     * @param stMetadata 输出的文件元信息。
     * @param pLaneIndex 可选输出本连接的通道编号，不需要时可省略。
     * @param strErrorMessage 输出错误信息。
     * @return 成功返回 true，否则返回 false。
     * @details 将元信息 JSON 负载还原成可靠传输会话元数据。
     */
    static bool ParseMetadataPayload(const QByteArray &arrPayload, SReliableFileMetadata &stMetadata, QString &strErrorMessage, int *pLaneIndex = nullptr);

    /**
     * @brief BuildResumeStatePacket
     * @param stMetadata 文件元信息。
     * @param arrMissingRangeList 接收端尚缺失的分片范围。
     * @param nReceivedPacketCount 已接收分片数。
     * @return 恢复状态包。
     * @details 接收端把当前缺失分片窗口回传给发送端。
     */
    static QByteArray BuildResumeStatePacket(const SReliableFileMetadata &stMetadata,
                                             const QList<SReliablePacketRange> &arrMissingRangeList,
                                             quint32 nReceivedPacketCount);

    /**
     * @brief ParseResumeStatePayload
     * @param arrPayload 恢复状态 JSON。
     * @param arrMissingRangeList 输出的缺失分片范围。
     * @param nReceivedPacketCount 输出的已接收分片数。
     * @param strErrorMessage 输出错误信息。
     * @return 成功返回 true，否则返回 false。
     * @details 解析接收端返回的断点续传缺失信息。
     */
    static bool ParseResumeStatePayload(const QByteArray &arrPayload,
                                        QList<SReliablePacketRange> &arrMissingRangeList,
                                        quint32 &nReceivedPacketCount,
                                        QString &strErrorMessage);

    /**
     * @brief BuildDataPacket
     * @param stMetadata 文件元信息。
     * @param nPacketSequence 当前分片序号。
     * @param nPayloadOffset 当前负载在文件中的偏移量。
     * @param arrPayload 分片内容。
     * @return 数据分片包。
     * @details 构造带偏移和分片序号的可靠数据包。
     */
    static QByteArray BuildDataPacket(const SReliableFileMetadata &stMetadata,
                                      quint64 nPacketSequence,
                                      quint64 nPayloadOffset,
                                      const QByteArray &arrPayload);

    /**
     * @brief BuildAckPacket
     * @param stMetadata 文件元信息。
     * @param nPacketSequence 已确认收到的分片序号。
     * @return ACK 控制包。
     * @details 接收端对每个已写入临时文件且更新内存位图的分片进行确认；ACK 不保证状态文件已保存或物理介质持久化。
     */
    static QByteArray BuildAckPacket(const SReliableFileMetadata &stMetadata, quint64 nPacketSequence);


    /**
     * @brief BuildCompleteRequestPacket
     * @param stMetadata 文件元信息。
     * @return 完成请求包。
     * @details 所有缺失分片补齐后，由控制连接通知接收端合并各通道临时文件并执行最终 SHA-256 校验。
     */
    static QByteArray BuildCompleteRequestPacket(const SReliableFileMetadata &stMetadata);


    /**
     * @brief BuildResultPacket
     * @param stMetadata 文件元信息。
     * @param bSuccess 是否成功。
     * @param strMessage 结果描述。
     * @param arrMissingRangeList 缺失分片范围。
     * @return 结果控制包。
     * @details 接收端在校验或完成阶段回传成功、失败或缺失清单。
     */
    static QByteArray BuildResultPacket(const SReliableFileMetadata &stMetadata,
                                        bool bSuccess,
                                        const QString &strMessage,
                                        const QList<SReliablePacketRange> &arrMissingRangeList);

    /**
     * @brief BuildErrorPacket
     * @param stMetadata 文件元信息。
     * @param strMessage 错误描述。
     * @return 错误控制包。
     * @details 把协议级异常封装成错误包返回给对端。
     */
    static QByteArray BuildErrorPacket(const SReliableFileMetadata &stMetadata, const QString &strMessage);


    /**
     * @brief ParseResultPayload
     * @param arrPayload 结果 JSON。
     * @param bSuccess 输出成功标记。
     * @param strMessage 输出结果消息。
     * @param arrMissingRangeList 输出缺失分片范围。
     * @param strErrorMessage 输出错误信息。
     * @return 成功返回 true，否则返回 false。
     * @details 解析接收端返回的最终校验或补传结果。
     */
    static bool ParseResultPayload(const QByteArray &arrPayload,
                                   bool &bSuccess,
                                   QString &strMessage,
                                   QList<SReliablePacketRange> &arrMissingRangeList,
                                   QString &strErrorMessage);

};

#endif // RELIABLEPACKETCODEC_H
