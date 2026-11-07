#ifndef RELIABLETRANSFERUTILS_H
#define RELIABLETRANSFERUTILS_H

#include "ReliableTransferTypes.h"

//【公共计算提取】由原来的 ReliableReceiveSession::SLaneRange 移出。
//将一组计算结果放在一起，避免用多个引用参数分别返回。
//发送端和接收端共用同一套通道划分结果。
struct SReliableLaneRange
{
    qint64 m_nFirstSequence; // 当前通道负责的第一个全局分片序号，包含此序号
    qint64 m_nEndSequence;   // 当前通道分片区间的结束位置，不包含此序号
    qint64 m_nStartOffset;   // 当前通道在整个文件中的起始字节偏移
    qint64 m_nByteSize;      // 当前通道临时文件的字节数，最后一片按实际长度计算

    SReliableLaneRange()
        : m_nFirstSequence(0),
          m_nEndSequence(0),
          m_nStartOffset(0),
          m_nByteSize(0)
    {
    }
};

//单个分片在整个文件中的位置和实际长度。
struct SReliablePacketLayout
{
    qint64 m_nPayloadOffset; // 分片在整个文件中的起始字节偏移
    int m_nPayloadSize;     // 分片实际长度，最后一个分片可以更小

    SReliablePacketLayout()
        : m_nPayloadOffset(0),
          m_nPayloadSize(0)
    {
    }
};

/**
 * @brief GetReliablePacketCount
 * @param nFileSize 文件大小，必须大于 0。
 * @param nSegmentSize 标准分片大小，必须大于 0。
 * @return 向上取整后的总分片数。
 * @details 调用方先检查参数；返回值使用 qint64，
 *          是否超过位图容量仍由调用方判断。
 */
inline qint64 GetReliablePacketCount(
    qint64 nFileSize,
    int nSegmentSize)
{
    //整除部分加上不足一个分片的尾部，避免加法形式可能溢出。
    return nFileSize / nSegmentSize
        + (nFileSize % nSegmentSize != 0);
}

/**
 * @brief GetReliableLaneRange
 * @param stMetadata 已通过校验的文件元信息。
 * @param nLaneIndex 数据通道编号，从 0 开始，调用前必须保证编号合法。
 * @return 当前通道的分片区间、全局起始偏移和实际字节数。
 * @details 分片区间为左闭右开区间，统一供发送端区间分配，
 *          以及接收端通道文件创建、分片归属校验和最终合并使用。
 *          只读取调用期间不再改变的元信息，不在内部加锁。
 */
inline SReliableLaneRange GetReliableLaneRange(
    const SReliableFileMetadata &stMetadata,
    int nLaneIndex)
{
    const qint64 nPacketCount =
        static_cast<qint64>(stMetadata.m_nTotalPacketCount);

    SReliableLaneRange stLaneRange;

    //通道负责的全局分片区间：[nFirstSequence, nEndSequence)
    stLaneRange.m_nFirstSequence =
        nPacketCount * nLaneIndex / stMetadata.m_nLaneCount;

    stLaneRange.m_nEndSequence =
        nPacketCount * (nLaneIndex + 1) / stMetadata.m_nLaneCount;

    //计算当前通道的起始字节位置。
    stLaneRange.m_nStartOffset =
        stLaneRange.m_nFirstSequence * stMetadata.m_nSegmentSize;

    //最后一个分片可能不足标准分片大小。
    //需要用 qMin() 将结束偏移量限制在文件末尾。
    const qint64 nEndOffset = qMin(
        stLaneRange.m_nEndSequence * stMetadata.m_nSegmentSize,
        stMetadata.m_nFileSize);

    stLaneRange.m_nByteSize = nEndOffset - stLaneRange.m_nStartOffset;

    return stLaneRange;
}

/**
 * @brief GetReliablePacketLayout
 * @param stMetadata 已通过校验的文件元信息。
 * @param nSequence 全局分片序号，必须小于总分片数。
 * @return 分片在整个文件中的起始偏移和实际长度。
 * @details 发送端用于读取分片，接收端用于计算期望值；
 *          不直接相信发送端包头给出的偏移和长度。
 */
inline SReliablePacketLayout GetReliablePacketLayout(
    const SReliableFileMetadata &stMetadata,
    quint64 nSequence)
{
    SReliablePacketLayout stLayout;

    stLayout.m_nPayloadOffset =
        static_cast<qint64>(nSequence) * stMetadata.m_nSegmentSize;

    //最后一个分片可能小于标准分片大小。
    stLayout.m_nPayloadSize = static_cast<int>(
        qMin<qint64>(
            stMetadata.m_nSegmentSize,
            stMetadata.m_nFileSize - stLayout.m_nPayloadOffset));

    return stLayout;
}

#endif // RELIABLETRANSFERUTILS_H
