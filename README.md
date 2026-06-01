# Game Server Projects

从零开始学习游戏服务器开发，使用 C++ 和 CMake，从简单到复杂逐步构建。

## 项目路线

| 阶段 | 项目 | 核心概念 |
|------|------|----------|
| Level 1 | Echo 服务器 | socket、bind、listen、accept |
| Level 2 | 多人聊天室 | select/poll、多客户端、广播 |
| Level 3 | 井字棋对战 | 游戏状态机、匹配、回合协议 |
| Level 4 | 实时乒乓游戏 | 游戏主循环、tick同步、状态复制 |
| Level 5 | 简易坦克大战 | 权威服务器、实体系统、延迟补偿 |
| Level 6 | Mini MMO | AOI兴趣区域、空间分区、持久化 |

## 编译运行

```bash
# 编译所有项目
mkdir build && cd build
cmake ..
cmake --build .

# 运行 Echo 服务器
./level1-echo/Debug/echo_server.exe

# 另一个终端运行客户端测试
./level1-echo/Debug/echo_client.exe
```

## 技术栈

- **语言**: C++17
- **构建系统**: CMake 3.14+
- **网络库**: Winsock2 (Windows) / POSIX socket (Linux)
- **平台**: Windows / Linux
