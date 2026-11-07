#include "mainwidget.h"
#include "TransferController.h"
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpServer>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <stdexcept>

// Windows Qt 5.14 QMessageBox::showEvent accesses a native system menu.
// The offscreen plugin has no such native window; skip that Show handler
// while preserving the application's completion slots and modal event loop.
class OffscreenDialogFilter : public QObject
{
    bool eventFilter(QObject *pObject, QEvent *pEvent) override
    {
        return pEvent->type() == QEvent::Show
            && qobject_cast<QMessageBox *>(pObject) != nullptr;
    }
};

static void Require(bool bCondition, const char *pMessage)
{
    if (!bCondition)
        throw std::runtime_error(pMessage);
}

static QByteArray HashFile(const QString &strPath)
{
    QFile oFile(strPath);
    Require(oFile.open(QIODevice::ReadOnly), "cannot open file for hash");
    QCryptographicHash oHash(QCryptographicHash::Sha256);
    Require(oHash.addData(&oFile), "hash read failed");
    return oHash.result();
}

int main(int argc, char **argv)
{
    QApplication oApp(argc, argv);
    oApp.setQuitOnLastWindowClosed(false);
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    OffscreenDialogFilter oDialogFilter;
    oApp.installEventFilter(&oDialogFilter);
    QTimer oCloseDialogs;
    QObject::connect(&oCloseDialogs, &QTimer::timeout, []() {
        for (QWidget *pWidget : QApplication::topLevelWidgets())
            if (auto pDialog = qobject_cast<QMessageBox *>(pWidget))
                pDialog->accept();
    });
    oCloseDialogs.start(10);
    try
    {
        Require(argc == 2, "expected isolated test directory");
        const QString strRoot = QDir(argv[1]).absolutePath();
        Require(QDir().mkpath(strRoot + "/received"), "cannot create test directory");
        MainWidget oWidget;
        const auto arrEdits = oWidget.findChildren<QLineEdit *>(QString(), Qt::FindDirectChildrenOnly);
        const auto arrSpins = oWidget.findChildren<QSpinBox *>();
        // Layout insertion reparents line edits, so locate the four outer text fields by type.
        QList<QLineEdit *> arrTextFields;
        for (QLineEdit *pEdit : oWidget.findChildren<QLineEdit *>())
            if (!qobject_cast<QSpinBox *>(pEdit->parent()))
                arrTextFields.append(pEdit);
        Q_UNUSED(arrEdits);
        Require(arrTextFields.size() == 4 && arrSpins.size() == 5, "unexpected UI controls");
        auto pController = oWidget.findChild<TransferController *>();
        auto pProgress = oWidget.findChild<QProgressBar *>();
        Require(pController && pProgress, "controller or progress missing");
        QPushButton *pSendButton = nullptr;
        for (QPushButton *pButton : oWidget.findChildren<QPushButton *>())
            if (pButton->text() == QStringLiteral("发送文件"))
                pSendButton = pButton;
        Require(pSendButton, "send button missing");
        QTcpServer oPortProbe;
        Require(oPortProbe.listen(QHostAddress::LocalHost, 0), "cannot reserve port");
        const int nPort = oPortProbe.serverPort();
        oPortProbe.close();
        arrTextFields[0]->setText("127.0.0.1");
        arrTextFields[1]->setText("127.0.0.1");
        arrTextFields[3]->setText(strRoot + "/received");
        arrSpins[0]->setValue(nPort);
        arrSpins[1]->setValue(nPort);
        Require(QMetaObject::invokeMethod(&oWidget, "OnStartReceiverClicked"), "start slot missing");
        Require(!arrSpins[0]->isEnabled(), "listener did not start");
        QStringList arrLogs;
        QObject::connect(pController, &TransferController::SignalLogMessage, &oApp,
            [&](const QString &strMessage) { arrLogs.append(strMessage); });
        bool bDone = false;
        bool bSuccess = false;
        QString strReceivedPath;
        QObject::connect(pController, &TransferController::SignalSendFinished, &oApp,
            [&](bool bOk, const QString &strMessage) {
                bDone = true;
                bSuccess = bOk;
                QTextStream(stdout) << "SEND_RESULT " << bOk << " " << strMessage << Qt::endl;
            });
        QObject::connect(pController, &TransferController::SignalReceiveFinished, &oApp,
            [&](const QString &strPath) { strReceivedPath = strPath; });
        const QList<int> arrSizes = {5 * 1024 * 1024 + 37, 37};
        for (int nCase = 0; nCase < arrSizes.size(); ++nCase)
        {
            const QString strSource = strRoot + QString("/input_%1.bin").arg(nCase);
            QFile oSource(strSource);
            Require(oSource.open(QIODevice::WriteOnly), "cannot create source");
            QByteArray arrData(arrSizes[nCase], '\0');
            for (int nIndex = 0; nIndex < arrData.size(); ++nIndex)
                arrData[nIndex] = static_cast<char>((nIndex * 31 + nIndex / 8192) % 251);
            Require(oSource.write(arrData) == arrData.size(), "source write failed");
            oSource.close();
            arrTextFields[2]->setText(strSource);
            arrSpins[2]->setValue(4);
            arrSpins[3]->setValue(nCase == 0 ? 1 : 16);
            arrSpins[4]->setValue(60);
            oWidget.findChild<QComboBox *>()->setCurrentIndex(0);
            bDone = false;
            bSuccess = false;
            strReceivedPath.clear();
            Require(QMetaObject::invokeMethod(&oWidget, "OnSendFileClicked"), "send slot missing");
            Require(!pSendButton->isEnabled(), "send task not started");
            QElapsedTimer oTimer;
            oTimer.start();
            while ((!bDone || strReceivedPath.isEmpty()) && oTimer.elapsed() < 30000)
            {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(1);
            }
            Require(bDone && bSuccess && !strReceivedPath.isEmpty(), "transfer failed or timed out");
            Require(HashFile(strSource) == HashFile(strReceivedPath), "source and output hash differ");
            Require(pSendButton->isEnabled(), "send button not restored");
            Require(pProgress->value() == pProgress->maximum(), "progress did not reach total");
            QTextStream(stdout) << "CASE_OK " << nCase << " bytes=" << arrSizes[nCase]
                << " sha256=" << HashFile(strReceivedPath).toHex() << Qt::endl;
        }
        Require(QMetaObject::invokeMethod(&oWidget, "OnStopReceiverClicked"), "stop slot missing");
        Require(arrSpins[0]->isEnabled(), "listener controls not restored");
        for (const QString &strMessage : arrLogs)
        {
            Require(!strMessage.contains(QStringLiteral("合并计时")) &&
                !strMessage.contains(QStringLiteral("合并流水线计时")), "removed merge timing log emitted");
        }
        QTextStream(stdout) << "ALL_OK" << Qt::endl;
        return 0;
    }
    catch (const std::exception &oError)
    {
        QTextStream(stderr) << "CHECK_FAILED " << oError.what() << Qt::endl;
        return 1;
    }
}

