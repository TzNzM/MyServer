QT = core
CONFIG += console c++17 release
CONFIG -= app_bundle debug
TEMPLATE = app
TARGET = session_pipeline_check
TASK_SOURCE = $$quote($$PWD/../source)
INCLUDEPATH += $$TASK_SOURCE
SOURCES += session_pipeline_check.cpp
TASK_NAMES = ReliableReceiveSession ReliablePacketCodec ReliableSha256 ReliableMergeHasher
for(taskName, TASK_NAMES) {
    SOURCES += $$quote($$TASK_SOURCE/$${taskName}.cpp)
    HEADERS += $$quote($$TASK_SOURCE/$${taskName}.h)
}
win32: LIBS += -lbcrypt
