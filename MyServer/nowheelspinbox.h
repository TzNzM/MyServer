#ifndef NOWHEELSPINBOX_H
#define NOWHEELSPINBOX_H

#include <QSpinBox>

class NoWheelSpinBox : public QSpinBox
{
    Q_OBJECT
public:
    /**
     * @brief NoWheelSpinBox
     * @param pParent Qt 父对象指针。
     * @return 无
     * @details 创建禁用鼠标滚轮的数值框，防止误触修改参数。
     */
    NoWheelSpinBox(QWidget *pParent = nullptr);

protected:
    /**
     * @brief wheelEvent
     * @param pWheelEvent 鼠标滚轮事件。
     * @return 无
     * @details 直接忽略滚轮输入，保留键盘和按钮修改能力。
     */
    void wheelEvent(QWheelEvent *pWheelEvent) override;

};

#endif // NOWHEELSPINBOX_H
