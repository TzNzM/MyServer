#ifndef RELIABLEPACKETCODEC_H
#define RELIABLEPACKETCODEC_H

#include "ReliableTransferTypes.h"

class ReliablePacketCodec
{
public:
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
     * @details 将固定包头解析回结构化字段。
     */
    static bool ParsePacketHeader(const QByteArray &arrHeaderData, SReliablePacketHeader &stPacketHeader, QString &strErrorMessage);

    /**
     * @brief BuildMetadataPacket
     * @param stMetadata 文件元信息。
     * @param nWindowSize 发送端声明的窗口大小。
     * @return 元信息控制包。
     * @details 构造会话建立或断点续传恢复握手包。
     */
    static QByteArray BuildMetadataPacket(const SReliableFileMetadata &stMetadata, int nWindowSize);


    /**
     * @brief ParseMetadataPayload
     * @param arrPayload 元信息 JSON。
     * @param stMetadata 输出的文件元信息。
     * @param strErrorMessage 输出错误信息。
     * @return 成功返回 true，否则返回 false。
     * @details 将元信息 JSON 负载还原成可靠传输会话元数据。
     */
    static bool ParseMetadataPayload(const QByteArray &arrPayload, SReliableFileMetadata &stMetadata, QString &strErrorMessage);

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
     * @details 接收端对每个已落盘分片进行确认。
     */
    static QByteArray BuildAckPacket(const SReliableFileMetadata &stMetadata, quint64 nPacketSequence);


    /**
     * @brief BuildCompleteRequestPacket
     * @param stMetadata 文件元信息。
     * @return 完成请求包。
     * @details 所有缺失分片补齐后通知接收端执行最终校验。
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
