#ifndef MAINWIDGET_H
#define MAINWIDGET_H

#include <QWidget>
#include "transfertypes.h"

class QComboBox;
class QLineEdit;
class QPushButton;
class QTextEdit;
class QProgressBar;
class QLabel;
class NoWheelSpinBox;

class MainWidget : public QWidget
{
//    Q_OBJECT
//        ↓
//    Qt 元对象系统
//        ↓
//    signals / slots / properties / 元信息等
    Q_OBJECT //Q_OBJECT 本质上是一个宏。

public:
    MainWidget(QWidget *parent = nullptr);
    ~MainWidget();

private slots:
    /**
     * @brief OnBrowseFileClicked
     * @param 无
     * @return 无
     * @details 选择需要发送的大文件。
     */
    void OnBrowseFileClicked();

    /**
     * @brief OnBrowseOutputDirectoryClicked
     * @param 无
     * @return 无
     * @details 选择接收端文件输出目录。
     */
    void OnBrowseOutputDirectoryClicked();

    /**
     * @brief OnStartReceiverClicked
     * @param 无
     * @return 无
     * @details 根据当前配置启动接收端监听。
     */
    void OnStartReceiverClicked();

    /**
     * @brief OnStopReceiverClicked
     * @param 无
     * @return 无
     * @details 停止当前接收监听。
     */
    void OnStopReceiverClicked();

    /**
     * @brief OnSendFileClicked
     * @param 无
     * @return 无
     * @details 发起一次多线程文件发送任务。
     */
    void OnSendFileClicked();

    /**
     * @brief OnLogMessageAppended
     * @param strMessage 需要输出的日志消息。
     * @return 无
     * @details 在界面日志区按时间顺序追加运行日志。
     */
    void OnLogMessageAppended(const QString &strMessage);

    /**
     * @brief OnSendProgressChanged
     * @param nCompletedChunkCount 已发送完成块数。
     * @param nTotalChunkCount 总块数。
     * @return 无
     * @details 更新发送进度条和发送状态文本。
     */
    void OnSendProgressChanged(int nCompletedChunkCount, int nTotalChunkCount);

    /**
     * @brief OnSendFinished
     * @param bSuccess 发送是否成功。
     * @param strMessage 完成时的提示消息。
     * @return 无
     * @details 在所有发送线程结束后恢复界面可操作状态。
     */
    void OnSendFinished(bool bSuccess, const QString &strMessage);

    /**
     * @brief OnTransferModeChanged
     * @param nCurrentIndex 当前下拉框索引。
     * @return 无
     * @details 根据协议模式刷新界面参数语义。
     */
    void OnTransferModeChanged(int nCurrentIndex);

    /**
     * @brief OnReceiveProgressChanged
     * @param strFileName 当前接收文件名。
     * @param nCompletedChunkCount 已完成块数。
     * @param nTotalChunkCount 总块数。
     * @return 无
     * @details 更新接收端块级进度说明文本。
     */
    void OnReceiveProgressChanged(const QString &strFileName, int nCompletedChunkCount, int nTotalChunkCount);

    /**
     * @brief OnReceiveFinished
     * @param strFilePath 接收完成的最终文件路径。
     * @return 无
     * @details 接收端完成文件合并后提示最终路径。
     */
    void OnReceiveFinished(const QString &strFilePath);

private:
    //当前UI的初始化的工作
    void InitializeUI();

    /**
     * @brief MainWidget::GetCurrentTransferMode
     * @param 无
     * @return 当前界面选择的传输模式。
     * @details 读取下拉框中保存的枚举值并转成内部模式。
     */
    ETransferMode GetCurrentTransferMode() const;

    /**
     * @brief UpdateModeSpecificUi
     * @param 无
     * @return 无
     * @details 根据 TCP / UDP 模式调整参数标签与启用状态。
     */
    void UpdateModeSpecificUi();

    /**
     * @brief UpdateReceiverButtons
     * @param bListening 当前接收端是否正在监听。
     * @return 无
     * @details 统一控制启动/停止接收端按钮的可用状态。
     */
    void UpdateReceiverButtons(bool bListening);

    /**
     * @brief GetChunkSizeBytes
     * @param 无
     * @return 当前界面配置的块大小字节数。
     * @details 结合数值和单位下拉框，换算出块大小的真实字节数。
     */
    qint64 GetChunkSizeBytes() const;

    /**
     * @brief GetSegmentSizeBytes
     * @param 无
     * @return 当前界面配置的分片大小字节数。
     * @details 结合数值和单位下拉框，换算出分片大小的真实字节数。
     */
    int GetSegmentSizeBytes() const;

    /**
     * @brief GetSizeUnitMultiplier
     * @param pUnitComboBox 单位下拉框。
     * @return 当前单位对应的倍率。
     * @details 从单位下拉框读取 KB / MB 的倍率，用于发送参数换算。
     */
    qint64 GetSizeUnitMultiplier(const QComboBox *pUnitComboBox) const;

    /**
     * @brief AppendLogMessage
     * @param strMessage 需要追加的日志消息。
     * @return 无
     * @details 封装带时间戳的日志输出动作。
     */
    void AppendLogMessage(const QString &strMessage);

    QComboBox *m_pModeComboBox;//选择是TCP还是UDP连接
    QLineEdit *m_pBindIpLineEdit;//输入监听IP的地址
    QLineEdit *m_pTargetIpLineEdit;//输入目标的IP地址
    NoWheelSpinBox *m_pListenPortSpinBox;//输入监听的端口
    NoWheelSpinBox *m_pSendPortSpinBox;//输入发送的端口
    NoWheelSpinBox *m_pThreadCountSpinBox;//输入连接的线程数
    NoWheelSpinBox *m_pChunkSizeSpinBox;//输入文件块大小：一个线程任务负责发送多大的文件块
    NoWheelSpinBox *m_pSegmentSizeSpinBox;//网络分片大小：一个文件块再拆成多大的网络包载荷
    QComboBox *m_pChunkUnitComboBox; // 选择文件块大小的单位（KB/MB）
    QComboBox *m_pSegmentUnitComboBox;// 选择网络分片大小的单位（KB/MB）
    QLabel *m_pThreadCountLabel;          // 显示线程数配置项的文字说明
    QLabel *m_pChunkSizeLabel;            // 显示文件块大小配置项的文字说明
    QLabel *m_pSegmentSizeLabel;          // 显示网络分片大小配置项的文字说明
    QLineEdit *m_pFilePathLineEdit;       // 输入或显示待发送文件的路径
    QLineEdit *m_pOutputDirectoryLineEdit;// 输入或显示接收文件的保存目录
    QPushButton *m_pStartReceiverButton;  // 点击后启动文件接收端监听
    QPushButton *m_pStopReceiverButton;   // 点击后停止文件接收端监听
    QPushButton *m_pSendFileButton;       // 点击后开始向目标地址发送文件
    QTextEdit *m_pLogTextEdit;            // 显示运行日志、发送状态和错误信息
    QProgressBar *m_pSendProgressBar;     // 显示当前文件发送进度
    QLabel *m_pReceiveStatusLabel;        // 显示接收端当前状态，例如“未启动”或“正在监听”
};
#endif // MAINWIDGET_H
