QT = core
CONFIG += console c++17 release
CONFIG -= app_bundle debug
TEMPLATE = app
TARGET = session_check
TASK_SOURCE = $$quote($$PWD/../MyServer)
INCLUDEPATH += $$TASK_SOURCE
SOURCES += $$PWD/session_check.cpp
TASK_NAMES = ReliableReceiveSession ReliablePacketCodec ReliableSha256
for(taskName, TASK_NAMES) {
    SOURCES += $$quote($$TASK_SOURCE/$${taskName}.cpp)
    HEADERS += $$quote($$TASK_SOURCE/$${taskName}.h)
}
win32: LIBS += -lbcrypt
linux {
    CONFIG += link_pkgconfig
    PKGCONFIG += libcrypto
}
