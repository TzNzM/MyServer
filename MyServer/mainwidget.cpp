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
      m_pBindIpLineEdit(nullptr),
      m_pTargetIpLineEdit(nullptr),
      m_pListenPortSpinBox(nullptr),
      m_pSendPortSpinBox(nullptr),
      m_pThreadCountSpinBox(nullptr),
      m_pSegmentSizeSpinBox(nullptr),
      m_pSegmentUnitComboBox(nullptr),
      m_pThreadCountLabel(nullptr),
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

    // 设置默认的地址路径
    const QString strDefaultDownloadPath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString strDefaultOutputDirectory = strDefaultDownloadPath.isEmpty()
                                                 ? QDir::currentPath()
                                                 : strDefaultDownloadPath;
    m_pOutputDirectoryLineEdit->setText(strDefaultOutputDirectory);
    //ToDo m_pTransferController->SetOutputDirectory(strDefaultOutputDirectory);
    UpdateReceiverButtons(false);
    AppendLogMessage(QStringLiteral("当前为 TCP 长连接复现界面；监听和发送模块尚待接入。"));

    setWindowTitle(QStringLiteral("TCP 长连接文件传输（复现中）"));
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
    // TODO: 接入可靠 TCP 接收管理器，监听成功后再更新按钮状态。
    AppendLogMessage(QStringLiteral("TCP 长连接接收模块尚待复现。"));
}

void MainWidget::OnStopReceiverClicked()
{
    // TODO: 接入可靠 TCP 接收管理器，停止监听后更新按钮状态。
    AppendLogMessage(QStringLiteral("TCP 长连接接收模块尚待复现。"));
}

void MainWidget::OnSendFileClicked()
{
    // TODO: 接入 ReliableSenderWorker，整次文件传输复用同一个 socket。
    AppendLogMessage(QStringLiteral("TCP 长连接发送模块尚待复现。"));
}

void MainWidget::OnLogMessageAppended(const QString &strMessage)
{
    AppendLogMessage(strMessage);
}

void MainWidget::OnSendProgressChanged(int nCompletedChunkCount, int nTotalChunkCount)
{
    m_pSendProgressBar->setMaximum(qMax(1,nTotalChunkCount));
    m_pSendProgressBar->setValue(nCompletedChunkCount);
    m_pSendProgressBar->setFormat(QStringLiteral("发送进度：%1 / %2 分片").arg(nCompletedChunkCount).arg(nTotalChunkCount));
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

void MainWidget::OnReceiveProgressChanged(const QString &strFileName, int nCompletedChunkCount, int nTotalChunkCount)
{
    m_pReceiveStatusLabel->setText(QStringLiteral("接收中:%1,已完成 %2 / %3 分片").arg(strFileName).arg(nCompletedChunkCount).arg(nTotalChunkCount));
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

    m_pBindIpLineEdit = new QLineEdit(QString("0.0.0.0"),this);
    m_pTargetIpLineEdit = new QLineEdit(QString("127.0.0.1"),this);

    m_pListenPortSpinBox = new NoWheelSpinBox(this);
    m_pListenPortSpinBox->setRange(1024,65535);
    m_pListenPortSpinBox->setValue(8899);

    m_pSendPortSpinBox = new NoWheelSpinBox(this);
    m_pSendPortSpinBox->setRange(1024,65535);
    m_pSendPortSpinBox->setValue(8899);

    m_pThreadCountSpinBox = new NoWheelSpinBox(this);
    m_pThreadCountSpinBox->setRange(1, 32);
    m_pThreadCountSpinBox->setValue(g_nDefaultSendWindowSize);
    m_pThreadCountSpinBox->setToolTip(QStringLiteral("允许同时已发送但未收到 ACK 的分片数。"));

    m_pSegmentSizeSpinBox = new NoWheelSpinBox(this);
    m_pSegmentSizeSpinBox->setRange(1, 4096);
    m_pSegmentSizeSpinBox->setValue(g_nDefaultSegmentSize / 1024);

    m_pSegmentUnitComboBox = new QComboBox(this);
    m_pSegmentUnitComboBox->addItem(QStringLiteral("KB"), 1024);
    m_pSegmentUnitComboBox->addItem(QStringLiteral("MB"), 1024 * 1024);
    m_pSegmentUnitComboBox->setCurrentIndex(0);

    m_pThreadCountLabel = new QLabel(QStringLiteral("发送窗口"), this);
    m_pSegmentSizeLabel = new QLabel(QStringLiteral("分片大小"), this);

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

    pConfigurationFormLayout->addRow(QStringLiteral("传输协议"), new QLabel(QStringLiteral("TCP 长连接（待接入）"), this));
    pConfigurationFormLayout->addRow(QStringLiteral("监听 IP"), m_pBindIpLineEdit);
    pConfigurationFormLayout->addRow(QStringLiteral("目标 IP"), m_pTargetIpLineEdit);
    pConfigurationFormLayout->addRow(QStringLiteral("接收端端口"), m_pListenPortSpinBox);
    pConfigurationFormLayout->addRow(QStringLiteral("目标接收端端口"), m_pSendPortSpinBox);
    pConfigurationFormLayout->addRow(m_pThreadCountLabel, m_pThreadCountSpinBox);
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
    m_pSendProgressBar->setFormat(QStringLiteral("发送进度：0 / 0 分片"));

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

void MainWidget::UpdateReceiverButtons(bool bListening)
{
    m_pStartReceiverButton->setEnabled(!bListening);
    m_pStopReceiverButton->setEnabled(bListening);
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

