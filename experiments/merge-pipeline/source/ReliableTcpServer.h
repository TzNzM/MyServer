#ifndef RELIABLETCPSERVER_H
#define RELIABLETCPSERVER_H

#include <QTcpServer>
class ReliableTcpServer : public QTcpServer
{
    Q_OBJECT

public:
    /**
     * @brief ReliableTcpServer
     * @param pParent Qt 父对象指针。
     * @return 无
     * @details 创建监听对象；新连接交给后台接收线程处理。
     */
    explicit ReliableTcpServer(QObject *pParent = nullptr);

signals:
    /**
     * @brief SignalConnectionAccepted
     * @param nSocketDescriptor 已接受连接的原生套接字描述符。
     * @return 无
     * @details 通知 Manager 分配接收线程，
     *          再由该线程中的 Worker 创建 QTcpSocket 接管连接。
     */
    void SignalConnectionAccepted(qintptr nSocketDescriptor);

protected:
    /**
     * @brief incomingConnection
     * @param nSocketDescriptor 已接受连接的原生套接字描述符。
     * @return 无
     * @details 重写 QTcpServer 的连接接入处理，
     *          不在监听线程中创建待交给其他线程的 QTcpSocket。
     */
    void incomingConnection(qintptr nSocketDescriptor) override;
};

#endif // RELIABLETCPSERVER_H
