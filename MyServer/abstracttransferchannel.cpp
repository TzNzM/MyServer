#include "abstracttransferchannel.h"


AbstractTransferChannel::AbstractTransferChannel(const QString &strChannelName, QObject *pParent)
    :QObject(pParent),m_strChannelName(strChannelName)
{

}

AbstractTransferChannel::~AbstractTransferChannel()
{

}

QString AbstractTransferChannel::GetChannelName() const
{
    return m_strChannelName;
}
