/**
 * Level 3: 自动化测试 — 模拟 X 获胜的一局
 *
 * 棋盘坐标:     X 的三连: 2-4-6 对角线
 *   0 | 1 | 2
 *  ---+---+---
 *   3 | 4 | 5
 *  ---+---+---
 *   6 | 7 | 8
 *
 * 走法: X(4) O(0) X(2) O(8) X(6) → X 对角线三连！
 */

#include <iostream>
#include <string>
#include <cstring>
#include <thread>
#include <chrono>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

constexpr int  SERVER_PORT = 8080;
constexpr char SERVER_IP[] = "127.0.0.1";
constexpr int  BUFFER_SIZE = 4096;

void print_error(const char* where) {
    std::cerr << "[ERROR] " << where
              << " failed, code=" << WSAGetLastError() << std::endl;
}

/** 阻塞 recv（带 500ms 超时） */
int recv_timeout(SOCKET sock, char* buf, int size) {
    int timeout_ms = 500;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&timeout_ms, sizeof(timeout_ms));
    int r = recv(sock, buf, size - 1, 0);
    if (r > 0) buf[r] = '\0';
    return r;
}

SOCKET connect_player(const char* name) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(SERVER_PORT);
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        print_error(name);
        return INVALID_SOCKET;
    }
    std::cout << "[OK] " << name << " connected." << std::endl;
    return sock;
}

int main() {
    std::cout << "=== Level 3 Tic-Tac-Toe Auto Test ===" << std::endl;

    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);

    // --- P1 连接，收到 WAIT ---
    SOCKET p1 = connect_player("P1");
    if (p1 == INVALID_SOCKET) return 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    char buf[BUFFER_SIZE];
    int r = recv_timeout(p1, buf, BUFFER_SIZE);
    std::cout << "  P1: " << (r > 0 ? buf : "(no data)") << std::endl;
    bool p1_wait = (r > 0 && strstr(buf, "WAIT"));
    std::cout << "  " << (p1_wait ? "[PASS]" : "[FAIL]") << " P1 received WAIT" << std::endl;

    // --- P2 连接，触发配对 ---
    SOCKET p2 = connect_player("P2");
    if (p2 == INVALID_SOCKET) return 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // P1 收到 START + BOARD
    r = recv_timeout(p1, buf, BUFFER_SIZE);
    std::cout << "  P1: " << (r > 0 ? buf : "(no data)") << std::endl;
    bool p1_start = (r > 0 && strstr(buf, "START X"));
    std::cout << "  " << (p1_start ? "[PASS]" : "[FAIL]") << " P1 got START X" << std::endl;

    // P2 收到 START + BOARD
    r = recv_timeout(p2, buf, BUFFER_SIZE);
    std::cout << "  P2: " << (r > 0 ? buf : "(no data)") << std::endl;
    bool p2_start = (r > 0 && strstr(buf, "START O"));
    std::cout << "  " << (p2_start ? "[PASS]" : "[FAIL]") << " P2 got START O" << std::endl;

    // --- 走子序列: P1(X) P2(O) P1(X) P2(O) P1(X) 五步决胜负 ---
    struct Step { SOCKET sock; int pos; const char* expect; };
    Step steps[] = {
        {p1, 4, "OK"},     // X→中心
        {p2, 0, "OK"},     // O→左上角
        {p1, 2, "OK"},     // X→右上角
        {p2, 8, "OK"},     // O→右下角
        {p1, 6, "WIN"},    // X→左下角 → 2-4-6 对角线三连！
    };

    int pass = 0, fail = 0;
    for (int i = 0; i < 5; i++) {
        // 发送落子命令
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "MOVE %d\n", steps[i].pos);
        send(steps[i].sock, cmd, (int)strlen(cmd), 0);

        // 等待服务器响应（需要等一下让服务器处理）
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 读取响应
        r = recv_timeout(steps[i].sock, buf, BUFFER_SIZE);
        if (r > 0) {
            // 去除换行方便显示
            for (int j = 0; buf[j]; j++) if (buf[j] == '\n') buf[j] = ' ';
            std::cout << "  Move " << steps[i].pos << ": " << buf << std::endl;
        }

        bool ok = (r > 0 && strstr(buf, steps[i].expect));
        if (ok) pass++; else fail++;
        std::cout << "  " << (ok ? "[PASS]" : "[FAIL]")
                  << " Move " << steps[i].pos << " expected '"
                  << steps[i].expect << "'" << std::endl;
    }

    // --- 最终结果：P2 应该收到 LOSE ---
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    r = recv_timeout(p2, buf, BUFFER_SIZE);
    if (r > 0) {
        for (int j = 0; buf[j]; j++) if (buf[j] == '\n') buf[j] = ' ';
        std::cout << "  P2 final: " << buf << std::endl;
    }
    bool p2_lose = (r > 0 && strstr(buf, "LOSE"));
    if (p2_lose) pass++; else fail++;
    std::cout << "  " << (p2_lose ? "[PASS]" : "[FAIL]") << " P2 received LOSE" << std::endl;

    closesocket(p1);
    closesocket(p2);
    WSACleanup();

    std::cout << "\n=== Results: " << pass << "/" << (pass+fail) << " passed ===" << std::endl;
    return fail > 0 ? 1 : 0;
}
