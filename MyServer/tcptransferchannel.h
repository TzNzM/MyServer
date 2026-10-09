#ifndef TCPTRANSFERCHANNEL_H
#define TCPTRANSFERCHANNEL_H

#include "abstracttransferchannel.h"

#include <QHash>
#include <QTcpServer>
#include <QTcpSocket>

class TcpTransferChannel : public AbstractTransferChannel
{
    Q_OBJECT

public:
    /**
     * @brief TcpTransferChannel
     * @param pParent Qt 父对象指针。
     * @return 无
     * @details 初始化 TCP 协议通道和服务端资源。
     */
    explicit TcpTransferChannel(QObject *pParent = nullptr);

    /**
     * @brief ~TcpTransferChannel
     * @param 无
     * @return 无
     * @details 释放 TCP 服务端和连接缓存资源。
     */
    ~TcpTransferChannel() override;

    /**
     * @brief StartListening
     * @param oBindAddress 需要绑定的本地地址。
     * @param nPort 需要绑定的端口号。
     * @param strErrorMessage 输出的错误描述。
     * @return 成功返回 true，否则返回 false。
     * @details 启动 TCP 服务端等待多个发送线程接入。
     */
    bool StartListening(const QHostAddress &oBindAddress, int nPort, QString &strErrorMessage) override;

    /**
     * @brief StopListening
     * @param 无
     * @return 无
     * @details 关闭 TCP 监听并断开所有活动连接。
     */
    void StopListening() override;

    /**
     * @brief SendPacket
     * @param oTargetAddress 目标 IP 地址。
     * @param nPort 目标端口号。
     * @param arrPacket 需要发送的完整数据包。
     * @param strErrorMessage 输出的错误描述。
     * @return 成功返回 true，否则返回 false。
     * @details 在发送线程内建立一个短连接并发送一个完整数据包。
     */
    bool SendPacket(const QHostAddress &oTargetAddress, int nPort, const QByteArray &arrPacket, QString &strErrorMessage) override;
private slots:
    /**
     * @brief OnNewConnection
     * @param 无
     * @return 无
     * @details 接收新的 TCP 客户端连接并挂接事件。
     */
    void OnNewConnection();

    /**
     * @brief OnSocketReadyRead
     * @param 无
     * @return 无
     * @details 从连接缓存中按固定包头规则拆出完整业务包。
     */
    void OnSocketReadyRead();

    /**
     * @brief OnSocketDisconnected
     * @param 无
     * @return 无
     * @details 清理断开的 TCP 连接以及对应缓存。
     */
    void OnSocketDisconnected();

    /**
     * @brief OnSocketError
     * @param eSocketError Socket 错误枚举。
     * @return 无
     * @details 输出当前 TCP 连接的运行错误信息。
     */
    void OnSocketError(QAbstractSocket::SocketError eSocketError);
private:
    QTcpServer* m_pTcpServer;
//    它是“每一个 TCP 连接各自的接收缓存”。
//    键和值分别是：
//    Key：   QTcpSocket*，某个已连接客户端
//    Value： QByteArray，该客户端尚未解析完的原始字节
    QHash<QTcpSocket*,QByteArray> m_mapReceiveBuffer;
};

#endif // TCPTRANSFERCHANNEL_H
