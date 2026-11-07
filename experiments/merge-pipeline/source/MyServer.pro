QT       += core gui network

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

#【CNG SHA-256】Windows 链接系统 BCrypt，不增加第三方部署 DLL。
win32: LIBS += -lbcrypt

#【Linux OpenSSL】通过 pkg-config 查找 libcrypto 的头文件和链接参数。
linux {
    CONFIG += link_pkgconfig
    PKGCONFIG += libcrypto
}

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    ReliableMergeHasher.cpp \
    ReliableSha256.cpp \
    ReliablePacketCodec.cpp \
    ReliableReceiveSession.cpp \
    ReliableReceiveSessionStore.cpp \
    ReliableReceiverWorker.cpp \
    ReliableSendLane.cpp \
    ReliableSenderWorker.cpp \
    ReliableTcpServer.cpp \
    ReliableTransferIo.cpp \
    ReliableTransferManager.cpp \
    TransferController.cpp \
    mainwidget.cpp \
    nowheelspinbox.cpp \
    main.cpp

HEADERS += \
    ReliableMergeHasher.h \
    ReliableSha256.h \
    ReliablePacketCodec.h \
    ReliableReceiveSession.h \
    ReliableReceiveSessionStore.h \
    ReliableReceiverWorker.h \
    ReliableSendLane.h \
    ReliableSendTaskState.h \
    ReliableSenderWorker.h \
    ReliableTcpServer.h \
    ReliableTransferIo.h \
    ReliableTransferManager.h \
    ReliableTransferTypes.h \
    ReliableTransferUtils.h \
    TransferController.h \
    mainwidget.h \
    nowheelspinbox.h \
    transferdefaults.h

FORMS += \
    mainwidget.ui

TRANSLATIONS += \
    MyServer_zh_CN.ts

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    resources/resources.qrc
