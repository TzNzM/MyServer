#ifndef ABSTRACTTRANSFERCHANNEL_H
#define ABSTRACTTRANSFERCHANNEL_H

#include <QObject>
#include <QHostAddress>

class AbstractTransferChannel : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief AbstractTransferChannel
     * @param strChannelName 当前通道名称。
     * @param pParent Qt 父对象指针。
     * @return 无
     * @details 初始化传输通道基类，统一输出日志名称。
     */
    explicit AbstractTransferChannel(const QString &strChannelName,QObject *pParent = nullptr);
    /**
     * @brief ~AbstractTransferChannel
     * @param 无
     * @return 无
     * @details 提供多态析构，保证子类资源正确释放。
     */
    virtual ~AbstractTransferChannel();

    /**
     * @brief StartListening
     * @param oBindAddress 需要绑定的本地地址。
     * @param nPort 需要绑定的端口号。
     * @param strErrorMessage 输出的错误描述。
     * @return 成功返回 true，否则返回 false。
     * @details 启动接收端监听，由子类决定具体协议实现。
     */
    virtual bool StartListening(const QHostAddress &oBindAddress, int nPort, QString &strErrorMessage) = 0;

    /**
     * @brief StopListening
     * @param 无
     * @return 无
     * @details 停止接收端监听，释放协议层资源。
     */
    virtual void StopListening() = 0;

    /**
     * @brief SendPacket
     * @param oTargetAddress 目标 IP 地址。
     * @param nPort 目标端口号。
     * @param arrPacket 需要发送的完整数据包。
     * @param strErrorMessage 输出的错误描述。
     * @return 成功返回 true，否则返回 false。
     * @details 发送一个已经编码好的完整网络包。
     */
    virtual bool SendPacket(const QHostAddress &oTargetAddress, int nPort, const QByteArray &arrPacket, QString &strErrorMessage) = 0;

    /**
     * @brief GetChannelName
     * @param 无
     * @return 当前传输通道名称。
     * @details 用于统一输出 TCP/UDP 协议相关日志。
     */
    QString GetChannelName() const; //const 函数承诺不修改当前对象的成员状态。

signals:
    void SignalPacketReceived(const QByteArray &arrPacket);
    void SignalLogMessage(const QString &strMessage);
protected:
    QString m_strChannelName;

};

#endif // ABSTRACTTRANSFERCHANNEL_H
