/**
 * Level 6: Mini MMO 客户端 — 收官之作
 *
 * 新增:
 *   1. 视口 (Viewport): 摄像机跟随玩家，只渲染可见区域
 *   2. 坐标映射: 世界坐标 → 屏幕坐标
 *   3. 小地图: 10×10 空间网格鸟瞰图
 *   4. 不同类型实体渲染: P=玩家(●), M=怪物(按等级显示不同字符)
 *
 * 操作: WASD=移动, Space=攻击, Esc=退出
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
constexpr int   BUFFER_SIZE = 16384;

// 世界参数（与服务器一致）
constexpr float WORLD_W = 3000.0f;
constexpr float WORLD_H = 3000.0f;
constexpr float PLAYER_SPEED = 200.0f;
constexpr int   GRID_COLS = 10;
constexpr int   GRID_ROWS = 10;

// 显示
constexpr int   DISPLAY_W = 80;
constexpr int   DISPLAY_H = 24;
constexpr float VIEWPORT_W = 900.0f;   // 屏幕显示的视野宽度
constexpr float VIEWPORT_H = 270.0f;   // 屏幕显示的视野高度

std::atomic<bool> running(true);
SOCKET g_sock = INVALID_SOCKET;

// 本地状态
float my_x = 0, my_y = 0;
int   my_hp = 10, my_xp = 0, my_level = 1;
char  my_name[32] = "Player";
int   my_id = -1;

// 视野内实体
struct RemoteEntity {
    int id, type;        // type: 0=player, 1=monster
    float x, y;
    int hp, level, mtype;
    char name[32];
};
std::vector<RemoteEntity> remote_entities;

// ============================================================
// 坐标映射
// ============================================================
/** 世界坐标 → 屏幕列 */
int world_to_col(float wx, float cam_x) {
    float ratio = (wx - cam_x) / VIEWPORT_W;
    int col = (int)(ratio * DISPLAY_W);
    if (col < 0) col = 0;
    if (col >= DISPLAY_W) col = DISPLAY_W - 1;
    return col;
}

/** 世界坐标 → 屏幕行 */
int world_to_row(float wy, float cam_y) {
    float ratio = (wy - cam_y) / VIEWPORT_H;
    int row = (int)(ratio * DISPLAY_H);
    if (row < 0) row = 0;
    if (row >= DISPLAY_H) row = DISPLAY_H - 1;
    return row;
}

/** 怪物类型 → 显示字符 */
char monster_char(int mtype) {
    switch (mtype) {
        case 0: return 'r';  // rat
        case 1: return 'w';  // wolf
        case 2: return 'D';  // dragon
        default: return 'm';
    }
}

// ============================================================
// 渲染
// ============================================================
void render_frame() {
    char screen[DISPLAY_H][DISPLAY_W + 2];

    // 清空
    for (int r = 0; r < DISPLAY_H; r++) {
        memset(screen[r], ' ', DISPLAY_W);
        screen[r][DISPLAY_W] = '\n';
        screen[r][DISPLAY_W+1] = '\0';
    }

    // 摄像机位置（以玩家为中心）
    float cam_x = my_x - VIEWPORT_W / 2;
    float cam_y = my_y - VIEWPORT_H / 2;

    // 画实体
    for (auto& e : remote_entities) {
        int col = world_to_col(e.x, cam_x);
        int row = world_to_row(e.y, cam_y);
        if (col >= 0 && col < DISPLAY_W && row >= 0 && row < DISPLAY_H) {
            if (e.type == 0) {
                screen[row][col] = '@';  // 其他玩家
            } else {
                screen[row][col] = monster_char(e.mtype);
            }
        }
    }

    // 画自己（最后画，确保在最上面）
    int my_col = world_to_col(my_x, cam_x);
    int my_row = world_to_row(my_y, cam_y);
    if (my_col >= 0 && my_col < DISPLAY_W && my_row >= 0 && my_row < DISPLAY_H) {
        screen[my_row][my_col] = '#';  // 自己
    }

    // --- 输出 ---
    std::cout << "\033[H";

    // 状态栏
    std::cout << "=== MINI MMO ===";
    std::cout << "  " << my_name;
    std::cout << "  Lv." << my_level;
    std::cout << "  HP:" << my_hp;
    std::cout << "  XP:" << my_xp << "/" << (my_level * 100);
    std::cout << "  Pos:(" << (int)my_x << "," << (int)my_y << ")";
    std::cout << "  Entities:" << remote_entities.size();
    std::cout << std::endl;

    // 上边框
    std::cout << '+';
    for (int c = 0; c < DISPLAY_W; c++) std::cout << '-';
    std::cout << '+' << std::endl;

    // 游戏画面
    for (int r = 0; r < DISPLAY_H; r++) {
        std::cout << '|' << screen[r] << '|';
    }

    // 下边框
    std::cout << '+';
    for (int c = 0; c < DISPLAY_W; c++) std::cout << '-';
    std::cout << '+' << std::endl;

    // 小地图
    std::cout << "Map: ";
    int player_cell_c = (int)(my_x / (WORLD_W / GRID_COLS));
    int player_cell_r = (int)(my_y / (WORLD_H / GRID_ROWS));
    if (player_cell_c >= GRID_COLS) player_cell_c = GRID_COLS - 1;
    if (player_cell_r >= GRID_ROWS) player_cell_r = GRID_ROWS - 1;

    for (int r = 0; r < GRID_ROWS; r++) {
        for (int c = 0; c < GRID_COLS; c++) {
            // 统计这个格子里有多少实体
            int count = 0;
            for (auto& e : remote_entities) {
                int ec = (int)(e.x / (WORLD_W / GRID_COLS));
                int er = (int)(e.y / (WORLD_H / GRID_ROWS));
                if (ec >= GRID_COLS) ec = GRID_COLS - 1;
                if (er >= GRID_ROWS) er = GRID_ROWS - 1;
                if (ec == c && er == r) count++;
            }
            if (r == player_cell_r && c == player_cell_c) {
                std::cout << '#';
            } else if (count > 0) {
                std::cout << (count < 10 ? (char)('0' + count) : '+');
            } else {
                std::cout << '.';
            }
        }
        std::cout << std::endl << "     ";
    }

    std::cout << "\nWASD:Move  Space:Attack  Esc:Quit" << std::endl;
    std::cout.flush();
}

// ============================================================
// 接收线程
// ============================================================
void recv_thread() {
    char buf[BUFFER_SIZE];
    int buf_pos = 0;

    while (running) {
        int bytes = recv(g_sock, buf + buf_pos, BUFFER_SIZE - 1 - buf_pos, 0);
        if (bytes <= 0) { running = false; break; }
        buf_pos += bytes;
        buf[buf_pos] = '\0';

        char* ptr = buf;
        while (*ptr) {
            char* end = strchr(ptr, '\n');
            if (!end) break;
            *end = '\0';
            char* line = ptr;
            ptr = end + 1;

            if (line[0] == 'S') {
                remote_entities.clear();
            }
            else if (line[0] == 'E') {
                render_frame();
            }
            else if (line[0] == 'P' && line[1] == ' ') {
                RemoteEntity e;
                e.type = 0;
                // P <id> <x> <y> <name> <hp> <level>
                char* tok = strtok(line+2, " ");
                if (tok) e.id = atoi(tok);
                tok = strtok(nullptr, " "); if (tok) e.x = (float)atof(tok);
                tok = strtok(nullptr, " "); if (tok) e.y = (float)atof(tok);
                tok = strtok(nullptr, " "); if (tok) strncpy(e.name, tok, 31);
                tok = strtok(nullptr, " "); if (tok) e.hp = atoi(tok);
                tok = strtok(nullptr, " "); if (tok) e.level = atoi(tok);
                e.mtype = 0;
                remote_entities.push_back(e);
            }
            else if (line[0] == 'M' && line[1] == ' ') {
                RemoteEntity e;
                e.type = 1;
                // M <id> <x> <y> <hp> <mtype>
                char* tok = strtok(line+2, " ");
                if (tok) e.id = atoi(tok);
                tok = strtok(nullptr, " "); if (tok) e.x = (float)atof(tok);
                tok = strtok(nullptr, " "); if (tok) e.y = (float)atof(tok);
                tok = strtok(nullptr, " "); if (tok) e.hp = atoi(tok);
                tok = strtok(nullptr, " "); if (tok) e.mtype = atoi(tok);
                e.level = 0;
                e.name[0] = '\0';
                remote_entities.push_back(e);
            }
            else if (strncmp(line, "STAT ", 5) == 0) {
                sscanf(line+5, "%d %d %d", &my_hp, &my_xp, &my_level);
            }
            else if (strncmp(line, "HIT ", 4) == 0) {
                int dmg, mob_hp;
                sscanf(line+4, "%d %d", &dmg, &mob_hp);
                // 简单显示（会被下一个渲染帧覆盖，但在日志中可见）
            }
            else if (strncmp(line, "KILL ", 5) == 0) {
                int mob_id, xp;
                sscanf(line+5, "%d %d", &mob_id, &xp);
            }
            else if (strncmp(line, "START ", 6) == 0) {
                // START <id> <name> <x> <y> <hp> <xp> <level>
                char* tok = strtok(line+6, " ");
                if (tok) my_id = atoi(tok);
                tok = strtok(nullptr, " "); if (tok) strncpy(my_name, tok, 31);
                tok = strtok(nullptr, " "); if (tok) my_x = (float)atof(tok);
                tok = strtok(nullptr, " "); if (tok) my_y = (float)atof(tok);
                tok = strtok(nullptr, " "); if (tok) my_hp = atoi(tok);
                tok = strtok(nullptr, " "); if (tok) my_xp = atoi(tok);
                tok = strtok(nullptr, " "); if (tok) my_level = atoi(tok);

                std::cout << "\033[2J\033[H";
                std::cout << "Welcome, " << my_name << "! Lv." << my_level << std::endl;
            }
        }

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
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    std::cout << "\033[2J\033[H\033[?25l";

    std::cout << "========================================" << std::endl;
    std::cout << "  Level 6: Mini MMO Client" << std::endl;
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

    // --- 主循环 ---
    constexpr int INPUT_MS = 16;

    while (running) {
        auto start = std::chrono::steady_clock::now();

        // 读取 WASD
        float vx = 0, vy = 0;
        if (GetAsyncKeyState('W') & 0x8000) vy -= 1;
        if (GetAsyncKeyState('S') & 0x8000) vy += 1;
        if (GetAsyncKeyState('A') & 0x8000) vx -= 1;
        if (GetAsyncKeyState('D') & 0x8000) vx += 1;

        if (vx != 0 || vy != 0) {
            float len = std::sqrt(vx*vx + vy*vy);
            vx /= len; vy /= len;

            // 客户端预测
            auto now = std::chrono::steady_clock::now();
            static auto last_pred = now;
            float dt = std::chrono::duration<float>(now - last_pred).count();
            last_pred = now;
            if (dt > 0.05f) dt = 0.016f;

            my_x += vx * PLAYER_SPEED * dt;
            my_y += vy * PLAYER_SPEED * dt;
            if (my_x < 0) my_x = 0;
            if (my_y < 0) my_y = 0;
            if (my_x > WORLD_W) my_x = WORLD_W;
            if (my_y > WORLD_H) my_y = WORLD_H;
        }

        // 发送移动命令
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "M %.2f %.2f\n", vx, vy);
        send(g_sock, cmd, (int)strlen(cmd), 0);

        // 攻击
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
            send(g_sock, "A\n", 2, 0);
        }

        // 退出
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            running = false;
            break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed < INPUT_MS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(INPUT_MS - elapsed));
        }
    }

    running = false;
    closesocket(g_sock);
    WSACleanup();
    if (recv.joinable()) recv.join();

    std::cout << "\033[?25h\n[OK] Client shutdown." << std::endl;
    return 0;
}
