/**
 * Level 2: 聊天室客户端
 *
 * 使用双线程模型：
 *   - 接收线程: 循环 recv()，收到消息就打印
 *   - 发送线程: 循环读取键盘输入，发送给服务器
 *
 * 新增学习目标：
 *   1. 理解为什么客户端不能用 select() 同时等 stdin 和 socket
 *      → Windows 的 select() 只支持 socket，不支持普通文件描述符
 *   2. 掌握 std::thread 的基本用法
 *   3. 理解线程间的同步问题（这里用 atomic<bool> 做退出信号）
 *   4. 理解"发送"和"接收"是两个独立的通道（全双工通信）
 *
 * 运行方式：
 *   ./chat_client.exe
 *   然后输入消息按回车发送，/quit 退出
 */

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

constexpr int  SERVER_PORT = 8080;
constexpr char SERVER_IP[] = "127.0.0.1";
constexpr int  BUFFER_SIZE = 4096;

// 全局退出标志 — 任何一个线程出问题，另一个也要停下来
std::atomic<bool> running(true);

void print_error(const char* where) {
    std::cerr << "[ERROR] " << where
              << " failed, code=" << WSAGetLastError() << std::endl;
}

// ============================================================
// 接收线程：不停从服务器收消息并显示
// ============================================================
void recv_thread(SOCKET sock) {
    char buffer[BUFFER_SIZE];

    while (running) {
        int bytes = recv(sock, buffer, BUFFER_SIZE - 1, 0);

        if (bytes > 0) {
            buffer[bytes] = '\0';
            std::cout << buffer;          // 服务器发来的消息自带换行
            std::cout.flush();            // 立即刷新，避免显示延迟
        }
        else if (bytes == 0) {
            std::cout << "\n[SERVER] Connection closed by server." << std::endl;
            running = false;
            break;
        }
        else {
            // recv 错误（非阻塞模式下可能返回 WSAEWOULDBLOCK，
            // 但我们用的是阻塞模式，所以错误就意味着真出问题了）
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                print_error("recv");
                running = false;
                break;
            }
        }
    }
}

// ============================================================
// 主函数
// ============================================================
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Level 2: Chat Client" << std::endl;
    std::cout << "  Commands: /help /list /name /quit" << std::endl;
    std::cout << "========================================" << std::endl;

    // --------------------------------------------------
    // 步骤 1: 初始化 + 连接（和 Level 1 一样）
    // --------------------------------------------------
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        print_error("WSAStartup");
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        print_error("socket");
        WSACleanup();
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    std::cout << "[...] Connecting to " << SERVER_IP
              << ":" << SERVER_PORT << "..." << std::endl;

    if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        print_error("connect");
        closesocket(sock);
        WSACleanup();
        return 1;
    }
    std::cout << "[OK] Connected! Type a message and press Enter." << std::endl;
    std::cout << "----------------------------------------" << std::endl;

    // --------------------------------------------------
    // 步骤 2: 启动接收线程
    //    主线程负责发消息，子线程负责收消息。
    //    这样用户打字时不会错过别人发来的消息。
    // --------------------------------------------------
    std::thread recv(recv_thread, sock);

    // --------------------------------------------------
    // 步骤 3: 主线程 — 读取键盘输入并发送
    // --------------------------------------------------
    std::string line;
    while (running && std::getline(std::cin, line)) {
        if (line.empty()) continue;

        // 添加换行符，服务器按行处理
        line += "\n";

        int sent = send(sock, line.c_str(), (int)line.length(), 0);
        if (sent == SOCKET_ERROR) {
            print_error("send");
            running = false;
            break;
        }

        // 本地输入 /quit 也要退出
        if (line == "/quit\n") {
            running = false;
            break;
        }
    }

    // --------------------------------------------------
    // 步骤 4: 清理
    // --------------------------------------------------
    running = false;
    closesocket(sock);
    WSACleanup();

    if (recv.joinable()) {
        recv.join();  // 等待接收线程结束
    }

    std::cout << "[OK] Client shutdown." << std::endl;
    return 0;
}
