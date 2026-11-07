#include "TransferController.h"
#include "ReliableTransferManager.h"

TransferController::TransferController(QObject *pParent)
    : QObject(pParent),m_pReliableTransferManager(new ReliableTransferManager(this))
{
    //第一段：转发 Manager 的日志。
    connect(m_pReliableTransferManager,&ReliableTransferManager::SignalLogMessage,this,&TransferController::SignalLogMessage);
    //第二段：转发发送进度和最终结果。
    connect(
        m_pReliableTransferManager,
        &ReliableTransferManager::SignalSendProgress,
        this,
        &TransferController::SignalSendProgress);

    connect(
        m_pReliableTransferManager,
        &ReliableTransferManager::SignalSendFinished,
        this,
        &TransferController::SignalSendFinished);

    //第三段：转发接收进度和完成通知。
    connect(
        m_pReliableTransferManager,
        &ReliableTransferManager::SignalReceiveProgress,
        this,
        &TransferController::SignalReceiveProgress);

    connect(
        m_pReliableTransferManager,
        &ReliableTransferManager::SignalReceiveFinished,
        this,
                &TransferController::SignalReceiveFinished);
}

void TransferController::SetOutputDirectory(const QString &strOutputDirectory)
{
    m_pReliableTransferManager->SetOutputDirectory(strOutputDirectory);
}

bool TransferController::StartReceiver(const QString &strBindIp, int nPort, QString &strErrorMessage)
{
    return m_pReliableTransferManager->StartListening(strBindIp,nPort,strErrorMessage);
}

void TransferController::StopReceiver()
{
    m_pReliableTransferManager->StopListening();
}

bool TransferController::StartSend(const QString &strFilePath, const QString &strTargetIp, int nPort, int nThreadCount, int nWindowSize, int nSegmentSize, QString &strErrorMessage)
{
    return m_pReliableTransferManager->StartSend(
            strFilePath,
            strTargetIp,
            nPort,
            nThreadCount,
            nWindowSize,
            nSegmentSize,
            strErrorMessage);
}

void TransferController::StopSending()
{
    m_pReliableTransferManager->StopSending();
}



TransferController::~TransferController() = default;

