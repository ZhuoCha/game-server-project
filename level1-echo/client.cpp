/**
 * Level 1: Echo 客户端
 *
 * 用于测试 Echo 服务器的简单客户端。
 * 发送一条消息，接收回显，验证无误后退出。
 *
 * 学习目标：
 *   1. 理解客户端 socket 流程: socket() → connect()
 *   2. 理解 connect() 如何与服务器的 listen/accept 配对
 *   3. 理解 send/recv 的对称性
 */

#include <iostream>
#include <string>
#include <cstring>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

constexpr int  SERVER_PORT = 8080;
constexpr char SERVER_IP[] = "127.0.0.1";  // 本机测试
constexpr int  BUFFER_SIZE = 4096;

void print_error(const char* where) {
    std::cerr << "[ERROR] " << where
              << " failed, code=" << WSAGetLastError() << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Level 1: Echo Client" << std::endl;
    std::cout << "========================================" << std::endl;

    // --------------------------------------------------
    // 步骤 1: 初始化 Winsock
    // --------------------------------------------------
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        print_error("WSAStartup");
        return 1;
    }

    // --------------------------------------------------
    // 步骤 2: 创建 socket（和服务器一样）
    // --------------------------------------------------
    SOCKET client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == INVALID_SOCKET) {
        print_error("socket");
        WSACleanup();
        return 1;
    }
    std::cout << "[OK] Socket created." << std::endl;

    // --------------------------------------------------
    // 步骤 3: connect — 连接到服务器
    //    inet_addr() 将 IP 字符串转为网络字节序整数
    //    htons() 转换端口号
    //    connect() 发起 TCP 三次握手
    // --------------------------------------------------
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    std::cout << "[...] Connecting to " << SERVER_IP << ":" << SERVER_PORT << "..." << std::endl;

    if (connect(client_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        print_error("connect");
        closesocket(client_socket);
        WSACleanup();
        return 1;
    }
    std::cout << "[OK] Connected to server!" << std::endl;

    // --------------------------------------------------
    // 步骤 4: 发送多条测试消息
    // --------------------------------------------------
    const char* test_messages[] = {
        "Hello, Game Server!",
        "ping",
        "Echo test: 12345",
        "quit"  // 服务器回显后，客户端就退出
    };

    char buffer[BUFFER_SIZE];
    for (int i = 0; i < 4; i++) {
        const char* msg = test_messages[i];
        int msg_len = (int)strlen(msg);

        // 发送消息
        std::cout << "[SEND] \"" << msg << "\" (" << msg_len << " bytes)" << std::endl;
        int sent = send(client_socket, msg, msg_len, 0);
        if (sent == SOCKET_ERROR) {
            print_error("send");
            break;
        }

        // 接收回显
        int received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        if (received > 0) {
            buffer[received] = '\0';
            std::cout << "[RECV] \"" << buffer << "\" (" << received << " bytes)" << std::endl;

            // 验证回显是否正确
            if (received == msg_len && memcmp(buffer, msg, msg_len) == 0) {
                std::cout << "       ✓ Echo matches!" << std::endl;
            } else {
                std::cout << "       ✗ Echo MISMATCH!" << std::endl;
            }
        } else if (received == 0) {
            std::cout << "[INFO] Server closed connection." << std::endl;
            break;
        } else {
            print_error("recv");
            break;
        }
    }

    // --------------------------------------------------
    // 步骤 5: 清理
    // --------------------------------------------------
    closesocket(client_socket);
    WSACleanup();
    std::cout << "[OK] Client shutdown." << std::endl;

    return 0;
}
