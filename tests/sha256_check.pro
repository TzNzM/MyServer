QT = core
CONFIG += console c++17 release
CONFIG -= app_bundle debug
TEMPLATE = app
TARGET = sha256_check
TASK_SOURCE = $$quote($$PWD/../MyServer)
INCLUDEPATH += $$TASK_SOURCE
SOURCES += $$PWD/sha256_check.cpp $$quote($$TASK_SOURCE/ReliableSha256.cpp)
HEADERS += $$quote($$TASK_SOURCE/ReliableSha256.h)
win32: LIBS += -lbcrypt
linux {
    CONFIG += link_pkgconfig
    PKGCONFIG += libcrypto
}
