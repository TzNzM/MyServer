#ifndef TRANSFERDEFAULTS_H
#define TRANSFERDEFAULTS_H

#include <QtGlobal>

// 仅保存界面默认值；可靠 TCP 协议将在 ReliableTransferTypes 中单独定义。
static const qint32 g_nDefaultSendWindowSize = 4;
static const qint32 g_nDefaultSegmentSize = 60 * 1024;

#endif // TRANSFERDEFAULTS_H
