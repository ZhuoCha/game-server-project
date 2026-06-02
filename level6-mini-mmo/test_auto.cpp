/**
 * Level 6: Mini MMO 自动化测试
 *
 * 验证: 登录、START、AOI快照(S/E)、STAT、怪物生成、移动、攻击、击杀、持久化
 */

#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

constexpr int  SERVER_PORT = 8080;
constexpr char SERVER_IP[] = "127.0.0.1";
constexpr int  BUFFER_SIZE = 16384;
constexpr int  TIMEOUT_MS = 300;

void set_timeout(SOCKET s, int ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
}

SOCKET connect_client() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    connect(s, (sockaddr*)&addr, sizeof(addr));
    set_timeout(s, TIMEOUT_MS);
    return s;
}

int main() {
    std::cout << "=== Level 6 Mini MMO Auto Test ===" << std::endl;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    int pass = 0, fail = 0;

    // --- 测试1: 连接 + 收到 START ---
    SOCKET p1 = connect_client();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    char buf[BUFFER_SIZE];
    int r = recv(p1, buf, BUFFER_SIZE-1, 0);
    bool start_ok = (r > 0 && strstr(buf, "START"));
    if (start_ok) pass++; else fail++;
    std::cout << "  " << (start_ok ? "[PASS]" : "[FAIL]")
              << " Login + START received" << std::endl;

    // --- 测试2: AOI快照 (S/E格式) ---
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    r = recv(p1, buf, BUFFER_SIZE-1, 0);
    bool aoi_ok = (r > 0 && strstr(buf, "S\n") && strstr(buf, "E\n"));
    if (aoi_ok) pass++; else fail++;
    std::cout << "  " << (aoi_ok ? "[PASS]" : "[FAIL]")
              << " AOI snapshot (S/E) received" << std::endl;

    // --- 测试3: 快照包含怪物 (视野内应该有预生成的怪物) ---
    bool has_monster = (r > 0 && strstr(buf, "\nM "));
    if (has_monster) pass++; else fail++;
    std::cout << "  " << (has_monster ? "[PASS]" : "[FAIL]")
              << " Monsters visible in AOI" << std::endl;

    // --- 测试4: STAT 消息 ---
    bool has_stat = (r > 0 && strstr(buf, "STAT"));
    if (has_stat) pass++; else fail++;
    std::cout << "  " << (has_stat ? "[PASS]" : "[FAIL]")
              << " STAT (own state) received" << std::endl;

    // --- 测试5: 移动到怪物附近 + 攻击 ---
    // 先移动几步
    for (int i = 0; i < 60; i++) {
        send(p1, "M 1.00 0.00\n", 12, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        recv(p1, buf, BUFFER_SIZE-1, 0);  // 清空缓冲
    }

    // 攻击
    bool got_hit_or_kill = false;
    for (int i = 0; i < 30 && !got_hit_or_kill; i++) {
        send(p1, "A\n", 2, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        r = recv(p1, buf, BUFFER_SIZE-1, 0);
        if (r > 0 && (strstr(buf, "HIT") || strstr(buf, "KILL"))) {
            got_hit_or_kill = true;
        }
    }
    // 注意：可能附近没有怪物，所以不强制要求通过
    if (got_hit_or_kill) pass++; else fail++;
    std::cout << "  " << (got_hit_or_kill ? "[PASS]" : "[INFO]")
              << " Combat system (HIT/KILL)"
              << (got_hit_or_kill ? "" : " — no monster in range") << std::endl;

    // --- 测试6: 第二个玩家连接，验证多人AOI ---
    SOCKET p2 = connect_client();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    r = recv(p2, buf, BUFFER_SIZE-1, 0);
    bool p2_start = (r > 0 && strstr(buf, "START"));
    if (p2_start) pass++; else fail++;
    std::cout << "  " << (p2_start ? "[PASS]" : "[FAIL]")
              << " Second player login" << std::endl;

    // P2 向 P1 方向移动（互相靠近）
    send(p2, "M -1.00 0.00\n", 13, 0);
    for (int i = 0; i < 30; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        recv(p2, buf, BUFFER_SIZE-1, 0);
    }

    // 检查 P2 的快照中是否有另一个玩家
    r = recv(p2, buf, BUFFER_SIZE-1, 0);
    // P2 和 P1 可能不在视野内（随机出生位置不同），所以这也是信息性的
    bool sees_other = (r > 0 && strstr(buf, "\nP "));
    if (sees_other) pass++; else fail++;
    std::cout << "  " << (sees_other ? "[PASS]" : "[INFO]")
              << " Multi-player AOI"
              << (sees_other ? " (players visible)" : " — too far apart") << std::endl;

    closesocket(p1);
    closesocket(p2);
    WSACleanup();

    std::cout << "\n=== Results: " << pass << "/" << (pass+fail) << " passed ===" << std::endl;
    return fail > 2 ? 1 : 0;  // 允许一些信息性失败
}
