/**
 * Level 4: 实时乒乓游戏服务器
 *
 * 从回合制跨越到实时游戏的关键一步！
 *
 * 新增学习目标：
 *   1. 游戏主循环 (Game Loop): select() 设置短超时 → 即使没有 I/O 也定期 tick
 *   2. 固定时间步 (Fixed Timestep): 物理计算使用真实 dt，不受网络波动影响
 *   3. 权威服务器 (Authoritative Server): 客户端只管输入和显示，服务端掌管一切
 *   4. 实时状态同步: 每 tick 把完整游戏状态发给双方
 *   5. 简单碰撞检测: AABB (轴对齐包围盒) 球-拍碰撞
 *
 * 架构对比：
 *   L3 井字棋: select(无限) → 等待事件 → 处理 → 等待 → ...
 *   L4 乒乓:   select(16ms) → 处理I/O → 物理tick → 广播状态 → select(16ms) → ...
 *
 * 协议（极简，适合高频通讯）:
 *   客户端 → 服务器:  P <y>            (期望的拍子 Y 坐标)
 *   服务器 → 客户端:  S <bx> <by> <p1y> <p2y> <s1> <s2>  (状态快照)
 *   服务器 → 客户端:  W <winner>       (游戏结束)
 *   服务器 → 客户端:  WAIT             (等待对手)
 */

#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <cmath>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

// ============================================================
// 游戏常量
// ============================================================
constexpr float COURT_W      = 800.0f;   // 场地宽度
constexpr float COURT_H      = 600.0f;   // 场地高度
constexpr float PADDLE_W     = 12.0f;    // 球拍宽度
constexpr float PADDLE_H     = 80.0f;    // 球拍高度
constexpr float PADDLE_SPEED = 450.0f;   // 球拍移动速度
constexpr float BALL_SIZE    = 12.0f;    // 球直径
constexpr float BALL_SPEED   = 350.0f;   // 球初始速度
constexpr float BALL_ACCEL   = 1.02f;    // 每次碰撞后速度增加（让对局不会无限拖下去）
constexpr int   WIN_SCORE    = 11;       // 先得 11 分获胜

// 网络常量
constexpr int   SERVER_PORT  = 8080;
constexpr int   BACKLOG      = 5;
constexpr int   BUFFER_SIZE  = 1024;
constexpr int   MAX_CLIENTS  = 60;

// 游戏循环: select 超时 16ms ≈ 60 tick/sec
constexpr int   TICK_MS      = 16;
constexpr float MAX_DT       = 0.05f;    // 防止螺旋式死亡，dt 上限 50ms

// ============================================================
// 对局结构
// ============================================================
struct PongGame {
    // --- 球 ---
    float bx = COURT_W / 2;   // 球 X
    float by = COURT_H / 2;   // 球 Y
    float bvx = BALL_SPEED;   // 球 X 速度
    float bvy = BALL_SPEED * 0.6f;  // 球 Y 速度（斜向发射）

    // --- 球拍 Y 坐标（中心）---
    float p1y = COURT_H / 2;  // 玩家1 球拍
    float p2y = COURT_H / 2;  // 玩家2 球拍

    // --- 分数 ---
    int score1 = 0;
    int score2 = 0;

    // --- 状态 ---
    bool game_over = false;

    // 当前速度倍率（随碰撞增加）
    float speed_mult = 1.0f;

    /**
     * 每帧物理更新
     * @param dt 距离上一帧的秒数（如 0.016 = 16ms）
     */
    void update(float dt) {
        if (game_over) return;

        // --- 移动球 ---
        bx += bvx * speed_mult * dt;
        by += bvy * speed_mult * dt;

        // --- 上下边界反弹 ---
        if (by - BALL_SIZE/2 <= 0) {
            by = BALL_SIZE / 2;
            bvy = std::abs(bvy);  // 强制向下
        }
        if (by + BALL_SIZE/2 >= COURT_H) {
            by = COURT_H - BALL_SIZE/2;
            bvy = -std::abs(bvy); // 强制向上
        }

        // --- 球拍1 碰撞检测 ---
        float px1 = 30.0f;  // 左球拍 X 位置
        if (check_paddle_collision(px1, p1y)) {
            bx = px1 + PADDLE_W + BALL_SIZE/2;  // 把球推到拍子右侧
            bvx = std::abs(bvx);                  // 强制向右
            speed_mult *= BALL_ACCEL;             // 加速
        }

        // --- 球拍2 碰撞检测 ---
        float px2 = COURT_W - 30.0f - PADDLE_W;
        if (check_paddle_collision(px2, p2y)) {
            bx = px2 - BALL_SIZE/2;               // 把球推到拍子左侧
            bvx = -std::abs(bvx);                  // 强制向左
            speed_mult *= BALL_ACCEL;
        }

        // --- 得分检测 ---
        if (bx < 0) {
            // 球出左边界 → P2 得分
            score2++;
            if (score2 >= WIN_SCORE) {
                game_over = true;
            } else {
                reset_ball();
            }
        }
        if (bx > COURT_W) {
            // 球出右边界 → P1 得分
            score1++;
            if (score1 >= WIN_SCORE) {
                game_over = true;
            } else {
                reset_ball();
            }
        }
    }

    /**
     * 检测球是否碰到指定球拍
     * 使用 AABB (Axis-Aligned Bounding Box) 碰撞检测
     */
    bool check_paddle_collision(float px, float py) {
        // 球和球拍的包围盒是否重叠？
        float b_left   = bx - BALL_SIZE/2;
        float b_right  = bx + BALL_SIZE/2;
        float b_top    = by - BALL_SIZE/2;
        float b_bottom = by + BALL_SIZE/2;

        float p_left   = px;
        float p_right  = px + PADDLE_W;
        float p_top    = py - PADDLE_H/2;
        float p_bottom = py + PADDLE_H/2;

        // AABB 相交检测
        return (b_left < p_right && b_right > p_left &&
                b_top < p_bottom && b_bottom > p_top);
    }

    /**
     * 重置球到场地中心，随机方向发射
     */
    void reset_ball() {
        bx = COURT_W / 2;
        by = COURT_H / 2;
        speed_mult = 1.0f;

        // 随机发射角度 (-30° 到 +30°)
        float angle = ((rand() % 61) - 30) * 3.14159265f / 180.0f;
        float dir = (rand() % 2 == 0) ? 1.0f : -1.0f;  // 随机向左或向右

        bvx = BALL_SPEED * std::cos(angle) * dir;
        bvy = BALL_SPEED * std::sin(angle);
    }
};

// ============================================================
// 玩家
// ============================================================
enum class PlayerState { WAITING, IN_GAME };

struct Player {
    SOCKET      socket = INVALID_SOCKET;
    sockaddr_in addr{};
    PlayerState state = PlayerState::WAITING;
    PongGame*   game = nullptr;
    int         player_id = 0;  // 1 或 2
    float       target_y = 0;   // 客户端要求的球拍位置
};

// ============================================================
// 全局数据
// ============================================================
std::vector<Player*> players;
std::queue<Player*>  wait_queue;
PongGame*            active_game = nullptr;  // 简化：一次只支持一局

void print_error(const char* where) {
    std::cerr << "[ERROR] " << where
              << " failed, code=" << WSAGetLastError() << std::endl;
}

void send_str(SOCKET fd, const char* msg) {
    int len = (int)strlen(msg);
    send(fd, msg, len, 0);
}

// ============================================================
// 主函数
// ============================================================
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Level 4: Pong Server (Port " << SERVER_PORT << ")" << std::endl;
    std::cout << "  Game loop: " << TICK_MS << "ms ticks" << std::endl;
    std::cout << "========================================" << std::endl;

    srand((unsigned int)time(nullptr));

    // --- Winsock 初始化 ---
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        print_error("WSAStartup"); return 1;
    }

    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        print_error("socket"); WSACleanup(); return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(SERVER_PORT);

    if (bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        print_error("bind"); closesocket(server_socket); WSACleanup(); return 1;
    }
    if (listen(server_socket, BACKLOG) == SOCKET_ERROR) {
        print_error("listen"); closesocket(server_socket); WSACleanup(); return 1;
    }

    std::cout << "[OK] Pong server listening on port " << SERVER_PORT << std::endl;
    std::cout << "----------------------------------------" << std::endl;

    // ============================================================
    // 游戏主循环 — 这是 Level 4 的核心！
    //
    // select() 的超时参数是关键：
    //   L2/L3: timeout=NULL → 无限等待 I/O
    //   L4:    timeout=16ms → 即使没有 I/O，16ms 后也返回,
    //          这样我们就能定期执行 physics tick！
    //
    // 流程：
    //   select(16ms) → 处理 I/O → dt = now - last → 物理更新(dt) → 广播 → 循环
    // ============================================================
    auto last_tick = std::chrono::steady_clock::now();
    int  tick_count = 0;

    while (true) {
        // --- 构建 fd_set ---
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket, &read_fds);
        SOCKET max_fd = server_socket;

        for (auto* p : players) {
            if (p->socket != INVALID_SOCKET) {
                FD_SET(p->socket, &read_fds);
                if (p->socket > max_fd) max_fd = p->socket;
            }
        }

        // --- 关键！16ms 超时让 select() 变成游戏循环的计时器 ---
        timeval timeout;
        timeout.tv_sec  = 0;
        timeout.tv_usec = TICK_MS * 1000;  // 16ms = 16000µs

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

        if (activity == SOCKET_ERROR) {
            print_error("select"); break;
        }

        // ----------------------------------------------------
        // 阶段 1: 处理新连接
        // ----------------------------------------------------
        if (FD_ISSET(server_socket, &read_fds)) {
            sockaddr_in client_addr{};
            int addr_len = sizeof(client_addr);
            SOCKET fd = accept(server_socket, (sockaddr*)&client_addr, &addr_len);

            if (fd != INVALID_SOCKET && (int)players.size() < MAX_CLIENTS) {
                auto* p = new Player();
                p->socket = fd;
                p->addr   = client_addr;
                players.push_back(p);
                wait_queue.push(p);

                std::cout << "[JOIN] Player from "
                          << inet_ntoa(client_addr.sin_addr)
                          << " (queue: " << wait_queue.size() << ")" << std::endl;

                send_str(fd, "WAIT\n");

                // 检查能否配对
                if (wait_queue.size() >= 2 && active_game == nullptr) {
                    Player* p1 = wait_queue.front(); wait_queue.pop();
                    Player* p2 = wait_queue.front(); wait_queue.pop();

                    active_game = new PongGame();

                    p1->state     = PlayerState::IN_GAME;
                    p1->game      = active_game;
                    p1->player_id = 1;
                    p1->target_y  = COURT_H / 2;

                    p2->state     = PlayerState::IN_GAME;
                    p2->game      = active_game;
                    p2->player_id = 2;
                    p2->target_y  = COURT_H / 2;

                    send_str(p1->socket, "START 1\n");
                    send_str(p2->socket, "START 2\n");

                    std::cout << "[MATCH] Game started!" << std::endl;
                }
            }
        }

        // ----------------------------------------------------
        // 阶段 2: 处理客户端输入 (PADDLE 命令)
        // ----------------------------------------------------
        for (auto* p : players) {
            if (p->socket == INVALID_SOCKET) continue;
            if (!FD_ISSET(p->socket, &read_fds)) continue;

            char buf[BUFFER_SIZE];
            int bytes = recv(p->socket, buf, BUFFER_SIZE - 1, 0);

            if (bytes <= 0) {
                // 断开连接
                std::cout << "[LEAVE] Player " << p->player_id
                          << " disconnected." << std::endl;
                if (active_game && p->state == PlayerState::IN_GAME) {
                    active_game->game_over = true;
                }
                closesocket(p->socket);
                p->socket = INVALID_SOCKET;
                continue;
            }

            buf[bytes] = '\0';

            // 解析 PADDLE 命令: "P <y>\n"
            // 可能收到多条（TCP 粘包），按行分割处理
            char* line = strtok(buf, "\n");
            while (line) {
                if (line[0] == 'P' && line[1] == ' ') {
                    float target = (float)atof(line + 2);
                    // 限制在场地范围内
                    if (target < PADDLE_H/2) target = PADDLE_H/2;
                    if (target > COURT_H - PADDLE_H/2) target = COURT_H - PADDLE_H/2;
                    p->target_y = target;
                }
                line = strtok(nullptr, "\n");
            }
        }

        // ----------------------------------------------------
        // 阶段 3: 物理更新 — 计算真实的 delta time
        // ----------------------------------------------------
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_tick).count();
        last_tick = now;

        // 防止螺旋式死亡：如果因为某种原因 dt 特别大（如调试断点），
        // 把它限制在 MAX_DT，否则物体会穿过墙壁
        if (dt > MAX_DT) dt = MAX_DT;

        if (active_game && !active_game->game_over) {
            // 3a. 根据客户端输入移动球拍
            for (auto* p : players) {
                if (p->state != PlayerState::IN_GAME) continue;

                float current_y = (p->player_id == 1)
                                  ? active_game->p1y : active_game->p2y;
                float target    = p->target_y;

                // 平滑移动（不能超过 PADDLE_SPEED）
                float max_move = PADDLE_SPEED * dt;
                if (target > current_y) {
                    current_y = std::min(current_y + max_move, target);
                } else {
                    current_y = std::max(current_y - max_move, target);
                }

                if (p->player_id == 1) active_game->p1y = current_y;
                else                   active_game->p2y = current_y;
            }

            // 3b. 物理 tick
            active_game->update(dt);

            // 3c. 每 6 tick (~100ms) 打印服务器状态
            tick_count++;
            if (tick_count % 6 == 0) {
                std::cout << "\r[TICK] Ball(" << (int)active_game->bx
                          << "," << (int)active_game->by
                          << ") Score " << active_game->score1
                          << "-" << active_game->score2
                          << "    " << std::flush;
            }
        }

        // ----------------------------------------------------
        // 阶段 4: 广播游戏状态（每 tick 都发）
        // ----------------------------------------------------
        if (active_game) {
            char state_msg[256];
            if (active_game->game_over) {
                // 游戏结束 — 通知双方
                int winner = (active_game->score1 >= WIN_SCORE) ? 1 : 2;
                for (auto* p : players) {
                    if (p->state == PlayerState::IN_GAME && p->socket != INVALID_SOCKET) {
                        snprintf(state_msg, sizeof(state_msg), "W %d\n", winner);
                        send_str(p->socket, state_msg);
                    }
                }
                // 清理
                delete active_game;
                active_game = nullptr;
                std::cout << "\n[GAME] Over! Winner: Player " << winner << std::endl;

                // 把还在的玩家放回等待队列
                for (auto* p : players) {
                    if (p->socket != INVALID_SOCKET) {
                        p->state = PlayerState::WAITING;
                        p->game  = nullptr;
                        wait_queue.push(p);
                        send_str(p->socket, "WAIT\n");
                    }
                }
                // 尝试再次配对
                if (wait_queue.size() >= 2 && active_game == nullptr) {
                    Player* p1 = wait_queue.front(); wait_queue.pop();
                    Player* p2 = wait_queue.front(); wait_queue.pop();

                    active_game = new PongGame();
                    p1->state = PlayerState::IN_GAME;
                    p1->game  = active_game;
                    p1->player_id = 1;
                    p1->target_y  = COURT_H / 2;
                    p2->state = PlayerState::IN_GAME;
                    p2->game  = active_game;
                    p2->player_id = 2;
                    p2->target_y  = COURT_H / 2;
                    send_str(p1->socket, "START 1\n");
                    send_str(p2->socket, "START 2\n");
                    std::cout << "[MATCH] New game started!" << std::endl;
                }
            } else {
                // 发送状态快照: "S <bx> <by> <p1y> <p2y> <s1> <s2>\n"
                snprintf(state_msg, sizeof(state_msg),
                         "S %.1f %.1f %.1f %.1f %d %d\n",
                         active_game->bx, active_game->by,
                         active_game->p1y, active_game->p2y,
                         active_game->score1, active_game->score2);

                for (auto* p : players) {
                    if (p->state == PlayerState::IN_GAME && p->socket != INVALID_SOCKET) {
                        send_str(p->socket, state_msg);
                    }
                }
            }
        }

        // --- 清理已断开的玩家 ---
        players.erase(
            std::remove_if(players.begin(), players.end(),
                           [](Player* p) {
                               if (p->socket == INVALID_SOCKET) {
                                   delete p; return true;
                               }
                               return false;
                           }),
            players.end()
        );

        // 清理无效的队列条目
        std::queue<Player*> clean_q;
        while (!wait_queue.empty()) {
            Player* p = wait_queue.front(); wait_queue.pop();
            if (p->socket != INVALID_SOCKET && p->state == PlayerState::WAITING) {
                clean_q.push(p);
            }
        }
        wait_queue = clean_q;
    }

    // --- 清理 ---
    if (active_game) delete active_game;
    for (auto* p : players) { closesocket(p->socket); delete p; }
    closesocket(server_socket);
    WSACleanup();
    return 0;
}
