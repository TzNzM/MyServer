#include "ReliableSha256.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <stdexcept>

static void Check(bool bCondition, const QString &strMessage)
{
    if (!bCondition)
        throw std::runtime_error(strMessage.toStdString());
}

static QByteArray Calculate(const QByteArray &arrData, int nChunkSize)
{
    ReliableSha256 oHash;
    QString strError;
    Check(oHash.Initialize(strError), strError);
    for (int nOffset = 0; nOffset < arrData.size(); nOffset += nChunkSize)
        Check(oHash.AddData(arrData.constData() + nOffset,
            qMin(nChunkSize, arrData.size() - nOffset), strError), strError);
    QByteArray arrDigest;
    Check(oHash.Finish(arrDigest, strError), strError);
    return arrDigest;
}

int main(int argc, char **argv)
{
    QCoreApplication oApp(argc, argv);
    try
    {
        Check(Calculate({}, 1).toHex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty vector failed");
        Check(Calculate("abc", 1).toHex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc vector failed");
        int nCases = 2;
        for (int nSize : {1, 55, 56, 63, 64, 65, 4095, 4096, 1024 * 1024 + 17, 5 * 1024 * 1024 + 37})
        {
            QByteArray arrData(nSize, '\0');
            for (int nIndex = 0; nIndex < nSize; ++nIndex)
                arrData[nIndex] = static_cast<char>((nIndex * 31 + nIndex / 8192) % 251);
            const QByteArray arrExpected = QCryptographicHash::hash(arrData, QCryptographicHash::Sha256);
            for (int nChunk : {1, 63, 4096, 1024 * 1024})
            {
                Check(Calculate(arrData, nChunk) == arrExpected, "chunked digest differs from Qt");
                ++nCases;
            }
        }
        ReliableSha256 oState;
        QString strError;
        QByteArray arrDigest;
        Check(!oState.AddData("a", 1, strError), "uninitialized update accepted");
        Check(!oState.Finish(arrDigest, strError), "uninitialized finish accepted");
        Check(oState.Initialize(strError), strError);
        Check(!oState.AddData(nullptr, 1, strError), "null nonempty input accepted");
        Check(!oState.AddData("a", -1, strError), "negative size accepted");
        Check(oState.AddData(nullptr, 0, strError), "empty update failed");
        Check(oState.AddData("abc", 3, strError), strError);
        Check(oState.Finish(arrDigest, strError), strError);
        const QByteArray arrFirst = arrDigest;
        Check(oState.Finish(arrDigest, strError) && arrDigest == arrFirst, "finish cache failed");
        Check(!oState.AddData("a", 1, strError), "update after finish accepted");
        Check(oState.Initialize(strError) && oState.Finish(arrDigest, strError), "reset failed");
        Check(arrDigest == Calculate({}, 1), "reset did not start empty digest");

        const QJsonObject oResult{{"digest_cases", nCases}, {"state_checks_passed", true}};
        QTextStream(stdout) << QJsonDocument(oResult).toJson();
        return 0;
    }
    catch (const std::exception &oError)
    {
        QTextStream(stderr) << oError.what() << Qt::endl;
        return 1;
    }
}
