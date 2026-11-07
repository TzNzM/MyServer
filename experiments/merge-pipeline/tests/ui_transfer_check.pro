QT += core gui widgets network
CONFIG += console c++17 release
CONFIG -= app_bundle debug
TEMPLATE = app
TARGET = ui_transfer_check
TASK_SOURCE = $$quote($$PWD/../source)
INCLUDEPATH += $$TASK_SOURCE
SOURCES += ui_transfer_check.cpp
TASK_NAMES = ReliableMergeHasher ReliableSha256 ReliablePacketCodec ReliableReceiveSession ReliableReceiveSessionStore ReliableReceiverWorker ReliableSendLane ReliableSenderWorker ReliableTcpServer ReliableTransferIo ReliableTransferManager TransferController mainwidget nowheelspinbox
for(taskName, TASK_NAMES) {
    SOURCES += $$quote($$TASK_SOURCE/$${taskName}.cpp)
    HEADERS += $$quote($$TASK_SOURCE/$${taskName}.h)
}
FORMS += $$quote($$TASK_SOURCE/mainwidget.ui)
RESOURCES += $$quote($$TASK_SOURCE/resources/resources.qrc)
win32: LIBS += -lbcrypt
