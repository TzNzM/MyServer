#include "nowheelspinbox.h"

#include <QWheelEvent>
//NoWheelSpinBox
//       ↓ 继承
//   QSpinBox
//       ↓ 继承
//    QWidget

//我要创建 NoWheelSpinBox
//        ↓
//先构造它的父类 QSpinBox
//        ↓
//调用 QSpinBox(pParent)
//        ↓
//再执行 NoWheelSpinBox 构造函数体
//        ↓
//{
//}
NoWheelSpinBox::NoWheelSpinBox(QWidget *pParent)
    : QSpinBox(pParent)
{

}

void NoWheelSpinBox::wheelEvent(QWheelEvent *pWheelEvent)
{
    pWheelEvent->ignore();
}


