/**
 * Level 4: 乒乓游戏客户端
 *
 * 实时渲染 + 非阻塞键盘输入。
 *
 * 新增学习目标：
 *   1. ANSI 转义序列渲染: \033[2J 清屏, \033[H 光标复位 → 60fps 无闪烁
 *   2. GetAsyncKeyState: Windows 非阻塞键盘轮询（比 _kbhit 更适合游戏）
 *   3. 坐标映射: 虚拟坐标(800×600) → 控制台字符网格(60×25)
 *   4. 客户端预测: 立即响应键盘，不等服务器确认
 *
 * 控制方式：
 *   玩家1: W/S 键
 *   玩家2: ↑/↓ 键
 */

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <winsock2.h>
#include <windows.h>   // GetAsyncKeyState, SetConsoleMode

#pragma comment(lib, "ws2_32.lib")

constexpr int   SERVER_PORT = 8080;
constexpr char  SERVER_IP[] = "127.0.0.1";
constexpr int   BUFFER_SIZE = 1024;

// 显示尺寸（控制台字符网格）
constexpr int   DISPLAY_W = 62;   // 玩区域宽度（含边框）
constexpr int   DISPLAY_H = 26;   // 玩区域高度（含边框）

// 虚拟场地尺寸（与服务器一致）
constexpr float COURT_W = 800.0f;
constexpr float COURT_H = 600.0f;
constexpr float PADDLE_W = 12.0f;
constexpr float PADDLE_H = 80.0f;
constexpr float BALL_SIZE = 12.0f;
constexpr float PADDLE_SPEED = 450.0f;

std::atomic<bool> running(true);
SOCKET g_sock = INVALID_SOCKET;

// 本地游戏状态（从服务器同步）
float ball_x = COURT_W/2, ball_y = COURT_H/2;
float paddle1_y = COURT_H/2, paddle2_y = COURT_H/2;
int   score1 = 0, score2 = 0;
int   my_player_id = 0;       // 1 或 2
char  game_result[64] = "";   // "WIN" "LOSE" 或 ""
bool  in_game = false;

// ============================================================
// 坐标映射: 虚拟坐标 → 控制台字符坐标
// ============================================================
int x_to_col(float x) {
    // 玩区域映射到列 1..DISPLAY_W-2
    float ratio = x / COURT_W;
    return 1 + (int)(ratio * (DISPLAY_W - 2));
}

int y_to_row(float y) {
    float ratio = y / COURT_H;
    return 1 + (int)(ratio * (DISPLAY_H - 2));
}

// ============================================================
// ANSI 渲染
// ============================================================
void render_frame() {
    // 构建字符缓冲区（比逐行打印快得多，减少闪烁）
    char screen[DISPLAY_H][DISPLAY_W + 2];  // +2 for \n\0

    // 清空
    for (int r = 0; r < DISPLAY_H; r++) {
        for (int c = 0; c < DISPLAY_W; c++) {
            screen[r][c] = ' ';
        }
        screen[r][DISPLAY_W]   = '\n';
        screen[r][DISPLAY_W+1] = '\0';
    }

    // 画上下边框
    for (int c = 0; c < DISPLAY_W; c++) {
        screen[0][c] = '=';
        screen[DISPLAY_H-1][c] = '=';
    }

    // 画左右边框
    for (int r = 0; r < DISPLAY_H; r++) {
        screen[r][0] = '|';
        screen[r][DISPLAY_W-1] = '|';
    }

    // 画中线（虚线）
    int mid_col = DISPLAY_W / 2;
    for (int r = 1; r < DISPLAY_H - 1; r += 2) {
        screen[r][mid_col] = ':';
    }

    // 画球
    int ball_col = x_to_col(ball_x);
    int ball_row = y_to_row(ball_y);
    if (ball_col >= 1 && ball_col < DISPLAY_W - 1 &&
        ball_row >= 1 && ball_row < DISPLAY_H - 1) {
        screen[ball_row][ball_col] = 'O';
    }

    // 画球拍1（左）
    int p1_col = x_to_col(30.0f);  // 球拍 X 位置
    int p1_row = y_to_row(paddle1_y);
    int half_h = (int)(PADDLE_H / COURT_H * (DISPLAY_H - 2) / 2);
    for (int r = p1_row - half_h; r <= p1_row + half_h; r++) {
        if (r >= 1 && r < DISPLAY_H - 1) {
            screen[r][p1_col] = '#';
        }
    }

    // 画球拍2（右）
    int p2_col = x_to_col(COURT_W - 30.0f);
    int p2_row = y_to_row(paddle2_y);
    for (int r = p2_row - half_h; r <= p2_row + half_h; r++) {
        if (r >= 1 && r < DISPLAY_H - 1) {
            screen[r][p2_col] = '#';
        }
    }

    // --- 输出 ---
    // ANSI: 光标复位（不清屏，直接覆盖上一帧）
    std::cout << "\033[H";

    // 分数栏
    std::cout << "  P1: " << score1;
    for (int i = 0; i < DISPLAY_W - 20; i++) std::cout << ' ';
    std::cout << "P2: " << score2 << std::endl;

    // 游戏区域
    for (int r = 0; r < DISPLAY_H; r++) {
        std::cout << screen[r];
    }

    // 状态消息
    std::cout << std::endl;
    if (game_result[0]) {
        if (strcmp(game_result, "WIN") == 0) {
            std::cout << "  ★ ★ ★  YOU WIN!  ★ ★ ★" << std::endl;
        } else {
            std::cout << "  ... You lose. Waiting for next game..." << std::endl;
        }
    } else if (!in_game) {
        std::cout << "  Waiting for opponent..." << std::endl;
    } else {
        std::cout << "  P1: W/S    P2: Arrow Up/Down" << std::endl;
    }

    std::cout.flush();
}

// ============================================================
// 接收线程：接收服务器状态快照
// ============================================================
void recv_thread() {
    char buf[BUFFER_SIZE];

    while (running) {
        int bytes = recv(g_sock, buf, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) {
            running = false;
            break;
        }

        buf[bytes] = '\0';

        // 按行分割（可能一次 recv 收到多行）
        char* line = strtok(buf, "\n");
        while (line) {
            if (line[0] == 'S' && line[1] == ' ') {
                // 状态快照: S bx by p1y p2y s1 s2
                sscanf(line + 2, "%f %f %f %f %d %d",
                       &ball_x, &ball_y,
                       &paddle1_y, &paddle2_y,
                       &score1, &score2);
                in_game = true;
                game_result[0] = '\0';
            }
            else if (line[0] == 'W' && line[1] == ' ') {
                int winner;
                sscanf(line + 2, "%d", &winner);
                if (winner == my_player_id) {
                    strcpy(game_result, "WIN");
                } else {
                    strcpy(game_result, "LOSE");
                }
                in_game = false;
            }
            else if (strncmp(line, "WAIT", 4) == 0) {
                in_game = false;
                game_result[0] = '\0';
            }
            else if (strncmp(line, "START ", 6) == 0) {
                sscanf(line + 6, "%d", &my_player_id);
                in_game = true;
                game_result[0] = '\0';
                score1 = score2 = 0;
            }

            line = strtok(nullptr, "\n");
        }

        // 收到任何更新后立即重新渲染
        render_frame();
    }
}

// ============================================================
// 主函数 + 键盘轮询线程
// ============================================================
int main() {
    // 启用 ANSI 转义序列支持（Windows 10+）
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    // 隐藏光标
    std::cout << "\033[?25l";
    // 清屏
    std::cout << "\033[2J\033[H";

    std::cout << "========================================" << std::endl;
    std::cout << "  Level 4: Pong Client" << std::endl;
    std::cout << "  P1: W/S   P2: Arrow Up/Down" << std::endl;
    std::cout << "========================================" << std::endl;

    // --- Winsock 初始化 + 连接 ---
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in server_addr{};
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    if (connect(g_sock, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Connect failed!" << std::endl;
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }

    std::cout << "[OK] Connected!" << std::endl;

    // --- 启动接收线程 ---
    std::thread recv(recv_thread);

    // --- 主线程：键盘轮询 + 发送 PADDLE 命令 ---
    // 轮询速率：~60Hz（与服务器 tick 同步）
    constexpr int INPUT_MS = 16;

    float my_paddle_y = COURT_H / 2;     // 本地 paddle 位置
    auto last_input = std::chrono::steady_clock::now();

    while (running) {
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_input).count();
        last_input = now;
        if (dt > 0.05f) dt = 0.05f;  // cap

        if (in_game) {
            bool moved = false;

            if (my_player_id == 1) {
                // 玩家1: W/S
                if (GetAsyncKeyState('W') & 0x8000) {
                    my_paddle_y -= PADDLE_SPEED * dt;
                    moved = true;
                }
                if (GetAsyncKeyState('S') & 0x8000) {
                    my_paddle_y += PADDLE_SPEED * dt;
                    moved = true;
                }
            } else if (my_player_id == 2) {
                // 玩家2: ↑/↓
                if (GetAsyncKeyState(VK_UP) & 0x8000) {
                    my_paddle_y -= PADDLE_SPEED * dt;
                    moved = true;
                }
                if (GetAsyncKeyState(VK_DOWN) & 0x8000) {
                    my_paddle_y += PADDLE_SPEED * dt;
                    moved = true;
                }
            }

            // 限制范围
            if (my_paddle_y < PADDLE_H/2) my_paddle_y = PADDLE_H/2;
            if (my_paddle_y > COURT_H - PADDLE_H/2)
                my_paddle_y = COURT_H - PADDLE_H/2;

            // 发送 paddle 位置到服务器
            if (moved) {
                char cmd[64];
                snprintf(cmd, sizeof(cmd), "P %.1f\n", my_paddle_y);
                send(g_sock, cmd, (int)strlen(cmd), 0);
            }
        }

        // 检查退出键 (Esc)
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            running = false;
            break;
        }

        // 帧率控制：~60Hz 轮询
        std::this_thread::sleep_for(std::chrono::milliseconds(INPUT_MS));
    }

    // --- 清理 ---
    running = false;
    closesocket(g_sock);
    WSACleanup();
    if (recv.joinable()) recv.join();

    // 恢复光标
    std::cout << "\033[?25h\n";
    std::cout << "[OK] Client shutdown." << std::endl;
    return 0;
}
