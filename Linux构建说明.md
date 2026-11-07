# MyServer Linux 构建说明

文件摘要在 Windows 使用 CNG，Linux 使用 OpenSSL EVP，其余平台使用 Qt。外部统一调用 ReliableSha256::Initialize / AddData / Finish，协议摘要始终为标准 SHA-256。当前主工程采用串行合并，不包含合并性能计时或独立摘要线程。

## 环境要求

- Qt 5 开发环境：Core、Widgets、Network 及 qmake、moc、uic、rcc。
- 支持 C++17 的编译器和 make。
- pkg-config、OpenSSL 开发头文件与 libcrypto；代码使用兼容 OpenSSL 1.1.1 / 3.x 的 EVP 接口，安装发行版维护的版本。

可先检查：

```sh
qmake -query QT_VERSION
pkg-config --modversion libcrypto
pkg-config --modversion Qt5Core Qt5Widgets Qt5Network
```

若默认 qmake 属于 Qt 6，选择本机 Qt 5 的工具路径。只有 openssl 命令行工具不代表已安装开发头文件、链接库和 libcrypto.pc。

## 编译与运行

复制完整源码到 Linux，在仓库根目录执行：

```sh
mkdir -p build-linux-release
cd build-linux-release
qmake ../MyServer/MyServer.pro CONFIG+=release CONFIG-=debug
make -j4
./MyServer
```

qmake 的 linux 条件使用 pkg-config 查找 libcrypto；Windows 只链接 bcrypt，不增加 OpenSSL DLL。不要复用 Windows 的 Makefile、目标文件或 EXE。

GUI 需要可用的桌面显示环境和对应 Qt 平台插件。分发到其他 Linux 电脑时要处理同架构、兼容系统的 Qt、libcrypto 和插件依赖；Windows 的 windeployqt 包不能作为 Linux 原生发布包。

## 后端实现与验证边界

EVP_MD_CTX_new 创建上下文，EVP_DigestInit_ex 选择 SHA-256，EVP_DigestUpdate 按顺序增量追加，EVP_DigestFinal_ex 得到摘要，析构时 EVP_MD_CTX_free。每步检查返回值、摘要长度和当前线程的错误队列。完成缓存、禁止完成后追加、重新初始化语义与 CNG 相同。

Windows CNG 的摘要和状态回归通过。相同 EVP 后端曾在 Windows 独立测试副本中验证摘要与失败恢复，但**真实 Linux 编译、链接和运行尚未完成**，不能把 Windows EVP 测试当作 Linux 验收。

首次在 Linux 构建后应验证标准摘要、分块输入、错误与重新初始化、小文件收发和取消，再进行跨系统及大文件传输。当前回归源码保存在 tests 目录。
