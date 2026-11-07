#include "ReliableReceiveSession.h"
#include "ReliablePacketCodec.h"
#include "ReliableTransferUtils.h"
#include "ReliableSha256.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <cstdlib>

static void Require(bool bCondition, const char *pMessage)
{
    if (!bCondition)
    {
        std::fprintf(stderr, "FAIL %s\n", pMessage);
        std::exit(1);
    }
}

int main(int argc, char **argv)
{
    QCoreApplication oApplication(argc, argv);
    QTemporaryDir oDirectory;
    Require(oDirectory.isValid(), "temp directory");
    SReliableFileMetadata stMetadata;
    stMetadata.m_oTransferId = QUuid::createUuid();
    stMetadata.m_strFileName = QStringLiteral("pipeline-cancel.bin");
    stMetadata.m_nSegmentSize = 4*1024*1024;
    stMetadata.m_nTotalPacketCount = 65;
    stMetadata.m_nFileSize = 64LL*stMetadata.m_nSegmentSize+317;
    stMetadata.m_nLaneCount = 3;
    QString strError;
    ReliableSha256 oExpected;
    Require(oExpected.Initialize(strError), "hash init");
    QByteArray arrBlock(stMetadata.m_nSegmentSize, 'p');
    for (int nIndex = 0; nIndex < 65; ++nIndex)
    {
        arrBlock.fill(static_cast<char>(nIndex));
        Require(oExpected.AddData(arrBlock.constData(), nIndex == 64 ? 317 : arrBlock.size(), strError), "hash block");
    }
    QByteArray arrExpected;
    Require(oExpected.Finish(arrExpected, strError), "hash finish");
    stMetadata.m_strSha256 = QString::fromLatin1(arrExpected.toHex());
    stMetadata.m_strSessionKey = ReliablePacketCodec::BuildSessionKey(stMetadata);
    QSharedPointer<QAtomicInt> pStop(new QAtomicInt(0));
    ReliableReceiveSession oSession(stMetadata, oDirectory.path(), pStop);
    Require(oSession.LoadOrCreateSession(strError), qPrintable(strError));
    for (int nLane = 0; nLane < 3; ++nLane)
    {
        const auto stRange = GetReliableLaneRange(stMetadata, nLane);
        for (qint64 nSequence = stRange.m_nFirstSequence; nSequence < stRange.m_nEndSequence; ++nSequence)
        {
            arrBlock.fill(static_cast<char>(nSequence));
            QByteArray arrPayload = nSequence == 64 ? arrBlock.left(317) : arrBlock;
            SReliablePacketHeader stHeader;
            stHeader.m_nMessageType = static_cast<quint16>(EReliableMessageType::DataPacket);
            stHeader.m_oTransferId = stMetadata.m_oTransferId;
            stHeader.m_nPacketSequence = nSequence;
            stHeader.m_nPayloadOffset = nSequence * stMetadata.m_nSegmentSize;
            stHeader.m_nPayloadSize = arrPayload.size();
            stHeader.m_nFileSize = stMetadata.m_nFileSize;
            stHeader.m_nTotalPacketCount = stMetadata.m_nTotalPacketCount;
            bool bAccepted = false, bNew = false;
            QString strWarning;
            Require(oSession.AcceptDataPacket(stHeader, arrPayload, nLane,
                bAccepted, bNew, strWarning, strError), qPrintable(strError));
            Require(bAccepted && bNew, "packet accepted");
        }
    }
    QScopedPointer<QThread> pCancel(QThread::create([pStop]() {
        QThread::msleep(35);
        pStop->storeRelease(1);
    }));
    pCancel->start();
    QList<SReliablePacketRange> arrMissing;
    QString strFinalPath, strTiming;
    Require(!oSession.VerifyAndFinalizeSession(arrMissing, strFinalPath, strError, &strTiming), "active merge cancelled");
    pCancel->wait();
    std::printf("CANCEL %s\n", qPrintable(strTiming));
    const auto oMatch = QRegularExpression(QStringLiteral("已合并=(\\d+)/")).match(strTiming);
    const qint64 nMerged = oMatch.captured(1).toLongLong();
    Require(nMerged > 0 && nMerged < stMetadata.m_nFileSize, "cancel happened during merge");
    Require(!QFileInfo::exists(strFinalPath), "cancel did not publish file");
    const QString strSessionDirectory = QDir(oDirectory.path()).filePath("session_" + stMetadata.m_strSessionKey);
    Require(QFileInfo::exists(QDir(strSessionDirectory).filePath("lane_0000.part")), "cancel retained lanes");
    Require(QFileInfo::exists(QDir(strSessionDirectory).filePath("state.json")), "cancel retained state");
    pStop->storeRelease(0);
    Require(oSession.VerifyAndFinalizeSession(arrMissing, strFinalPath, strError, &strTiming), qPrintable(strError));
    std::printf("RETRY %s\n", qPrintable(strTiming));
    QFile oResult(strFinalPath);
    Require(oResult.open(QIODevice::ReadOnly), "result open");
    ReliableSha256 oActual;
    Require(oActual.Initialize(strError), "verify init");
    while (!oResult.atEnd())
    {
        const auto arrData = oResult.read(4*1024*1024);
        Require(!arrData.isEmpty(), "result read");
        Require(oActual.AddData(arrData.constData(), arrData.size(), strError), "result hash");
    }
    QByteArray arrActual;
    Require(oActual.Finish(arrActual, strError) && arrActual == arrExpected, "whole-file content digest");
    Require(!QFileInfo::exists(strSessionDirectory), "success cleaned session");
    const QString strOriginal = strFinalPath;
    Require(oSession.VerifyAndFinalizeSession(arrMissing, strFinalPath, strError) && strFinalPath == strOriginal, "idempotent completion");
    std::puts("PASS active cancel, retained state, retry, digest, cleanup, repeated completion");
    return 0;
}
