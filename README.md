# MyServer

基于 Qt Widgets / Qt Network 的 TCP 长连接文件传输学习项目，正在逐步复现。

## 分支

- `archive/current-scaffold`：保留清理前的源码、学习注释、旧协议和短连接原型。
- `main`：仅面向可靠 TCP 长连接的复现主线。

## 当前进度

主线已保留界面、QSS 资源和无滚轮数值控件，移除了旧短连接通道、旧包协议及 UDP 专用配置。
目前监听、发送按钮仅提示功能待接入，尚不能传输文件；ACK、重传、断点续传和哈希校验尚未实现。
发送窗口代表未确认分片数量，不是线程数量。

## 后续复现顺序

1. `ReliableTransferTypes`：可靠协议包头、消息类型与会话元信息。
2. `ReliablePacketCodec`：编解码、TCP 粘包拆包与输入校验。
3. `ReliableTransferManager`：监听、接收会话、分片落盘。
4. `ReliableSenderWorker`：一个 socket 持续发送整个文件。
5. ACK、发送窗口、超时重传、断线重连。
6. 状态持久化、断点续传与最终哈希校验。
7. 仅调度可靠 TCP 的 `TransferController`，接入界面。

## 构建

使用 Qt 5.14.2 / MinGW 64 位套件，在 Qt Creator 打开 `MyServer/MyServer.pro`。
也可以在配置好 Qt 和 MinGW PATH 的终端中进行独立构建：

```powershell
mkdir build
cd build
qmake ../MyServer/MyServer.pro
mingw32-make
```

默认监听 `0.0.0.0:8899`，目标 `127.0.0.1:8899`；网络模块接入后再进行本机传输验证。
构建目录、可执行文件和 Qt Creator 用户配置不纳入版本控制。
