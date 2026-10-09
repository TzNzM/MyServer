#ifndef PACKETCODEC_H
#define PACKETCODEC_H

#include "transfertypes.h"
#include <QByteArray>


class PacketCodec
{
public:
    PacketCodec();

    /**
     * @brief GetHeaderLength
     * @param 无
     * @return 固定包头长度。
     * @details 返回当前协议约定的固定包头字节数。
     */
    static int GetHeaderLength();

    /**
     * @brief BuildPacket
     * @param stPacketHeader 需要序列化的包头结构。
     * @param arrPayload 需要拼接到包尾的负载数据。
     * @return 完整可发送的数据包字节流。
     * @details 将固定包头与业务负载拼接成网络发送包。
     */
    static QByteArray BuildPacket(const SPacketHeader &stPacketHeader, const QByteArray &arrPayload);

    /**
     * @brief ParsePacketHeader
     * @param arrHeaderData 固定长度包头字节流。
     * @param stPacketHeader 输出的解析结果。
     * @param strErrorMessage 输出的错误描述。
     * @return 解析成功返回 true，否则返回 false。
     * @details 按固定顺序将网络头部还原为包头结构。
     */
    static bool ParsePacketHeader(const QByteArray &arrHeaderData, SPacketHeader &stPacketHeader, QString &strErrorMessage);

    /**
     * @brief BuildMetadataPayload
     * @param stMetadata 文件传输元信息。
     * @return 元信息对应的 JSON 负载。
     * @details 将文件名、分片策略等元信息序列化为 JSON。
     */
    static QByteArray BuildMetadataPayload(const STransferFileMetadata &stMetadata);

    /**
     * @brief ParseMetadataPayload
     * @param arrPayload 元信息 JSON 字节流。
     * @param stMetadata 输出的文件传输元信息。
     * @param strErrorMessage 输出的错误描述。
     * @return 解析成功返回 true，否则返回 false。
     * @details 将元信息负载解析为接收端可用的文件会话描述。
     */
    static bool ParseMetadataPayload(const QByteArray &arrPayload, STransferFileMetadata &stMetadata, QString &strErrorMessage);

    /**
     * @brief BuildMetadataPacket
     * @param stMetadata 文件传输元信息。
     * @return 用于发送的元信息完整数据包。
     * @details 生成开始传输前首先发送的握手元数据包。
     */
    static QByteArray BuildMetadataPacket(const STransferFileMetadata &stMetadata);

    /**
     * @brief BuildChunkPacket
     * @param stMetadata 当前文件的元信息。
     * @param nChunkIndex 当前块序号。
     * @param nSegmentIndex 当前块内分段序号。
     * @param nSegmentCount 当前块内总分段数。
     * @param nChunkOffset 当前块在原文件中的偏移量。
     * @param arrPayload 当前分段的二进制内容。
     * @return 可发送的数据包。
     * @details 根据块序号和分段特征位生成真正的数据分片包。
     */
    static QByteArray BuildChunkPacket(const STransferFileMetadata &stMetadata,
                                       quint32 nChunkIndex,
                                       quint32 nSegmentIndex,
                                       quint32 nSegmentCount,
                                       quint64 nChunkOffset,
                                       const QByteArray &arrPayload);
    /**
     * @brief BuildCompletePacket
     * @param stMetadata 当前文件的元信息。
     * @return 发送完成通知包。
     * @details 所有分片发送结束后通知接收端开始尝试最终合并。
     */
    static QByteArray BuildCompletePacket(const STransferFileMetadata &stMetadata);
};

#endif // PACKETCODEC_H
