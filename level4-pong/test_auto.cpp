/**
 * Level 4: 自动化冒烟测试
 *
 * 模拟 P1 把球拍放中间挡球，P2 球拍放上面让球通过。
 * 验证: STATE 消息正常到达、比分更新、游戏结束通知。
 *
 * 这是实时游戏测试，使用 recv 超时来处理异步消息流。
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
constexpr int  RECV_TIMEOUT_MS = 200;

void set_timeout(SOCKET s, int ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
}

int main() {
    std::cout << "=== Level 4 Pong Smoke Test ===" << std::endl;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // --- 连接 P1 ---
    SOCKET p1 = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SERVER_PORT);
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    connect(p1, (sockaddr*)&addr, sizeof(addr));
    set_timeout(p1, RECV_TIMEOUT_MS);
    std::cout << "[OK] P1 connected" << std::endl;

    // P1 收到 WAIT
    char buf[BUFFER_SIZE];
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int r = recv(p1, buf, BUFFER_SIZE-1, 0);
    std::cout << "  P1: " << (r > 0 ? buf : "(timeout)") << std::endl;
    bool wait_ok = (r > 0 && strstr(buf, "WAIT"));
    std::cout << "  " << (wait_ok ? "[PASS]" : "[FAIL]") << " WAIT received" << std::endl;

    // --- 连接 P2（触发配对）---
    SOCKET p2 = socket(AF_INET, SOCK_STREAM, 0);
    connect(p2, (sockaddr*)&addr, sizeof(addr));
    set_timeout(p2, RECV_TIMEOUT_MS);
    std::cout << "[OK] P2 connected" << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // P1 收到 START + STATE
    r = recv(p1, buf, BUFFER_SIZE-1, 0);
    std::cout << "  P1 START: " << (r > 0 ? buf : "(timeout)") << std::endl;
    bool start_ok = (r > 0 && strstr(buf, "START 1"));
    std::cout << "  " << (start_ok ? "[PASS]" : "[FAIL]") << " P1 got START 1" << std::endl;

    // P2 收到 START + STATE
    r = recv(p2, buf, BUFFER_SIZE-1, 0);
    std::cout << "  P2 START: " << (r > 0 ? buf : "(timeout)") << std::endl;
    bool start2_ok = (r > 0 && strstr(buf, "START 2"));
    std::cout << "  " << (start2_ok ? "[PASS]" : "[FAIL]") << " P2 got START 2" << std::endl;

    // --- 模拟对局: P1 挡球（拍子在中央 y=300），P2 放水（拍子在顶部 y=40）---
    // 球初始向右发射，P1 在左侧 → 球碰到 P1 后会反弹向右 → P2 不在位置 → P1 得分
    int pass = wait_ok && start_ok && start2_ok ? 3 : 0;
    int fail = (wait_ok && start_ok && start2_ok) ? 0 : 3;
    bool scored = false, game_ended = false;
    float last_score1 = 0, last_score2 = 0;

    auto start_time = std::chrono::steady_clock::now();
    constexpr int TEST_DURATION_SEC = 8;

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count();
        if (elapsed > TEST_DURATION_SEC) break;

        // P1: paddle at center (y=300) — should hit the ball
        send(p1, "P 300.0\n", 8, 0);

        // P2: paddle at top (y=40) — ball passes through
        send(p2, "P 40.0\n", 7, 0);

        // Read STATE from P1
        r = recv(p1, buf, BUFFER_SIZE-1, 0);
        if (r > 0) {
            buf[r] = '\0';
            // Parse last STATE line
            char* last_s = nullptr;
            char* line = strtok(buf, "\n");
            while (line) {
                if (line[0] == 'S') last_s = line;
                if (line[0] == 'W') {
                    game_ended = true;
                    last_s = line;
                }
                line = strtok(nullptr, "\n");
            }
            if (last_s && last_s[0] == 'S') {
                float bx, by, p1y, p2y;
                int s1, s2;
                sscanf(last_s+2, "%f %f %f %f %d %d",
                       &bx, &by, &p1y, &p2y, &s1, &s2);
                if (s1 > last_score1) {
                    std::cout << "  [SCORE] P1 scored! " << s1 << "-" << s2
                              << " (ball at " << (int)bx << "," << (int)by << ")" << std::endl;
                    scored = true;
                }
                last_score1 = s1;
                last_score2 = s2;
            }
            if (last_s && last_s[0] == 'W') {
                std::cout << "  [GAME] Winner: " << last_s << std::endl;
            }
        }

        // Flush P2's buffer too
        recv(p2, buf, BUFFER_SIZE-1, 0);

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (scored || game_ended) pass++; else fail++;
    std::cout << "  " << (scored || game_ended ? "[PASS]" : "[FAIL]")
              << " Score changed or game ended (real-time sync works)" << std::endl;

    // Check STATE messages arrived
    bool state_ok = (last_score1 > 0 || game_ended);
    // Actually, the ball might not have reached yet. Let's check if we at least got data.
    if (state_ok) pass++; else fail++;
    std::cout << "  " << (state_ok ? "[PASS]" : "[WARN]")
              << " Score progress detected" << std::endl;

    closesocket(p1);
    closesocket(p2);
    WSACleanup();

    std::cout << "\n=== Results: " << pass << "/" << (pass+fail) << " passed ===" << std::endl;
    return fail > 0 ? 1 : 0;
}
