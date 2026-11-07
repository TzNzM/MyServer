#include "mainwidget.h"
#include "ui_mainwidget.h"
#include "nowheelspinbox.h"
#include "ReliableTransferTypes.h"
#include "transferdefaults.h"

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
      m_pTransferController(new TransferController(this)),
      m_pBindIpLineEdit(nullptr),
      m_pTargetIpLineEdit(nullptr),
      m_pListenPortSpinBox(nullptr),
      m_pSendPortSpinBox(nullptr),
      m_pThreadCountSpinBox(nullptr),
      m_pWindowSizeSpinBox(nullptr),
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

    //第一段：将控制器日志显示到界面。
    connect(
        m_pTransferController,
        &TransferController::SignalLogMessage,
        this,
        &MainWidget::OnLogMessageAppended);

    //第二段：更新发送进度，并处理发送结果。
    connect(
        m_pTransferController,
        &TransferController::SignalSendProgress,
        this,
        &MainWidget::OnSendProgressChanged);

    connect(
        m_pTransferController,
        &TransferController::SignalSendFinished,
        this,
        &MainWidget::OnSendFinished);

    //第三段：更新接收进度，并显示最终文件路径。
    connect(
        m_pTransferController,
        &TransferController::SignalReceiveProgress,
        this,
        &MainWidget::OnReceiveProgressChanged);

    connect(
        m_pTransferController,
        &TransferController::SignalReceiveFinished,
        this,
        &MainWidget::OnReceiveFinished);

    // 设置默认的地址路径
    const QString strDefaultDownloadPath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString strDefaultOutputDirectory = strDefaultDownloadPath.isEmpty()
                                                 ? QDir::currentPath()
                                                 : strDefaultDownloadPath;
    m_pOutputDirectoryLineEdit->setText(strDefaultOutputDirectory);
    m_pTransferController->SetOutputDirectory(strDefaultOutputDirectory);
    UpdateReceiverButtons(false);
    AppendLogMessage(
        QStringLiteral("TCP 多通道文件传输已就绪，可启动监听或发送文件。"));

    setWindowTitle(QStringLiteral("TCP 长连接"));
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
        m_pTransferController->SetOutputDirectory(strOutputDirectory);//传入Controller
    }
}

void MainWidget::OnStartReceiverClicked()
{
    //第一段：读取监听配置，并同步输出目录。
    const QString strBindIp =
        m_pBindIpLineEdit->text().trimmed();

    const int nPort = m_pListenPortSpinBox->value();

    m_pTransferController->SetOutputDirectory(m_pOutputDirectoryLineEdit->text().trimmed());

    //第二段：启动监听，失败时显示原因。
    QString strErrorMessage;
    if (!m_pTransferController->StartReceiver(
            strBindIp,
            nPort,
            strErrorMessage))
    {
        AppendLogMessage(
            QStringLiteral("启动接收端失败：%1")
                .arg(strErrorMessage));

        QMessageBox::warning(
            this,
            QStringLiteral("启动失败"),
            strErrorMessage);

        return;
    }

    //第三段：监听成功后更新界面。
    UpdateReceiverButtons(true);

    m_pReceiveStatusLabel->setText(
        QStringLiteral("正在监听：%1:%2，等待发送端连接。")
            .arg(strBindIp.isEmpty()
                     ? QStringLiteral("0.0.0.0")
                     : strBindIp)
            .arg(nPort));
}

void MainWidget::OnStopReceiverClicked()
{
    //第一段：停止监听，并等待接收线程退出。
    m_pTransferController->StopReceiver();

    //第二段：恢复界面状态。
    UpdateReceiverButtons(false);

    m_pReceiveStatusLabel->setText(
            QStringLiteral("接收端已停止。"));
    AppendLogMessage(
        QStringLiteral("接收端已停止，接收线程已回收。"));

}

void MainWidget::OnSendFileClicked()
{
    //第一段：读取界面中的发送参数。
    const QString strFilePath =
        m_pFilePathLineEdit->text();

    const QString strTargetIp =
        m_pTargetIpLineEdit->text().trimmed();

    const int nPort = m_pSendPortSpinBox->value();
    const int nThreadCount = m_pThreadCountSpinBox->value();
    const int nWindowSize = m_pWindowSizeSpinBox->value();
    const int nSegmentSize = GetSegmentSizeBytes();

    if(strFilePath.isEmpty())
    {
        QMessageBox::warning(
                    this,
                    QStringLiteral("无法发送"),
                    QStringLiteral("请先选择待发送文件。"));
                return;
    }

    if (nSegmentSize <= 0)
    {
        QMessageBox::warning(
            this,
            QStringLiteral("无法发送"),
            QStringLiteral("分片大小必须大于 0，且不超过 4 MiB。"));
        return;
    }

    //第三段：准备本次任务的界面状态，避免重复点击。
    m_pSendFileButton->setEnabled(false);
    m_pSendProgressBar->setRange(0, 1);
    m_pSendProgressBar->setValue(0);
    m_pSendProgressBar->setFormat(
        QStringLiteral("正在准备文件并计算 SHA-256……"));
    //第四段：启动后台发送任务，启动失败时恢复按钮。
    QString strErrorMessage;
    if(!m_pTransferController->StartSend(
                strFilePath,
                strTargetIp,
                nPort,
                nThreadCount,
                nWindowSize,
                nSegmentSize,
                strErrorMessage))
    {
        m_pSendFileButton->setEnabled(true);
        m_pSendProgressBar->setFormat(QStringLiteral("发送任务未启动"));
        AppendLogMessage(
                    QStringLiteral("启动发送失败：%1")
                        .arg(strErrorMessage));
        QMessageBox::warning(
                    this,
                    QStringLiteral("启动发送失败"),
                    strErrorMessage);
        return;
    }

    AppendLogMessage(
            QStringLiteral("发送任务已启动，正在后台准备文件。"));
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
    m_pThreadCountSpinBox->setRange(1, g_nReliableMaxThreadCount);
    m_pThreadCountSpinBox->setValue(g_nReliableDefaultSendThreadCount);
    m_pThreadCountSpinBox->setToolTip(QStringLiteral("请求的数据发送线程数，接收端为每条数据连接创建对应线程；"
                                                                                   "不包含额外的协调连接。"));
    m_pWindowSizeSpinBox = new NoWheelSpinBox(this);
    m_pWindowSizeSpinBox->setRange(1,g_nReliableMaxWindowSize);
    m_pWindowSizeSpinBox->setValue(g_nReliableDefaultWindowSize);
    m_pWindowSizeSpinBox->setToolTip(
        QStringLiteral(
            "每条数据连接允许同时已发送但未收到业务 ACK 的分片数。"));


    m_pSegmentSizeSpinBox = new NoWheelSpinBox(this);
    m_pSegmentSizeSpinBox->setRange(1, 4096);
    m_pSegmentSizeSpinBox->setValue(g_nDefaultSegmentSize / 1024);

    m_pSegmentUnitComboBox = new QComboBox(this);
    m_pSegmentUnitComboBox->addItem(QStringLiteral("KB"), 1024);
    m_pSegmentUnitComboBox->addItem(QStringLiteral("MB"), 1024 * 1024);
    m_pSegmentUnitComboBox->setCurrentIndex(0);

    m_pThreadCountLabel = new QLabel(QStringLiteral("数据发送线程数"), this);
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
    pConfigurationFormLayout->addRow(QStringLiteral("每通道发送窗口"),m_pWindowSizeSpinBox);
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

    //监听期间禁止修改监听地址和端口
    m_pBindIpLineEdit->setEnabled(!bListening);
    m_pListenPortSpinBox->setEnabled(!bListening);
}

int MainWidget::GetSegmentSizeBytes() const
{
    const qint64 nSegmentSize =
        static_cast<qint64>(m_pSegmentSizeSpinBox->value())
        * GetSizeUnitMultiplier(m_pSegmentUnitComboBox);

    //先检查范围，再转换为发送接口需要的 int。
    if (nSegmentSize <= 0 ||
        nSegmentSize > g_nReliableMaxSegmentSize)
    {
        return 0;
    }

    return static_cast<int>(nSegmentSize);
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

