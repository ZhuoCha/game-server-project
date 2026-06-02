/**
 * Level 2: 自动化测试客户端
 *
 * 发送预设消息序列，验证服务器响应。
 * 用于 CI/冒烟测试，不参与交互。
 */

#include <iostream>
#include <string>
#include <cstring>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

constexpr int  SERVER_PORT = 8080;
constexpr char SERVER_IP[] = "127.0.0.1";
constexpr int  BUFFER_SIZE = 4096;

void print_error(const char* where) {
    std::cerr << "[ERROR] " << where
              << " failed, code=" << WSAGetLastError() << std::endl;
}

int main() {
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(SERVER_PORT);
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        print_error("connect");
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    std::cout << "[OK] Connected." << std::endl;

    // 等待欢迎消息
    char buffer[BUFFER_SIZE];
    Sleep(200);  // 给服务器一点时间发送欢迎消息
    int bytes = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        std::cout << "[RECV] Welcome:\n" << buffer << std::endl;
    }

    // 测试 /help 命令
    const char* help_cmd = "/help\n";
    send(sock, help_cmd, (int)strlen(help_cmd), 0);
    Sleep(100);
    bytes = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        std::cout << "[RECV] /help response:\n" << buffer << std::endl;
    }

    // 测试 /name 命令
    const char* name_cmd = "/name TestBot\n";
    send(sock, name_cmd, (int)strlen(name_cmd), 0);
    Sleep(100);

    // 测试普通消息
    const char* chat_msg = "Hello everyone!\n";
    send(sock, chat_msg, (int)strlen(chat_msg), 0);
    Sleep(100);

    // 测试 /quit
    const char* quit_cmd = "/quit\n";
    send(sock, quit_cmd, (int)strlen(quit_cmd), 0);
    Sleep(100);
    bytes = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        std::cout << "[RECV] Goodbye:\n" << buffer << std::endl;
    }

    std::cout << "[OK] All tests passed!" << std::endl;

    closesocket(sock);
    WSACleanup();
    return 0;
}
