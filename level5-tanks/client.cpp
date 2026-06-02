/**
 * Level 5: 坦克大战客户端
 *
 * 新增:
 *   1. 多实体渲染: 遍历所有 Tank 和 Bullet，按坐标映射到字符网格
 *   2. 坦克朝向: 根据 angle 选择方向字符 (→ ↓ ← ↑)
 *   3. 快照解析: S/E 分隔的批量实体更新
 *   4. 击杀通知: KILL 消息实时显示
 *
 * 操作: WASD 移动, Space 射击, Esc 退出
 */

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <winsock2.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

constexpr int   SERVER_PORT = 8080;
constexpr char  SERVER_IP[] = "127.0.0.1";
constexpr int   BUFFER_SIZE = 8192;

// 显示
constexpr int   DISPLAY_W = 82;
constexpr int   DISPLAY_H = 28;

// 游戏常量（与服务器一致）
constexpr float ARENA_W = 800.0f;
constexpr float ARENA_H = 600.0f;
constexpr float TANK_SPEED = 220.0f;

std::atomic<bool> running(true);
SOCKET g_sock = INVALID_SOCKET;
int   my_player_id = -1;
int   total_players = 0;
bool  in_game = false;

// 本地实体缓存
struct LocalEntity {
    int id, type;       // 0=tank, 1=bullet
    float x, y, angle;
    int hp, kills, owner;
};
std::vector<LocalEntity> local_entities;

// 击杀日志
char kill_log[4][64] = {};
int  kill_log_count = 0;

// ============================================================
// 坐标映射
// ============================================================
int x_to_col(float x) {
    float ratio = x / ARENA_W;
    int col = 1 + (int)(ratio * (DISPLAY_W - 2));
    if (col < 1) col = 1;
    if (col >= DISPLAY_W - 1) col = DISPLAY_W - 2;
    return col;
}

int y_to_row(float y) {
    float ratio = y / ARENA_H;
    int row = 1 + (int)(ratio * (DISPLAY_H - 2));
    if (row < 1) row = 1;
    if (row >= DISPLAY_H - 1) row = DISPLAY_H - 2;
    return row;
}

/** 根据角度返回方向字符 */
char angle_to_char(float angle) {
    // 标准化到 [0, 2π)
    const float PI = 3.14159265f;
    while (angle < 0) angle += 2*PI;
    while (angle >= 2*PI) angle -= 2*PI;

    // 8 方向: 0=→, π/4=↘, π/2=↓, 3π/4=↙, π=←, 5π/4=↖, 3π/2=↑, 7π/4=↗
    int dir = (int)((angle + PI/8) / (PI/4)) % 8;
    switch (dir) {
        case 0: return '>';
        case 1: return '>';
        case 2: return 'v';
        case 3: return '<';
        case 4: return '<';
        case 5: return '<';
        case 6: return '^';
        case 7: return '>';
        default: return '?';
    }
}

// ============================================================
// 渲染
// ============================================================
void render_frame() {
    // 字符缓冲区
    static char screen[DISPLAY_H][DISPLAY_W + 2];

    // 清空
    for (int r = 0; r < DISPLAY_H; r++) {
        memset(screen[r], ' ', DISPLAY_W);
        screen[r][DISPLAY_W] = '\n';
        screen[r][DISPLAY_W+1] = '\0';
    }

    // 边框
    for (int c = 0; c < DISPLAY_W; c++) {
        screen[0][c] = '=';
        screen[DISPLAY_H-1][c] = '=';
    }
    for (int r = 0; r < DISPLAY_H; r++) {
        screen[r][0] = '|';
        screen[r][DISPLAY_W-1] = '|';
    }

    // 先画子弹（在坦克下面）
    for (auto& e : local_entities) {
        if (e.type == 1) {  // bullet
            int col = x_to_col(e.x);
            int row = y_to_row(e.y);
            if (col > 0 && col < DISPLAY_W-1 && row > 0 && row < DISPLAY_H-1) {
                screen[row][col] = '*';
            }
        }
    }

    // 再画坦克（在子弹上面）
    for (auto& e : local_entities) {
        if (e.type == 0) {  // tank
            int col = x_to_col(e.x);
            int row = y_to_row(e.y);
            if (col > 0 && col < DISPLAY_W-1 && row > 0 && row < DISPLAY_H-1) {
                screen[row][col] = angle_to_char(e.angle);
            }
        }
    }

    // --- 输出 ---
    std::cout << "\033[H";  // 光标回左上角

    // 标题栏
    std::cout << "=== TANK BATTLE ===";
    if (!in_game) std::cout << "  Waiting...";
    std::cout << std::endl;

    // 游戏区域
    for (int r = 0; r < DISPLAY_H; r++) {
        std::cout << screen[r];
    }

    // 状态栏
    std::cout << std::endl;
    for (auto& e : local_entities) {
        if (e.type == 0 && e.owner == my_player_id) {
            std::cout << "YOU | HP: ";
            for (int i = 0; i < e.hp; i++) std::cout << '#';
            for (int i = e.hp; i < 3; i++) std::cout << '_';
            std::cout << " | Kills: " << e.kills;
            break;
        }
    }

    // 击杀日志
    for (int i = 0; i < kill_log_count; i++) {
        std::cout << "  |  " << kill_log[i];
    }

    std::cout << std::endl;
    std::cout << "WASD:Move  Space:Fire  Esc:Quit" << std::endl;

    std::cout.flush();
}

// ============================================================
// 接收线程
// ============================================================
void recv_thread() {
    char buf[BUFFER_SIZE];
    int  buf_pos = 0;
    bool in_snapshot = false;

    while (running) {
        int bytes = recv(g_sock, buf + buf_pos, BUFFER_SIZE - 1 - buf_pos, 0);
        if (bytes <= 0) {
            running = false;
            break;
        }
        buf_pos += bytes;
        buf[buf_pos] = '\0';

        // 按行处理（手动解析，不依赖 strtok_r）
        char* ptr = buf;
        while (*ptr) {
            // 找行尾
            char* end = strchr(ptr, '\n');
            if (!end) break;  // 没有完整的行，等待更多数据
            *end = '\0';      // 临时截断

            char* line = ptr;
            ptr = end + 1;    // 移动到下一行

            if (line[0] == 'S') {
                local_entities.clear();
                in_snapshot = true;
            }
            else if (line[0] == 'E') {
                in_snapshot = false;
                render_frame();
            }
            else if (line[0] == 'T' && line[1] == ' ') {
                LocalEntity e;
                e.type = 0;
                e.owner = -1;

                // 格式: T <id> <x> <y> <angle> <hp> <kills>
                char* tok = strtok(line+2, " ");
                if (tok) e.id = atoi(tok);
                tok = strtok(nullptr, " "); if (tok) e.x = (float)atof(tok);
                tok = strtok(nullptr, " "); if (tok) e.y = (float)atof(tok);
                tok = strtok(nullptr, " "); if (tok) e.angle = (float)atof(tok);
                tok = strtok(nullptr, " "); if (tok) e.hp = atoi(tok);
                tok = strtok(nullptr, " "); if (tok) e.kills = atoi(tok);
                // Try to read owner if available
                tok = strtok(nullptr, " ");
                if (tok) e.owner = atoi(tok);

                local_entities.push_back(e);
            }
            else if (line[0] == 'B' && line[1] == ' ') {
                LocalEntity e;
                e.type = 1;
                e.id = 0; e.hp = 0; e.kills = 0; e.angle = 0; e.owner = -1;
                sscanf(line+2, "%d %f %f", &e.id, &e.x, &e.y);
                local_entities.push_back(e);
            }
            else if (strncmp(line, "KILL ", 5) == 0) {
                int killer, victim;
                sscanf(line+5, "%d %d", &killer, &victim);
                if (kill_log_count < 4) {
                    if (victim == my_player_id)
                        snprintf(kill_log[kill_log_count], 64,
                                 "You were killed by P%d!", killer);
                    else if (killer == my_player_id)
                        snprintf(kill_log[kill_log_count], 64,
                                 "You killed P%d!", victim);
                    else
                        snprintf(kill_log[kill_log_count], 64,
                                 "P%d killed P%d", killer, victim);
                    kill_log_count++;
                }
                // 延迟清除日志
                std::thread([](){
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                    if (kill_log_count > 0) kill_log_count--;
                }).detach();
            }
            else if (strncmp(line, "START ", 6) == 0) {
                sscanf(line+6, "%d %d", &my_player_id, &total_players);
                in_game = true;
                kill_log_count = 0;
                local_entities.clear();
                std::cout << "\033[2J\033[H";
                std::cout << "Game starting! You are Player "
                          << my_player_id << std::endl;
            }
            else if (strncmp(line, "WAIT", 4) == 0) {
                in_game = false;
            }
        }

        // 把未处理完的数据移到缓冲区开头
        int remaining = buf_pos - (int)(ptr - buf);
        if (remaining > 0 && remaining < BUFFER_SIZE) {
            memmove(buf, ptr, remaining);
            buf_pos = remaining;
        } else {
            buf_pos = 0;
        }
    }
}

// ============================================================
// 主函数
// ============================================================
int main() {
    // 启用 ANSI
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    std::cout << "\033[2J\033[H\033[?25l";  // 清屏 + 隐藏光标

    std::cout << "========================================" << std::endl;
    std::cout << "  Level 5: Tank Battle Client" << std::endl;
    std::cout << "========================================" << std::endl;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    if (connect(g_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "Connect failed!" << std::endl;
        return 1;
    }
    std::cout << "[OK] Connected!" << std::endl;

    std::thread recv(recv_thread);

    // --- 主循环: 键盘输入 ---
    constexpr int INPUT_MS = 16;

    while (running) {
        auto start = std::chrono::steady_clock::now();

        if (in_game) {
            // 读取 WASD
            float vx = 0, vy = 0;
            if (GetAsyncKeyState('W') & 0x8000) vy -= 1;
            if (GetAsyncKeyState('S') & 0x8000) vy += 1;
            if (GetAsyncKeyState('A') & 0x8000) vx -= 1;
            if (GetAsyncKeyState('D') & 0x8000) vx += 1;

            // 归一化
            if (vx != 0 || vy != 0) {
                float len = std::sqrt(vx*vx + vy*vy);
                vx /= len; vy /= len;
            }

            // 发送移动命令
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "M %.2f %.2f\n", vx, vy);
            send(g_sock, cmd, (int)strlen(cmd), 0);

            // 射击
            if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
                send(g_sock, "F\n", 2, 0);
            }
        }

        // 退出
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            running = false;
            break;
        }

        // 帧率控制
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed < INPUT_MS) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(INPUT_MS - elapsed));
        }
    }

    running = false;
    closesocket(g_sock);
    WSACleanup();
    if (recv.joinable()) recv.join();

    std::cout << "\033[?25h\n[OK] Client shutdown." << std::endl;
    return 0;
}
