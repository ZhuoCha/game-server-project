/**
 * Level 1: Echo 服务器
 *
 * 最基础的游戏服务器 — 接收客户端消息并原样返回。
 *
 * 学习目标：
 *   1. 理解 TCP socket 的生命周期: socket() → bind() → listen() → accept()
 *   2. 理解网络字节序（big-endian）与主机字节序的转换
 *   3. 理解 recv/send 的基本用法
 *   4. 理解服务器端口的含义
 *
 * 这是所有游戏服务器的基石。后续的聊天室、游戏对战服务器
 * 都建立在这个基础之上。
 */

#include <iostream>
#include <string>
#include <cstring>
#include <winsock2.h>   // Windows Socket API

#pragma comment(lib, "ws2_32.lib")  // 链接 Winsock 库

// ============================================================
// 服务器配置
// ============================================================
constexpr int  SERVER_PORT   = 8080;        // 监听端口
constexpr int  BACKLOG       = 5;           // 等待队列最大长度
constexpr int  BUFFER_SIZE   = 4096;        // 接收缓冲区大小

// ============================================================
// 辅助函数：打印错误信息
// ============================================================
void print_error(const char* where) {
    std::cerr << "[ERROR] " << where
              << " failed, code=" << WSAGetLastError() << std::endl;
}

// ============================================================
// 主函数
// ============================================================
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Level 1: Echo Server (Port " << SERVER_PORT << ")" << std::endl;
    std::cout << "========================================" << std::endl;

    // --------------------------------------------------
    // 步骤 1: 初始化 Winsock
    //    所有 Windows 网络编程的第一步。
    //    MAKEWORD(2,2) 表示请求 Winsock 2.2 版本。
    // --------------------------------------------------
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        print_error("WSAStartup");
        return 1;
    }
    std::cout << "[OK] Winsock initialized." << std::endl;

    // --------------------------------------------------
    // 步骤 2: 创建 socket
    //    AF_INET     = IPv4 协议族
    //    SOCK_STREAM = TCP（可靠、有序、面向连接）
    //    0           = 自动选择协议（TCP）
    //    socket() 返回一个"文件描述符"，后续所有操作都用它
    // --------------------------------------------------
    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        print_error("socket");
        WSACleanup();
        return 1;
    }
    std::cout << "[OK] Socket created." << std::endl;

    // --------------------------------------------------
    // 步骤 3: bind — 将 socket 绑定到指定的地址和端口
    //    sin_addr.s_addr = INADDR_ANY 表示监听所有网卡
    //    htons() 将主机字节序转为网络字节序（大端）
    //    网络协议规定端口号必须是大端字节序
    // --------------------------------------------------
    sockaddr_in server_addr{};
    server_addr.sin_family      = AF_INET;          // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY;        // 监听所有网卡 (0.0.0.0)
    server_addr.sin_port        = htons(SERVER_PORT); // 端口号 → 网络字节序

    if (bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        print_error("bind");
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }
    std::cout << "[OK] Bound to port " << SERVER_PORT << "." << std::endl;

    // --------------------------------------------------
    // 步骤 4: listen — 开始监听连接
    //    BACKLOG = 等待队列的长度
    //    超过这个数量的连接请求会被拒绝
    // --------------------------------------------------
    if (listen(server_socket, BACKLOG) == SOCKET_ERROR) {
        print_error("listen");
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }
    std::cout << "[OK] Listening for connections..." << std::endl;

    // --------------------------------------------------
    // 步骤 5: accept — 接受客户端连接（阻塞等待）
    //    accept() 会阻塞直到有客户端连接进来
    //    返回一个新的 socket，用于和这个客户端通信
    //    原来的 server_socket 继续监听新连接
    // --------------------------------------------------
    std::cout << "[...] Waiting for a client..." << std::endl;

    sockaddr_in client_addr{};
    int client_addr_len = sizeof(client_addr);
    SOCKET client_socket = accept(server_socket,
                                   (sockaddr*)&client_addr,
                                   &client_addr_len);
    if (client_socket == INVALID_SOCKET) {
        print_error("accept");
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    // inet_ntoa 将 IP 地址转为可读字符串
    std::cout << "[OK] Client connected from "
              << inet_ntoa(client_addr.sin_addr) << ":"
              << ntohs(client_addr.sin_port) << std::endl;

    // --------------------------------------------------
    // 步骤 6: Echo 循环 — 收什么就回什么
    //    recv() 读取客户端发来的数据
    //    send() 把同样的数据发回去
    //    当 recv 返回 0 时表示客户端断开连接
    // --------------------------------------------------
    char buffer[BUFFER_SIZE];
    int total_echoes = 0;

    while (true) {
        // 接收数据（阻塞等待）
        int bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';  // 字符串结尾（方便打印）
            std::cout << "[RECV] " << bytes_received << " bytes: "
                      << buffer << std::endl;

            // 原样发回
            int bytes_sent = send(client_socket, buffer, bytes_received, 0);
            if (bytes_sent == SOCKET_ERROR) {
                print_error("send");
                break;
            }
            std::cout << "[SEND] Echoed " << bytes_sent << " bytes back." << std::endl;
            total_echoes++;
        }
        else if (bytes_received == 0) {
            // recv 返回 0 = 对方正常关闭连接
            std::cout << "[INFO] Client disconnected." << std::endl;
            break;
        }
        else {
            // recv 返回负数 = 发生错误
            print_error("recv");
            break;
        }
    }

    std::cout << "[DONE] Total echoes: " << total_echoes << std::endl;

    // --------------------------------------------------
    // 步骤 7: 清理资源
    // --------------------------------------------------
    closesocket(client_socket);
    closesocket(server_socket);
    WSACleanup();
    std::cout << "[OK] Server shutdown cleanly." << std::endl;

    return 0;
}
