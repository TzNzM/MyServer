# MyServer

基于 Qt Widgets / Qt Network 的 TCP 长连接文件传输学习项目，正在逐步复现。

## 分支

- `archive/unfinished-udp-tcp-short`：保留清理前的源码和学习注释；UDP 与 TCP 短连接部分尚未完成。
- `main`：仅面向可靠 TCP 长连接的复现主线。

## 当前进度

主线已保留界面、QSS 资源和无滚轮数值控件，移除了旧短连接通道、旧包协议及 UDP 专用配置。
已添加 `ReliableTransferTypes.h` 和 `ReliablePacketCodec.h/.cpp`，写出可靠协议类型、80 字节包头编解码、七种消息封包及对应载荷解析，并加入 qmake 工程。
2026-10-13 使用 Qt 5.14.2 / MinGW 64 位完成 Debug 构建；协议往返、半包/粘包和非法输入测试尚未执行，输入校验仍需完善。
已添加 `ReliableTransferManager.h/.cpp`，写出接收端监听、连接管理、缓存拆包、元信息握手、分片写入和 ACK、状态保存与恢复、最终哈希校验及文件生成逻辑；这些行为尚未经过传输联调验收。
2026-10-16 使用同一套件完成 Debug 编译和链接；`StartSend()` 仍为空实现，存在缺少返回值及参数未使用警告。
目前监听、发送按钮仅提示功能待接入，尚不能从界面传输文件；`ReliableSenderWorker.h/.cpp` 已新建为空类骨架并加入 `.pro`，QObject 继承、信号、ProcessSend 和发送逻辑均待实现；`TransferController` 尚未创建。
发送窗口代表未确认分片数量，不是线程数量。

## 后续复现顺序

1. 协议阶段收尾：编解码往返测试、范围转换测试与输入校验。
2. `ReliableSenderWorker.h/.cpp`：在现有空类骨架上补齐工作对象接口，再逐步实现长连接发送。
3. 补齐 `ReliableTransferManager::StartSend()` 的线程、信号和生命周期调度；当前空实现不可调用。
4. 首次收发联调：验证握手、分片写入、业务 ACK、最终结果及文件 SHA-256。
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
