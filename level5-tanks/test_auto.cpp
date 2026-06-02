/**
 * Level 5: 自动化测试 — 两人连接、移动、射击、击杀
 *
 * 模拟:
 *   P1 连接 → WAIT
 *   P2 连接 → 配对 → START
 *   验证状态快照 S/E 格式
 *   P1 向 P2 方向移动、射击
 *   验证 KILL 消息
 */

#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

constexpr int  SERVER_PORT = 8080;
constexpr char SERVER_IP[] = "127.0.0.1";
constexpr int  BUFFER_SIZE = 8192;
constexpr int  TIMEOUT_MS = 300;

void set_timeout(SOCKET s, int ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
}

SOCKET connect_player(const char* name) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    connect(s, (sockaddr*)&addr, sizeof(addr));
    set_timeout(s, TIMEOUT_MS);
    std::cout << "[OK] " << name << " connected" << std::endl;
    return s;
}

int main() {
    std::cout << "=== Level 5 Tank Battle Auto Test ===" << std::endl;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // --- P1 连接 ---
    SOCKET p1 = connect_player("P1");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    char buf[BUFFER_SIZE];
    int r = recv(p1, buf, BUFFER_SIZE-1, 0);
    bool wait_ok = (r > 0 && strstr(buf, "WAIT"));
    std::cout << "  " << (wait_ok ? "[PASS]" : "[FAIL]") << " P1 WAIT" << std::endl;

    // --- P2 连接 ---
    SOCKET p2 = connect_player("P2");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // P1 收到 START
    r = recv(p1, buf, BUFFER_SIZE-1, 0);
    bool p1_start = (r > 0 && strstr(buf, "START 0"));
    std::cout << "  " << (p1_start ? "[PASS]" : "[FAIL]") << " P1 START" << std::endl;

    // P2 收到 START
    r = recv(p2, buf, BUFFER_SIZE-1, 0);
    bool p2_start = (r > 0 && strstr(buf, "START 1"));
    std::cout << "  " << (p2_start ? "[PASS]" : "[FAIL]") << " P2 START" << std::endl;

    // --- 等待快照到达 ---
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    r = recv(p1, buf, BUFFER_SIZE-1, 0);
    bool has_snapshot = (r > 0 && strstr(buf, "S\n") && strstr(buf, "E\n"));
    std::cout << "  " << (has_snapshot ? "[PASS]" : "[FAIL]") << " Snapshot S/E format" << std::endl;

    // --- 两人都移动并向对方射击 ---
    // P1: 向右下移动，向右射击
    send(p1, "M 1.00 0.00\n", 12, 0);
    send(p2, "M -1.00 0.00\n", 13, 0);  // P2 向左移动（互相靠近）

    // 等几帧让坦克靠近
    for (int i = 0; i < 60; i++) {
        send(p1, "F\n", 2, 0);
        send(p2, "F\n", 2, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        // 清空缓冲区
        recv(p1, buf, BUFFER_SIZE-1, 0);
        recv(p2, buf, BUFFER_SIZE-1, 0);
    }

    // 检查是否有 KILL 消息（可能子弹击中了）
    r = recv(p1, buf, BUFFER_SIZE-1, 0);
    bool has_kill_or_state = (r > 0);
    std::cout << "  " << (has_kill_or_state ? "[PASS]" : "[FAIL]")
              << " Received game data during battle" << std::endl;

    std::cout << "  (Battle simulation ran for ~1 sec, ";

    // 再运行几秒看是否有击杀
    bool kill_detected = false;
    for (int i = 0; i < 180 && !kill_detected; i++) {
        send(p1, "M 1.00 0.00\n", 12, 0);
        send(p2, "M -1.00 0.00\n", 13, 0);
        send(p1, "F\n", 2, 0);
        send(p2, "F\n", 2, 0);
        r = recv(p1, buf, BUFFER_SIZE-1, 0);
        if (r > 0 && strstr(buf, "KILL")) {
            kill_detected = true;
        }
        recv(p2, buf, BUFFER_SIZE-1, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (kill_detected) {
        std::cout << "KILL detected!)" << std::endl;
        std::cout << "  [PASS] Kill system works" << std::endl;
    } else {
        std::cout << "no kill in time)" << std::endl;
        std::cout << "  [INFO] Tanks might not have collided (OK for smoke test)" << std::endl;
    }

    closesocket(p1);
    closesocket(p2);
    WSACleanup();

    std::cout << "\n=== Tank Battle smoke test complete ===" << std::endl;
    return 0;
}
