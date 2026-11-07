#ifndef RELIABLETRANSFERIO_H
#define RELIABLETRANSFERIO_H

#include "ReliableTransferTypes.h"

#include <functional>

class QTcpSocket;

//取消检查函数：不接收参数，返回 true 表示请求取消。
//调用方必须提供有效的函数。
using ReliableCancelCheck = std::function<bool()>;


/**
 * @brief ReliableTransferIo
 * @details 提供发送端后台线程使用的阻塞式网络操作；
 *          socket 必须由调用线程拥有和使用。
 */
class ReliableTransferIo
{
public:
    /**
     * @brief WritePacket
     * @param oSocket 当前线程拥有的已连接 socket。
     * @param arrPacket 已经封装好的完整协议包。
     * @param fnCancelled 取消检查函数，返回 true 时结束写出。
     * @param strErrorMessage 输出失败原因。
     * @return 整个包写入且 Qt 待发送缓冲区排空返回 true，
     *         取消、断线、写入失败或超时返回 false。
     * @details 处理分次写入，并通过短时间等待保持取消响应；
     *          成功只表示本地写出完成，对端业务接收仍需 ACK 确认。
     */
    static bool WritePacket(
        QTcpSocket &oSocket,
        const QByteArray &arrPacket,
        const ReliableCancelCheck &fnCancelled,
        QString &strErrorMessage);

    /**
     * @brief ReadPacket
     * @param oSocket 当前线程拥有的 socket。
     * @param arrBuffer 当前连接的接收缓存，调用之间保留。
     * @param stHeader 输出完整协议包的包头。
     * @param arrPayload 输出完整协议包的载荷。
     * @param nTimeoutMs 本次读取的总超时，单位：毫秒。
     * @param fnCancelled 取消检查函数，返回 true 表示请求取消。
     * @param strErrorMessage 输出失败原因。
     * @return 取得一个合法完整包返回 true，否则返回 false。
     * @details 优先解析已有缓存；半包继续读取，粘包保留后续内容。
     *          格式非法、取消、断线或总超时均结束等待。
     *          仅供 socket 所属的后台发送线程调用。
     */
    static bool ReadPacket(
        QTcpSocket &oSocket,
        QByteArray &arrBuffer,
        SReliablePacketHeader &stHeader,
        QByteArray &arrPayload,
        int nTimeoutMs,
        const ReliableCancelCheck &fnCancelled,
        QString &strErrorMessage);

    /**
     * @brief ConnectAndHandshake
     * @param oSocket 当前线程拥有的 socket。
     * @param arrBuffer 当前连接的接收缓存，建立新连接前清空。
     * @param stMetadata 整个文件任务的元信息。
     * @param strTargetIp 接收端 IP。
     * @param nPort 接收端端口。
     * @param nWindowSize 每条数据连接的发送窗口。
     * @param arrMissingRanges 输出接收端缺失的分片范围。
     * @param nReceivedCount 输出接收端已收到的分片数量。
     * @param fnCancelled 取消检查函数。
     * @param strErrorMessage 输出失败原因。
     * @param nLaneIndex 通道编号，-1 表示控制连接。
     * @return 连接、握手及续传状态校验成功返回 true。
     * @details 关闭旧连接并清空缓存，以相同任务元信息重新握手；
     *          控制连接先创建接收会话，各数据连接再加入该会话。
     */
    static bool ConnectAndHandshake(
        QTcpSocket &oSocket,
        QByteArray &arrBuffer,
        const SReliableFileMetadata &stMetadata,
        const QString &strTargetIp,
        int nPort,
        int nWindowSize,
        QList<SReliablePacketRange> &arrMissingRanges,
        quint32 &nReceivedCount,
        const ReliableCancelCheck &fnCancelled,
        QString &strErrorMessage,
        int nLaneIndex = -1);
};

#endif // RELIABLETRANSFERIO_H
