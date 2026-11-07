#include "ReliableMergeHasher.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QScopedPointer>
#include <cstdio>
#include <cstring>

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
    int nPassed = 0;
    const qint64 arrSizes[] = {0, 1, 63, 64, 65, 4*1024*1024-1,
        4*1024*1024, 4*1024*1024+37, 53*1024*1024+137};
    for (qint64 nSize : arrSizes)
    {
        QSharedPointer<QAtomicInt> pStop(new QAtomicInt(0));
        ReliableMergeHasher oHasher(pStop);
        QCryptographicHash oExpected(QCryptographicHash::Sha256);
        QString strError;
        qint64 nOffset = 0;
        int nBlock = 0;
        while (nOffset < nSize)
        {
            int nIndex = -1;
            Require(oHasher.AcquireBuffer(nIndex, strError), "acquire");
            QByteArray &arrBuffer = oHasher.GetBuffer(nIndex);
            const int nCount = static_cast<int>(qMin<qint64>(
                nBlock % 2 ? 1024*1024+129 : arrBuffer.size(), nSize-nOffset));
            std::memset(arrBuffer.data(), (nBlock*73+19)%256, nCount);
            oExpected.addData(arrBuffer.constData(), nCount);
            Require(oHasher.SubmitBuffer(nIndex, nCount, strError), "submit");
            nOffset += nCount;
            ++nBlock;
        }
        QByteArray arrDigest;
        Require(oHasher.Finish(arrDigest, strError), "finish");
        Require(arrDigest == oExpected.result(), "standard SHA-256 and FIFO reuse");
        Require(oHasher.GetHashedByteCount() == nSize, "hashed byte count");
        ++nPassed;
    }
    {
        QSharedPointer<QAtomicInt> pStop(new QAtomicInt(0));
        ReliableMergeHasher oHasher(pStop);
        QString strError;
        for (int nSlot = 0; nSlot < 3; ++nSlot)
        {
            int nIndex = -1;
            Require(oHasher.AcquireBuffer(nIndex, strError), "hold all three buffers");
        }
        QAtomicInt nResult(0);
        QScopedPointer<QThread> pWaiter(QThread::create([&]() {
            int nIndex = -1;
            QString strWaitError;
            nResult.storeRelease(!oHasher.AcquireBuffer(nIndex, strWaitError)
                && !strWaitError.isEmpty() ? 1 : -1);
        }));
        pWaiter->start();
        QThread::msleep(60);
        Require(nResult.loadAcquire() == 0, "fourth acquisition waits for bounded capacity");
        pStop->storeRelease(1);
        Require(pWaiter->wait(3000), "stop wakes blocked producer");
        Require(nResult.loadAcquire() == 1, "cancel reason returned");
        oHasher.CancelAndWait();
        ++nPassed;
    }
    {
        QSharedPointer<QAtomicInt> pStop(new QAtomicInt(0));
        ReliableMergeHasher oHasher(pStop);
        QString strError;
        int nIndex = -1;
        Require(oHasher.AcquireBuffer(nIndex, strError), "acquire before cancel");
        std::memset(oHasher.GetBuffer(nIndex).data(), 'x', 4*1024*1024);
        Require(oHasher.SubmitBuffer(nIndex, 4*1024*1024, strError), "submit before cancel");
        pStop->storeRelease(1);
        QByteArray arrDigest;
        Require(!oHasher.Finish(arrDigest, strError), "cancel never reports successful digest");
        Require(!strError.isEmpty(), "cancel error");
        ++nPassed;
    }
    {
        QElapsedTimer oClock;
        oClock.start();
        {
            QSharedPointer<QAtomicInt> pStop(new QAtomicInt(0));
            ReliableMergeHasher oHasher(pStop);
            QString strError;
            int nIndex = -1;
            Require(oHasher.AcquireBuffer(nIndex, strError), "acquire before early return");
        }
        Require(oClock.elapsed() < 3000, "destructor cancels waiting consumer and joins");
        ++nPassed;
    }
    std::printf("PASS %d/12 pipeline checks\n", nPassed);
    return nPassed == 12 ? 0 : 1;
}
