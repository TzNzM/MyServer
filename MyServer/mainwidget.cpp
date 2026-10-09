#include "mainwidget.h"
#include "ui_mainwidget.h"
#include "nowheelspinbox.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QDateTime>

MainWidget::MainWidget(QWidget *pParent)
    : QWidget(pParent),
//      m_pTransferController(new TransferController(this)),
      m_pModeComboBox(nullptr),
      m_pBindIpLineEdit(nullptr),
      m_pTargetIpLineEdit(nullptr),
      m_pListenPortSpinBox(nullptr),
      m_pSendPortSpinBox(nullptr),
      m_pThreadCountSpinBox(nullptr),
      m_pChunkSizeSpinBox(nullptr),
      m_pSegmentSizeSpinBox(nullptr),
      m_pChunkUnitComboBox(nullptr),
      m_pSegmentUnitComboBox(nullptr),
      m_pThreadCountLabel(nullptr),
      m_pChunkSizeLabel(nullptr),
      m_pSegmentSizeLabel(nullptr),
      m_pFilePathLineEdit(nullptr),
      m_pOutputDirectoryLineEdit(nullptr),
      m_pStartReceiverButton(nullptr),
      m_pStopReceiverButton(nullptr),
      m_pSendFileButton(nullptr),
      m_pLogTextEdit(nullptr),
      m_pSendProgressBar(nullptr),
      m_pReceiveStatusLabel(nullptr)
{
    setObjectName(QStringLiteral("MainWidget"));
    InitializeUI();

    //ToDo Connect函数中关于TransferController的
    //QOverload<int>::of从一组同名函数中，选出“参数类型为 int”的那个版本
    connect(m_pModeComboBox,QOverload<int>::of(&QComboBox::currentIndexChanged),this,&MainWidget::OnTransferModeChanged);

    // 设置默认的地址路径
    const QString strDefaultDownloadPath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString strDefaultOutputDirectory = strDefaultDownloadPath.isEmpty()
                                                 ? QDir::currentPath()
                                                 : strDefaultDownloadPath;
    m_pOutputDirectoryLineEdit->setText(strDefaultOutputDirectory);
    //ToDo m_pTransferController->SetOutputDirectory(strDefaultOutputDirectory);
    UpdateReceiverButtons(false);
    UpdateModeSpecificUi();

    setWindowTitle(QStringLiteral("Qt 大文件多线程传输工具"));
    resize(860, 640);
}

MainWidget::~MainWidget()
{

}

void MainWidget::OnBrowseFileClicked()
{
    QString strFilePath = QFileDialog::getOpenFileName(this,QStringLiteral("选择待发送文件"));
    if(!strFilePath.isEmpty()){
        m_pFilePathLineEdit->setText(strFilePath);
    }
}

void MainWidget::OnBrowseOutputDirectoryClicked()
{
    //trimmed() 是去掉首尾空格
    QString strOutputDirectory = QFileDialog::getExistingDirectory(this,QStringLiteral("选择接收输出目录"),m_pOutputDirectoryLineEdit->text().trimmed());
    if(!strOutputDirectory.isEmpty()){
        m_pOutputDirectoryLineEdit->setText(strOutputDirectory);
        //To do
        //m_pTransferController->SetOutputDirectory(strOutputDirectory);//传入Controller
    }
}

void MainWidget::OnStartReceiverClicked()
{
    m_pStartReceiverButton->setEnabled(false);
    m_pStopReceiverButton->setEnabled(true);
    //To do
}

void MainWidget::OnStopReceiverClicked()
{
    m_pStartReceiverButton->setEnabled(true);
    m_pStopReceiverButton->setEnabled(false);
    //To do
}

void MainWidget::OnSendFileClicked()
{
    //To do
}

void MainWidget::OnLogMessageAppended(const QString &strMessage)
{
    AppendLogMessage(strMessage);
}

void MainWidget::OnSendProgressChanged(int nCompletedChunkCount, int nTotalChunkCount)
{
    m_pSendProgressBar->setMaximum(qMax(1,nTotalChunkCount));
    m_pSendProgressBar->setValue(nCompletedChunkCount);
    m_pSendProgressBar->setFormat(QStringLiteral("发送进度：%1 / %2 块").arg(nCompletedChunkCount,nTotalChunkCount));
}

void MainWidget::OnSendFinished(bool bSuccess, const QString &strMessage)
{
    m_pSendFileButton->setEnabled(true);
    AppendLogMessage(strMessage);

    if(bSuccess){
        QMessageBox::information(this,QStringLiteral("发送完成"),strMessage);
    }else {
        QMessageBox::warning(this,QStringLiteral("发送失败"),strMessage);
    }
}

void MainWidget::OnTransferModeChanged(int nCurrentIndex)
{
    Q_UNUSED(nCurrentIndex) // 表示：这个参数虽然传进来了，但函数里故意不使用它。
//    它主要用于消除编译器的“未使用参数”警告，效果近似：
//    (void)nCurrentIndex;
//    这里槽函数必须接收 currentIndexChanged(int) 信号传来的索引，才能匹配信号：
//    connect(m_pModeComboBox,
//            QOverload<int>::of(&QComboBox::currentIndexChanged),
//            this,
//            &MainWidget::OnTransferModeChanged);
//    但实际逻辑不直接依赖这个 nCurrentIndex，而是在 UpdateModeSpecificUi() 内重新读取当前选择：
    UpdateModeSpecificUi();
}

void MainWidget::OnReceiveProgressChanged(const QString &strFileName, int nCompletedChunkCount, int nTotalChunkCount)
{
    m_pReceiveStatusLabel->setText(QStringLiteral("接收中:%1,已完成 %2 / %3 块").arg(strFileName,nCompletedChunkCount,nTotalChunkCount));
}

void MainWidget::OnReceiveFinished(const QString &strFilePath)
{
    m_pReceiveStatusLabel->setText(QStringLiteral("接收完成:%1").arg(strFilePath));
    AppendLogMessage(QStringLiteral("接收文件已合并完成：%1").arg(strFilePath));
    QMessageBox::information(this, QStringLiteral("接收完成"), QStringLiteral("文件已保存到：\n%1").arg(strFilePath));
}

void MainWidget::InitializeUI()
{
    QVBoxLayout *pMainLayout = new QVBoxLayout(this);

    QGroupBox *pConfigurationGroupBox = new QGroupBox(QStringLiteral("传输配置"),this);//一个分组布局
    QFormLayout *pConfigurationFormLayout = new QFormLayout(pConfigurationGroupBox);//表单布局

    m_pModeComboBox = new QComboBox(this);
    m_pModeComboBox->addItem(QStringLiteral("TCP"),static_cast<int>(ETransferMode::TcpMode));
    m_pModeComboBox->addItem(QStringLiteral("UDP"),static_cast<int>(ETransferMode::UdpMode));

    m_pBindIpLineEdit = new QLineEdit(QString("127.0.0.1"),this);
    m_pTargetIpLineEdit = new QLineEdit(QString("127.0.0.1"),this);

    m_pListenPortSpinBox = new NoWheelSpinBox(this);
    m_pListenPortSpinBox->setRange(1024,65535);
    m_pListenPortSpinBox->setValue(8899);

    m_pSendPortSpinBox = new NoWheelSpinBox(this);
    m_pSendPortSpinBox->setRange(1024,65535);
    m_pSendPortSpinBox->setValue(8890);

    m_pThreadCountSpinBox = new NoWheelSpinBox(this);
    m_pThreadCountSpinBox->setRange(1, 32);
    m_pThreadCountSpinBox->setValue(g_nDefaultThreadCount);

    m_pChunkSizeSpinBox = new NoWheelSpinBox(this);
    m_pChunkSizeSpinBox->setRange(1, 4096);
    m_pChunkSizeSpinBox->setValue(4);

    m_pSegmentSizeSpinBox = new NoWheelSpinBox(this);
    m_pSegmentSizeSpinBox->setRange(1, 4096);
    m_pSegmentSizeSpinBox->setValue(4);

    m_pChunkUnitComboBox = new QComboBox(this);
    m_pChunkUnitComboBox->addItem(QStringLiteral("KB"), 1024);//addItem(显示文本, 附加数据) 的第二个参数存的是单位换算倍率
    m_pChunkUnitComboBox->addItem(QStringLiteral("MB"), 1024 * 1024);
    m_pChunkUnitComboBox->setCurrentIndex(1);

    m_pSegmentUnitComboBox = new QComboBox(this);
    m_pSegmentUnitComboBox->addItem(QStringLiteral("KB"), 1024);
    m_pSegmentUnitComboBox->addItem(QStringLiteral("MB"), 1024 * 1024);
    m_pSegmentUnitComboBox->setCurrentIndex(1);

    m_pThreadCountLabel = new QLabel(this);
    m_pChunkSizeLabel = new QLabel(this);
    m_pSegmentSizeLabel = new QLabel(this);

    QWidget *pChunkSizeWidget = new QWidget(this);
    QHBoxLayout *pChunkSizeLayout = new QHBoxLayout(pChunkSizeWidget);
    pChunkSizeLayout->setContentsMargins(0,0,0,0);//是设置布局四周的内边距为 0。
    pChunkSizeLayout->addWidget(m_pChunkSizeSpinBox);
    pChunkSizeLayout->addWidget(m_pChunkUnitComboBox);

    QWidget *pSegmentSizeWidget = new QWidget(this);
    QHBoxLayout *pSegmentSizeLayout = new QHBoxLayout(pSegmentSizeWidget);
    pSegmentSizeLayout->setContentsMargins(0, 0, 0, 0);
    pSegmentSizeLayout->addWidget(m_pSegmentSizeSpinBox);
    pSegmentSizeLayout->addWidget(m_pSegmentUnitComboBox);

    m_pFilePathLineEdit = new QLineEdit(this);
    m_pFilePathLineEdit->setEnabled(false);//不让用户手动改地址
    QPushButton *pBrowseFileButton = new QPushButton(QStringLiteral("选择文件"), this);
    //connect(发送信号的对象, 信号, 接收信号的对象, 槽函数);
    //& 用来取得“成员函数的地址”，更准确说是成员函数指针
    connect(pBrowseFileButton,&QPushButton::clicked,this,&MainWidget::OnBrowseFileClicked);

    QWidget *pFileWidget = new QWidget(this);
    QHBoxLayout *pFileLayout = new QHBoxLayout(pFileWidget);
    pFileLayout->setContentsMargins(0, 0, 0, 0);
    pFileLayout->addWidget(m_pFilePathLineEdit);
    pFileLayout->addWidget(pBrowseFileButton);

    m_pOutputDirectoryLineEdit = new QLineEdit(this);
    m_pOutputDirectoryLineEdit->setEnabled(false);
    QPushButton *pBrowseOutputButton = new QPushButton(QStringLiteral("选择目录"), this);
    connect(pBrowseOutputButton, &QPushButton::clicked, this, &MainWidget::OnBrowseOutputDirectoryClicked);

    QWidget *pOutputWidget = new QWidget(this);
    QHBoxLayout *pOutputLayout = new QHBoxLayout(pOutputWidget);
    pOutputLayout->setContentsMargins(0, 0, 0, 0);
    pOutputLayout->addWidget(m_pOutputDirectoryLineEdit);
    pOutputLayout->addWidget(pBrowseOutputButton);

    pConfigurationFormLayout->addRow(QStringLiteral("传输模式"), m_pModeComboBox);
    pConfigurationFormLayout->addRow(QStringLiteral("监听 IP"), m_pBindIpLineEdit);
    pConfigurationFormLayout->addRow(QStringLiteral("目标 IP"), m_pTargetIpLineEdit);
    pConfigurationFormLayout->addRow(QStringLiteral("接收端端口"), m_pListenPortSpinBox);
    pConfigurationFormLayout->addRow(QStringLiteral("发送端端口"), m_pSendPortSpinBox);
    pConfigurationFormLayout->addRow(m_pThreadCountLabel, m_pThreadCountSpinBox);
    pConfigurationFormLayout->addRow(m_pChunkSizeLabel, pChunkSizeWidget);
    pConfigurationFormLayout->addRow(m_pSegmentSizeLabel, pSegmentSizeWidget);
    pConfigurationFormLayout->addRow(QStringLiteral("发送文件"), pFileWidget);
    pConfigurationFormLayout->addRow(QStringLiteral("接收目录"), pOutputWidget);

    QGroupBox *pActionGroupBox = new QGroupBox(QStringLiteral("操作区"),this);
    QHBoxLayout *pActionLayout = new QHBoxLayout(pActionGroupBox);
    m_pStartReceiverButton = new QPushButton(QStringLiteral("启动接收端"), this);
    m_pStopReceiverButton = new QPushButton(QStringLiteral("停止接收端"), this);
    m_pSendFileButton = new QPushButton(QStringLiteral("发送文件"), this);

    pActionLayout->addWidget(m_pStartReceiverButton);
    pActionLayout->addWidget(m_pStopReceiverButton);
    pActionLayout->addWidget(m_pSendFileButton);

    connect(m_pStartReceiverButton, &QPushButton::clicked, this, &MainWidget::OnStartReceiverClicked);
    connect(m_pStopReceiverButton, &QPushButton::clicked, this, &MainWidget::OnStopReceiverClicked);
    connect(m_pSendFileButton, &QPushButton::clicked, this, &MainWidget::OnSendFileClicked);

    m_pSendProgressBar = new QProgressBar(this);
    m_pSendProgressBar->setMinimum(0);
    m_pSendProgressBar->setMaximum(1);
    m_pSendProgressBar->setValue(0);
    m_pSendProgressBar->setFormat(QStringLiteral("发送进度：0 / 0 块"));

    m_pReceiveStatusLabel = new QLabel(QStringLiteral("接收状态：等待任务"), this);
    m_pReceiveStatusLabel->setObjectName(QStringLiteral("ReceiveStatusLabel"));//给控件起对象名，这样方便qss
    m_pReceiveStatusLabel->setWordWrap(true);//开启自动换行。状态文字较长、控件宽度不够时，文本会换到下一行显示，不会被横向截断。

    QGroupBox *pLogGroupBox = new QGroupBox(QStringLiteral("运行日志"), this);
    QVBoxLayout *pLogLayout = new QVBoxLayout(pLogGroupBox);
    m_pLogTextEdit = new QTextEdit(this);
    m_pLogTextEdit->setReadOnly(true);
    pLogLayout->addWidget(m_pLogTextEdit);

    pMainLayout->addWidget(pConfigurationGroupBox);
    pMainLayout->addWidget(pActionGroupBox);
    pMainLayout->addWidget(m_pSendProgressBar);
    pMainLayout->addWidget(m_pReceiveStatusLabel);
    pMainLayout->addWidget(pLogGroupBox);
}

/**
 * @brief MainWidget::GetCurrentTransferMode
 * @param 无
 * @return 当前界面选择的传输模式。
 * @details 读取下拉框中保存的枚举值并转成内部模式。
 */
ETransferMode MainWidget::GetCurrentTransferMode() const
{
    return static_cast<ETransferMode>(m_pModeComboBox->currentData().toInt());
}

void MainWidget::UpdateModeSpecificUi()
{
    const bool bIsTcpMode = (GetCurrentTransferMode() == ETransferMode::TcpMode);
    m_pThreadCountLabel->setText(bIsTcpMode ? QStringLiteral("发送窗口") : QStringLiteral("线程数"));
    m_pChunkSizeLabel->setText(bIsTcpMode ? QStringLiteral("块大小（仅 UDP）") : QStringLiteral("块大小"));
    m_pSegmentSizeLabel->setText(QStringLiteral("分片大小"));


    m_pChunkSizeSpinBox->setEnabled(!bIsTcpMode); //不是TCP时才可以选择
    m_pChunkUnitComboBox->setEnabled(!bIsTcpMode);


    m_pThreadCountSpinBox->setToolTip(bIsTcpMode
                                          ? QStringLiteral("TCP 长连接可靠模式下表示发送窗口大小。")
                                          : QStringLiteral("UDP 模式下表示 Qt 线程池并发数。"));
    m_pChunkSizeSpinBox->setToolTip(bIsTcpMode
                                        ? QStringLiteral("TCP 长连接可靠模式不使用短连接块发送。")
                                        : QStringLiteral("UDP 模式下控制每个任务负责的块大小。"));
    m_pChunkUnitComboBox->setToolTip(m_pChunkSizeLabel->toolTip());
    m_pSegmentSizeSpinBox->setToolTip(QStringLiteral("控制单个网络分片大小，可选择 KB 或 MB。"));
    m_pSegmentUnitComboBox->setToolTip(m_pSegmentSizeSpinBox->toolTip());
}

void MainWidget::UpdateReceiverButtons(bool bListening)
{
    m_pStartReceiverButton->setEnabled(!bListening);
    m_pStopReceiverButton->setEnabled(bListening);
}

qint64 MainWidget::GetChunkSizeBytes() const
{
    return static_cast<int>(static_cast<qint64>(m_pChunkSizeSpinBox->value()) * GetSizeUnitMultiplier(m_pChunkUnitComboBox));
}

int MainWidget::GetSegmentSizeBytes() const
{
    return static_cast<int>(static_cast<qint64>(m_pSegmentSizeSpinBox->value()) * GetSizeUnitMultiplier(m_pSegmentUnitComboBox));
}

qint64 MainWidget::GetSizeUnitMultiplier(const QComboBox *pUnitComboBox) const
{
//    因为之前已经把数据给进去了，所以这里就只需要.toLongLong()就行
//    m_pSegmentUnitComboBox->addItem(QStringLiteral("KB"), 1024);
//    m_pSegmentUnitComboBox->addItem(QStringLiteral("MB"), 1024 * 1024);
    if (pUnitComboBox == nullptr){
        return 1;
    }

    return pUnitComboBox->currentData().toLongLong();
}

void MainWidget::AppendLogMessage(const QString &strMessage)
{
    const QString strTimePrefix = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    m_pLogTextEdit->append(QStringLiteral("[%1] %2").arg(strTimePrefix,strMessage));
}

