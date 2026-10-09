#include "mainwidget.h"

#include <QApplication>
#include <QFile>

// 设置qss的全局设置
static void ApplyApplacationStyle(QApplication &application){
    QFile f(QStringLiteral(":/app_style.qss"));//QStringLiteral专门处理固定字符串的,更高效
    if(f.open(QIODevice::ReadOnly | QIODevice::Text)){
        application.setStyleSheet(QString::fromUtf8(f.readAll()));
    }
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    ApplyApplacationStyle(a);
    MainWidget w;
    w.show();
    return a.exec();
}


