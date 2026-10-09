#include "tcptransferchannel.h"

TcpTransferChannel::TcpTransferChannel(QObject *pParent)
    : AbstractTransferChannel(QStringLiteral("TCP"),pParent),
      m_pTcpServer(new QTcpServer(this)),//this 指向“当前正在创建的 TcpTransferChannel 对象”,可以辅助析构
      m_mapReceiveBuffer()
{
    connect(m_pTcpServer,&QTcpServer::newConnection,this,&TcpTransferChannel::OnNewConnection);//如果有新连接到达，跳转过去
}

TcpTransferChannel::~TcpTransferChannel()
{
    StopListening();
}

bool TcpTransferChannel::StartListening(const QHostAddress &oBindAddress, int nPort, QString &strErrorMessage)
{
    StopListening();//防止误操作的，始终只监听一个端口

    if (!m_pTcpServer->listen(oBindAddress,static_cast<quint16>(nPort))){//如果不能正常启动监听
        strErrorMessage = m_pTcpServer->errorString();
        return false;
    }

    emit SignalLogMessage(QStringLiteral("[TCP] 已开始监听 %1:%2").arg(m_pTcpServer->serverAddress().toString(),m_pTcpServer->serverPort()));
    return true;
}

void TcpTransferChannel::StopListening()
{
    const QList<QTcpSocket*> arrSockets = m_mapReceiveBuffer.keys();
    //断开所有连接
    for(QTcpSocket* pSocket : arrSockets){
        if(pSocket != nullptr){
            pSocket->disconnect(this);
            pSocket->close();
            pSocket->deleteLater();
        }
    }
    //清空所有缓存
    m_mapReceiveBuffer.clear();
    //关闭所有监听
    if(m_pTcpServer->isListening()){
        m_pTcpServer->close();
    }

}

//这里都是短连接的写法
bool TcpTransferChannel::SendPacket(const QHostAddress &oTargetAddress, int nPort, const QByteArray &arrPacket, QString &strErrorMessage)
{
    QTcpSocket oTcpSocket;

    oTcpSocket.connectToHost(oTargetAddress,static_cast<quint16>(nPort));
    if(!oTcpSocket.waitForConnected(5000)){
        strErrorMessage = oTcpSocket.errorString();
        return false;
    }

    if(oTcpSocket.write(arrPacket) != arrPacket.size()){
        strErrorMessage =  QStringLiteral("TCP 写入长度不完整。");
        return false;
    }

    while (oTcpSocket.bytesToWrite() > 0) //确保 Qt 的发送缓冲区已经写空，数据已交给操作系统网络栈继续发送。
    {
        if (!oTcpSocket.waitForBytesWritten(5000)) //阻塞式循环等待
        {
            strErrorMessage = oTcpSocket.errorString();
            return false;
        }
    }

    oTcpSocket.disconnectFromHost();
    oTcpSocket.waitForDisconnected(1000);
    return true;
}
