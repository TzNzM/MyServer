#include "ReliableTcpServer.h"


ReliableTcpServer::ReliableTcpServer(QObject *pParent)
    : QTcpServer(pParent)
{

}

void ReliableTcpServer::incomingConnection(qintptr nSocketDescriptor)
{
    emit SignalConnectionAccepted(nSocketDescriptor);
}
