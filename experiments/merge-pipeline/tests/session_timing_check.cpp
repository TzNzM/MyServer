#include "ReliableReceiveSession.h"
#include "ReliablePacketCodec.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cstdio>
#include <functional>
#include <stdexcept>

static void Check(bool bCondition, const char *pMessage)
{
    if (!bCondition)
    {
        throw std::runtime_error(pMessage);
    }
}

static QByteArray Read(const QString &strPath)
{
    QFile oFile(strPath);
    Check(oFile.open(QIODevice::ReadOnly), "read open failed");
    return oFile.readAll();
}

static void Write(const QString &strPath, const QByteArray &arrBytes)
{
    QFile oFile(strPath);
    Check(oFile.open(QIODevice::WriteOnly), "write open failed");
    Check(oFile.write(arrBytes) == arrBytes.size(), "write failed");
}

static QString Bitmap(const QBitArray &oMap)
{
    QByteArray arrBytes;
    QDataStream oStream(&arrBytes, QIODevice::WriteOnly);
    oStream.setVersion(QDataStream::Qt_5_12);
    oStream << oMap;
    return QString::fromLatin1(arrBytes.toBase64());
}

static SReliableFileMetadata Metadata()
{
    SReliableFileMetadata stMetadata;
    stMetadata.m_oTransferId = QUuid::createUuid();
    stMetadata.m_strFileName = QStringLiteral("sample.bin");
    stMetadata.m_nFileSize = 8192 * 10 + 317;
    stMetadata.m_nSegmentSize = 8192;
    stMetadata.m_nTotalPacketCount = 11;
    stMetadata.m_nLaneCount = 3;
    stMetadata.m_strSha256 = QString::fromLatin1(
        QCryptographicHash::hash("sample", QCryptographicHash::Sha256).toHex());
    stMetadata.m_strSessionKey = ReliablePacketCodec::BuildSessionKey(stMetadata);
    return stMetadata;
}

struct SFixture
{
    QTemporaryDir m_oDirectory;
    SReliableFileMetadata m_stMetadata = Metadata();
    QSharedPointer<QAtomicInt> m_pStopFlag{new QAtomicInt(0)};
    QString m_strSessionDirectory;
    QString m_strStatePath;

    SFixture()
    {
        Check(m_oDirectory.isValid(), "temporary directory failed");
        m_strSessionDirectory = QDir(m_oDirectory.path()).filePath(
            QStringLiteral("session_%1").arg(m_stMetadata.m_strSessionKey));
        m_strStatePath = QDir(m_strSessionDirectory).filePath("state.json");
        ReliableReceiveSession oSession(m_stMetadata, m_oDirectory.path(), m_pStopFlag);
        QString strError;
        Check(oSession.LoadOrCreateSession(strError), qPrintable(strError));
    }

    QString Lane(int nIndex) const
    {
        return QDir(m_strSessionDirectory).filePath(
            QStringLiteral("lane_%1.part").arg(nIndex, 4, 10, QChar('0')));
    }

    QJsonObject State() const
    {
        return QJsonDocument::fromJson(Read(m_strStatePath)).object();
    }

    void SetState(const QJsonObject &oJson)
    {
        Write(m_strStatePath, QJsonDocument(oJson).toJson(QJsonDocument::Compact));
    }
};

static void Rejected(const std::function<void(SFixture &)> &fnMutate)
{
    SFixture oFixture;
    fnMutate(oFixture);
    const QByteArray arrOldState = Read(oFixture.m_strStatePath);
    QList<QByteArray> arrOldLanes;
    QList<bool> arrExisted;
    for (int nIndex = 0; nIndex < 3; ++nIndex)
    {
        const bool bExists = QFileInfo::exists(oFixture.Lane(nIndex));
        arrExisted.append(bExists);
        arrOldLanes.append(bExists ? Read(oFixture.Lane(nIndex)) : QByteArray());
    }
    ReliableReceiveSession oSession(oFixture.m_stMetadata,
        oFixture.m_oDirectory.path(), oFixture.m_pStopFlag);
    QString strError;
    Check(!oSession.LoadOrCreateSession(strError), "invalid state unexpectedly accepted");
    Check(!strError.isEmpty(), "missing failure reason");
    Check(Read(oFixture.m_strStatePath) == arrOldState, "rejected state was overwritten");
    for (int nIndex = 0; nIndex < 3; ++nIndex)
    {
        Check(QFileInfo::exists(oFixture.Lane(nIndex)) == arrExisted.at(nIndex),
            "missing lane was recreated");
        if (arrExisted.at(nIndex))
        {
            Check(Read(oFixture.Lane(nIndex)) == arrOldLanes.at(nIndex),
                "rejected lane was changed");
        }
    }
}

//通过实际接收、局部偏移写入和最终合并，检查三个调用位置使用一致的通道划分。
static void Transfer(const QList<int> &arrLaneEnds, int nLastPacketSize)
{
    QTemporaryDir oDirectory;
    Check(oDirectory.isValid(), "temporary directory failed");
    SReliableFileMetadata stMetadata = Metadata();
    stMetadata.m_nTotalPacketCount = static_cast<quint32>(arrLaneEnds.last());
    stMetadata.m_nLaneCount = arrLaneEnds.size();
    stMetadata.m_nFileSize =
        (arrLaneEnds.last() - 1) * 8192 + nLastPacketSize;
    QByteArray arrSource(static_cast<int>(stMetadata.m_nFileSize), '\0');
    for (int nIndex = 0; nIndex < arrSource.size(); ++nIndex)
    {
        arrSource[nIndex] = static_cast<char>((nIndex * 37 + nIndex / 8192) % 251);
    }
    stMetadata.m_strSha256 = QString::fromLatin1(
        QCryptographicHash::hash(arrSource, QCryptographicHash::Sha256).toHex());
    stMetadata.m_strSessionKey = ReliablePacketCodec::BuildSessionKey(stMetadata);
    QSharedPointer<QAtomicInt> pStopFlag(new QAtomicInt(0));
    ReliableReceiveSession oSession(stMetadata, oDirectory.path(), pStopFlag);
    QString strError;
    Check(oSession.LoadOrCreateSession(strError), qPrintable(strError));
    const QString strSessionDirectory = QDir(oDirectory.path()).filePath(
        QStringLiteral("session_%1").arg(stMetadata.m_strSessionKey));
    int nFirst = 0;
    for (int nLane = 0; nLane < arrLaneEnds.size(); ++nLane)
    {
        const QString strLane = QDir(strSessionDirectory).filePath(
            QStringLiteral("lane_%1.part").arg(nLane, 4, 10, QChar('0')));
        const int nEnd = arrLaneEnds.at(nLane);
        const int nExpectedSize = (nEnd - nFirst) * 8192 -
            (nLane == arrLaneEnds.size() - 1 ? 8192 - nLastPacketSize : 0);
        Check(QFileInfo(strLane).size() == nExpectedSize, "wrong allocated lane size");
        nFirst = nEnd;
    }
    QList<SReliablePacketRange> arrMissing;
    QString strFinalPath;
    QString strTiming;
    Check(!oSession.VerifyAndFinalizeSession(arrMissing, strFinalPath, strError, &strTiming),
        "incomplete transfer finalized" );
    Check(strTiming.contains(QStringLiteral("仍有缺片，未合并")), "missing-packet timing absent" );
    Check(arrMissing.size() == 1 && arrMissing.first().m_nStartSequence == 0 &&
        arrMissing.first().m_nEndSequence == stMetadata.m_nTotalPacketCount - 1,
        "wrong initial missing range");
    quint32 nAccepted = 0;
    for (int nLane = arrLaneEnds.size() - 1; nLane >= 0; --nLane)
    {
        const int nStart = nLane == 0 ? 0 : arrLaneEnds.at(nLane - 1);
        for (int nSequence = arrLaneEnds.at(nLane) - 1; nSequence >= nStart; --nSequence)
        {
            SReliablePacketHeader stHeader;
            stHeader.m_nMessageType = static_cast<quint16>(EReliableMessageType::DataPacket);
            stHeader.m_oTransferId = stMetadata.m_oTransferId;
            stHeader.m_nPacketSequence = static_cast<quint64>(nSequence);
            stHeader.m_nPayloadOffset = static_cast<quint64>(nSequence) * 8192;
            const QByteArray arrPayload = arrSource.mid(nSequence * 8192, 8192);
            stHeader.m_nPayloadSize = static_cast<quint32>(arrPayload.size());
            stHeader.m_nFileSize = static_cast<quint64>(stMetadata.m_nFileSize);
            stHeader.m_nTotalPacketCount = stMetadata.m_nTotalPacketCount;
            bool bAccepted = false;
            bool bNewPacket = false;
            QString strWarning;
            if (nAccepted == 0 && arrLaneEnds.size() > 1)
            {
                Check(!oSession.AcceptDataPacket(stHeader, arrPayload, 0,
                    bAccepted, bNewPacket, strWarning, strError), "wrong lane accepted");
                Check(!bAccepted && !bNewPacket && oSession.GetReceivedPacketCount() == 0,
                    "rejected lane changed progress");
            }
            Check(oSession.AcceptDataPacket(stHeader, arrPayload, nLane,
                bAccepted, bNewPacket, strWarning, strError), qPrintable(strError));
            Check(bAccepted && bNewPacket && strWarning.isEmpty(), "new packet flags wrong");
            Check(oSession.GetReceivedPacketCount() == ++nAccepted, "wrong progress");
            Check(oSession.AcceptDataPacket(stHeader, arrPayload, nLane,
                bAccepted, bNewPacket, strWarning, strError), "duplicate rejected");
            Check(bAccepted && !bNewPacket && oSession.GetReceivedPacketCount() == nAccepted,
                "duplicate changed progress");
        }
    }
    pStopFlag->storeRelease(1);
    Check(!oSession.VerifyAndFinalizeSession(arrMissing, strFinalPath, strError, &strTiming), "stopped merge unexpectedly succeeded");
    Check(strTiming.contains(QStringLiteral("失败或取消")), "cancel timing absent");
    Check(QFileInfo::exists(strSessionDirectory), "cancel removed resumable files");
    pStopFlag->storeRelease(0);
    Check(oSession.VerifyAndFinalizeSession(arrMissing, strFinalPath, strError, &strTiming), qPrintable(strError));
    Check(strTiming.contains(QStringLiteral("结果=完成，")), "success timing absent" );
    Check(arrMissing.isEmpty() && Read(strFinalPath) == arrSource, "merged contents differ");
    const QString strOriginalFinalPath = strFinalPath;
    Check(oSession.VerifyAndFinalizeSession(arrMissing, strFinalPath, strError, &strTiming), "repeat completion failed");
    Check(strTiming.contains(QStringLiteral("已完成，跳过合并")), "repeat-completion timing absent" );
    Check(strFinalPath == strOriginalFinalPath && Read(strFinalPath) == arrSource,
        "repeat completion changed result");
    Check(!QFileInfo::exists(strSessionDirectory), "session cleanup incomplete");
}

int main(int argc, char **argv)
{
    QCoreApplication oApplication(argc, argv);
    int nPassed = 0;
    int nFailed = 0;
    auto Run = [&](const char *pName, const std::function<void()> &fnTest)
    {
        try
        {
            fnTest();
            ++nPassed;
            std::fprintf(stdout, "PASS %s\n", pName);
        }
        catch (const std::exception &oError)
        {
            ++nFailed;
            std::fprintf(stdout, "FAIL %s: %s\n", pName, oError.what());
        }
    };

    Run("fresh session: empty bitmap and exact lane sizes", []
    {
        SFixture oFixture;
        const QJsonObject oState = oFixture.State();
        Check(oState.value("receivedPacketCount").toInt() == 0, "nonzero initial count");
        Check(oState.value("receivedPacketBitmap").toString() == Bitmap(QBitArray(11, false)),
            "nonempty initial bitmap");
        const qint64 arrSizes[] = {3 * 8192, 4 * 8192, 3 * 8192 + 317};
        for (int nIndex = 0; nIndex < 3; ++nIndex)
        {
            Check(QFileInfo(oFixture.Lane(nIndex)).size() == arrSizes[nIndex], "wrong lane size");
        }
        Check(!QFileInfo::exists(QDir(oFixture.m_strSessionDirectory).filePath("assembled.part")),
            "merge file created too early");
    });

    Run("resume: restore bitmap, recount, preserve lanes, rebuild final path", []
    {
        SFixture oFixture;
        QJsonObject oState = oFixture.State();
        QBitArray oMap(11, false);
        oMap.setBit(0); oMap.setBit(3); oMap.setBit(10);
        oState.insert("receivedPacketBitmap", Bitmap(oMap));
        oState.insert("receivedPacketCount", 999);
        const QString strForeignPath = QDir(oFixture.m_oDirectory.path()).filePath("foreign.bin");
        oState.insert("finalFilePath", strForeignPath);
        oFixture.SetState(oState);
        QList<QByteArray> arrOriginal;
        for (int nIndex = 0; nIndex < 3; ++nIndex)
        {
            QByteArray arrLane = Read(oFixture.Lane(nIndex));
            arrLane[0] = static_cast<char>('a' + nIndex);
            Write(oFixture.Lane(nIndex), arrLane);
            arrOriginal.append(arrLane);
        }
        Write(QDir(oFixture.m_oDirectory.path()).filePath("sample.bin"), "existing file");
        oFixture.m_stMetadata.m_oTransferId = QUuid::createUuid();
        ReliableReceiveSession oSession(oFixture.m_stMetadata,
            oFixture.m_oDirectory.path(), oFixture.m_pStopFlag);
        QString strError;
        Check(oSession.LoadOrCreateSession(strError), qPrintable(strError));
        const QJsonObject oRecovered = oFixture.State();
        Check(oRecovered.value("receivedPacketBitmap").toString() == Bitmap(oMap), "bitmap not restored");
        Check(oRecovered.value("receivedPacketCount").toInt() == 3, "count not recomputed");
        Check(oRecovered.value("finalFilePath").toString() ==
            QDir(oFixture.m_oDirectory.path()).filePath("sample_1.bin"), "wrong final path");
        Check(!QFileInfo::exists(strForeignPath), "untrusted path created");
        for (int nIndex = 0; nIndex < 3; ++nIndex)
        {
            Check(Read(oFixture.Lane(nIndex)) == arrOriginal.at(nIndex), "lane changed on resume");
        }
        Check(oSession.PersistSessionState(strError), "public persist failed");
    });

    Run("reject malformed JSON", [] { Rejected([](SFixture &f) { Write(f.m_strStatePath, "{"); }); });
    Run("reject mismatched lane count", [] { Rejected([](SFixture &f)
    { auto j = f.State(); j.insert("laneCount", 2); f.SetState(j); }); });
    Run("reject old state version", [] { Rejected([](SFixture &f)
    { auto j = f.State(); j.insert("stateVersion", 2); f.SetState(j); }); });
    Run("reject mismatched file size", [] { Rejected([](SFixture &f)
    { auto j = f.State(); j.insert("fileSize", "1"); f.SetState(j); }); });
    Run("reject huge declared bitmap length before allocation", [] { Rejected([](SFixture &f)
    { auto j = f.State(); j.insert("receivedPacketBitmap", QString::fromLatin1(QByteArray::fromHex("ffffffff").toBase64())); f.SetState(j); }); });
    Run("reject truncated bitmap", [] { Rejected([](SFixture &f)
    { auto j = f.State(); j.insert("receivedPacketBitmap", QString::fromLatin1(QByteArray::fromHex("0000000b").toBase64())); f.SetState(j); }); });
    Run("reject noncanonical base64", [] { Rejected([](SFixture &f)
    { auto j = f.State(); j.insert("receivedPacketBitmap", j.value("receivedPacketBitmap").toString() + "!"); f.SetState(j); }); });
    Run("reject trailing bitmap bytes", [] { Rejected([](SFixture &f)
    { auto j = f.State(); QByteArray a = QByteArray::fromBase64(j.value("receivedPacketBitmap").toString().toLatin1()); a.append('x'); j.insert("receivedPacketBitmap", QString::fromLatin1(a.toBase64())); f.SetState(j); }); });
    Run("reject missing lane without recreation", [] { Rejected([](SFixture &f)
    { Check(QFile::remove(f.Lane(1)), "remove fixture lane failed"); }); });
    Run("reject truncated lane without resizing", [] { Rejected([](SFixture &f)
    { QFile oFile(f.Lane(1)); Check(oFile.open(QIODevice::ReadWrite) && oFile.resize(7), "truncate fixture lane failed"); }); });

    Run("final SHA mismatch does not publish or remove resumable files", []
    {
        SFixture oFixture;
        auto oState = oFixture.State();
        oState.insert("receivedPacketBitmap", Bitmap(QBitArray(11, true)));
        oState.insert("receivedPacketCount", 11);
        oFixture.SetState(oState);
        ReliableReceiveSession oSession(oFixture.m_stMetadata,
            oFixture.m_oDirectory.path(), oFixture.m_pStopFlag);
        QString strError;
        Check(oSession.LoadOrCreateSession(strError), qPrintable(strError));
        QList<SReliablePacketRange> arrMissing;
        QString strFinalPath;
        Check(!oSession.VerifyAndFinalizeSession(arrMissing, strFinalPath, strError), "bad digest accepted");
        Check(arrMissing.isEmpty() && strError.contains("SHA-256"), "wrong failure reason");
        Check(!QFileInfo::exists(QDir(oFixture.m_oDirectory.path()).filePath("sample.bin")), "bad file published");
        Check(QFileInfo::exists(oFixture.m_strStatePath), "state removed on failure");
        for (int nIndex = 0; nIndex < 3; ++nIndex)
            Check(QFileInfo::exists(oFixture.Lane(nIndex)), "lane removed on failure");
    });

    Run("transfer: 11 packets, 3 uneven lanes, short last packet", [] { Transfer({3, 7, 11}, 317); });
    Run("transfer: 10 packets, 3 uneven lanes, full last packet", [] { Transfer({3, 6, 10}, 8192); });
    Run("transfer: one packet per lane", [] { Transfer({1, 2, 3, 4}, 23); });
    Run("transfer: one short packet and one lane", [] { Transfer({1}, 1); });
    Run("transfer: one lane larger than merge buffer", [] { Transfer({514}, 317); });

    std::fprintf(stdout, "%d passed, %d failed\n", nPassed, nFailed);
    return nFailed == 0 ? 0 : 1;
}

