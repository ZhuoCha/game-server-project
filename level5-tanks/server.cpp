/**
 * Level 5: 简易坦克大战服务器
 *
 * 在 Level 4 Pong 的实时游戏循环基础上，加入多实体管理。
 *
 * 新增学习目标：
 *   1. 实体系统 (Entity System): Tank + Bullet 两种实体，统一管理
 *   2. 实体生命周期: Spawn → Update → Collide → Destroy → Respawn
 *   3. 射击机制: 客户端按空格 → 服务器创建 Bullet 实体
 *   4. 碰撞矩阵: Bullet-Tank, Bullet-Wall, Tank-Tank
 *   5. 状态快照: 每 50ms 把全部实体状态打包发给客户端
 *   6. 计分板: 每人 tracking kills/deaths
 *
 * 协议:
 *   客户端 → 服务器:
 *     M <vx> <vy>    移动方向（-1..1 归一化向量）
 *     F              发射子弹（在当前朝向）
 *
 *   服务器 → 客户端:
 *     START <id> <total>          游戏开始，你是第几个玩家
 *     S                           快照开始（清空旧实体）
 *     T <id> <x> <y> <a> <hp> <k> 坦克实体
 *     B <id> <x> <y>              子弹实体
 *     E                           快照结束（渲染）
 *     KILL <killer> <victim>      击杀通知
 *     WAIT                        等待其他玩家
 */

#include <iostream>
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
constexpr float ARENA_W      = 800.0f;
constexpr float ARENA_H      = 600.0f;
constexpr float TANK_SIZE    = 30.0f;     // 坦克碰撞半径
constexpr float BULLET_SIZE  = 6.0f;      // 子弹碰撞半径
constexpr float TANK_SPEED   = 220.0f;    // 坦克移动速度
constexpr float BULLET_SPEED = 550.0f;    // 子弹飞行速度
constexpr float RESPAWN_TIME = 3.0f;      // 死亡后重生等待
constexpr int   TANK_HP      = 3;         // 坦克生命值
constexpr int   MAX_PLAYERS  = 4;         // 一局最多4人
constexpr float FIRE_COOLDOWN = 0.5f;     // 射击冷却（秒）

constexpr int   SERVER_PORT  = 8080;
constexpr int   BACKLOG      = 5;
constexpr int   BUFFER_SIZE  = 4096;
constexpr int   TICK_MS      = 16;        // ~60Hz
constexpr float MAX_DT       = 0.05f;
constexpr int   SNAPSHOT_INTERVAL = 3;    // 每3 tick (~50ms) 发一次快照

// ============================================================
// 实体
// ============================================================
enum EntityType { TANK = 0, BULLET = 1 };

struct Entity {
    int  id = 0;
    EntityType type = TANK;
    float x = 0, y = 0;          // 位置
    float vx = 0, vy = 0;        // 速度 (pixels/sec)
    float angle = 0;             // 朝向 (弧度, 0=右, π/2=下)
    int  owner_id = -1;          // 所属玩家
    int  hp = 0;                 // 生命值
    int  kills = 0;              // 击杀数（仅坦克）
    bool alive = true;
    float respawn_timer = 0;     // 重生倒计时
    float fire_cooldown = 0;     // 射击冷却
};

std::vector<Entity> entities;
int next_entity_id = 1;

// ============================================================
// 玩家
// ============================================================
struct Player {
    SOCKET socket = INVALID_SOCKET;
    bool   in_game = false;
    int    entity_id = -1;       // 该玩家的坦克实体 ID
    int    player_index = -1;    // 在游戏中的编号 (0..MAX_PLAYERS-1)
    float  input_vx = 0;         // 最新输入
    float  input_vy = 0;
    bool   want_fire = false;
};

std::vector<Player*> players;
std::queue<Player*>  wait_queue;
bool  game_active = false;

// ============================================================
// 工具函数
// ============================================================
void print_error(const char* where) {
    std::cerr << "[ERROR] " << where << " code=" << WSAGetLastError() << std::endl;
}

float dist(float x1, float y1, float x2, float y2) {
    float dx = x1 - x2, dy = y1 - y2;
    return std::sqrt(dx*dx + dy*dy);
}

/** 在位置 (x,y) 创建实体，返回其 ID */
int spawn_entity(EntityType type, float x, float y,
                 float vx, float vy, float angle, int owner) {
    Entity e;
    e.id       = next_entity_id++;
    e.type     = type;
    e.x = x; e.y = y;
    e.vx = vx; e.vy = vy;
    e.angle    = angle;
    e.owner_id = owner;
    e.hp       = (type == TANK) ? TANK_HP : 1;
    e.alive    = true;
    entities.push_back(e);
    return e.id;
}

/** 获取坦克朝向的单位向量 */
void angle_to_vec(float angle, float& vx, float& vy) {
    vx = std::cos(angle);
    vy = std::sin(angle);
}

Entity* find_entity(int id) {
    for (auto& e : entities) {
        if (e.id == id && e.alive) return &e;
    }
    return nullptr;
}

Entity* find_tank_by_owner(int owner_id) {
    for (auto& e : entities) {
        if (e.type == TANK && e.owner_id == owner_id && e.alive)
            return &e;
    }
    return nullptr;
}

// ============================================================
// 游戏逻辑
// ============================================================

/** 更新所有实体位置 */
void update_physics(float dt) {
    for (auto& e : entities) {
        if (!e.alive) continue;

        if (e.type == TANK) {
            // 坦克移动
            e.x += e.vx * dt;
            e.y += e.vy * dt;

            // 边界限制
            if (e.x < TANK_SIZE) e.x = TANK_SIZE;
            if (e.x > ARENA_W - TANK_SIZE) e.x = ARENA_W - TANK_SIZE;
            if (e.y < TANK_SIZE) e.y = TANK_SIZE;
            if (e.y > ARENA_H - TANK_SIZE) e.y = ARENA_H - TANK_SIZE;

            // 冷却递减
            if (e.fire_cooldown > 0) e.fire_cooldown -= dt;
        }
        else if (e.type == BULLET) {
            // 子弹移动
            e.x += e.vx * dt;
            e.y += e.vy * dt;

            // 飞出边界 → 销毁
            if (e.x < 0 || e.x > ARENA_W || e.y < 0 || e.y > ARENA_H) {
                e.alive = false;
            }
        }
    }
}

/** 碰撞检测 */
void check_collisions(char* kill_msg, int kill_msg_size) {
    kill_msg[0] = '\0';

    for (auto& bullet : entities) {
        if (bullet.type != BULLET || !bullet.alive) continue;

        for (auto& tank : entities) {
            if (tank.type != TANK || !tank.alive) continue;
            if (tank.owner_id == bullet.owner_id) continue;  // 不打自己

            float d = dist(bullet.x, bullet.y, tank.x, tank.y);
            if (d < TANK_SIZE + BULLET_SIZE) {
                // 子弹命中坦克！
                bullet.alive = false;
                tank.hp--;

                if (tank.hp <= 0) {
                    // 坦克被摧毁
                    tank.alive = false;
                    tank.respawn_timer = RESPAWN_TIME;

                    // 加分
                    Entity* killer = find_tank_by_owner(bullet.owner_id);
                    if (killer) killer->kills++;

                    snprintf(kill_msg, kill_msg_size,
                             "KILL %d %d\n", bullet.owner_id, tank.owner_id);

                    std::cout << "[KILL] Player " << bullet.owner_id
                              << " killed Player " << tank.owner_id << std::endl;
                }
                break;  // 一颗子弹只打一个目标
            }
        }
    }

    // 坦克间碰撞（推开）
    for (size_t i = 0; i < entities.size(); i++) {
        if (entities[i].type != TANK || !entities[i].alive) continue;
        for (size_t j = i+1; j < entities.size(); j++) {
            if (entities[j].type != TANK || !entities[j].alive) continue;

            float d = dist(entities[i].x, entities[i].y,
                           entities[j].x, entities[j].y);
            float min_dist = TANK_SIZE * 2;
            if (d < min_dist && d > 0.001f) {
                // 推开
                float overlap = min_dist - d;
                float dx = (entities[i].x - entities[j].x) / d;
                float dy = (entities[i].y - entities[j].y) / d;
                entities[i].x += dx * overlap * 0.5f;
                entities[i].y += dy * overlap * 0.5f;
                entities[j].x -= dx * overlap * 0.5f;
                entities[j].y -= dy * overlap * 0.5f;
            }
        }
    }
}

/** 处理重生 */
void process_respawns(float dt) {
    for (auto& e : entities) {
        if (e.type == TANK && !e.alive) {
            e.respawn_timer -= dt;
            if (e.respawn_timer <= 0) {
                // 重生！
                e.alive = true;
                e.hp = TANK_HP;
                e.x = TANK_SIZE + (float)(rand() % (int)(ARENA_W - TANK_SIZE*2));
                e.y = TANK_SIZE + (float)(rand() % (int)(ARENA_H - TANK_SIZE*2));
                e.vx = e.vy = 0;
                std::cout << "[SPAWN] Player " << e.owner_id << " respawned" << std::endl;
            }
        }
    }
}

/** 把实体列表序列化为快照字符串 */
void build_snapshot(char* out, int out_size) {
    int pos = 0;
    pos += snprintf(out + pos, out_size - pos, "S\n");

    for (auto& e : entities) {
        if (!e.alive) continue;

        if (e.type == TANK) {
            pos += snprintf(out + pos, out_size - pos,
                           "T %d %.0f %.0f %.2f %d %d\n",
                           e.id, e.x, e.y, e.angle, e.hp, e.kills);
        } else if (e.type == BULLET) {
            pos += snprintf(out + pos, out_size - pos,
                           "B %d %.0f %.0f\n", e.id, e.x, e.y);
        }

        if (pos >= out_size - 100) break;  // 安全截断
    }

    pos += snprintf(out + pos, out_size - pos, "E\n");
}

// ============================================================
// 主函数
// ============================================================
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Level 5: Tank Battle Server" << std::endl;
    std::cout << "  Max players: " << MAX_PLAYERS << std::endl;
    std::cout << "========================================" << std::endl;

    srand((unsigned int)time(nullptr));

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET server_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in saddr{};
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons(SERVER_PORT);
    bind(server_fd, (sockaddr*)&saddr, sizeof(saddr));
    listen(server_fd, BACKLOG);

    std::cout << "[OK] Listening on port " << SERVER_PORT << std::endl;
    std::cout << "----------------------------------------" << std::endl;

    auto last_tick = std::chrono::steady_clock::now();
    int tick_count = 0;

    while (true) {
        // --- 构建 fd_set ---
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        SOCKET max_fd = server_fd;

        for (auto* p : players) {
            if (p->socket != INVALID_SOCKET) {
                FD_SET(p->socket, &rfds);
                if (p->socket > max_fd) max_fd = p->socket;
            }
        }

        timeval tv = {0, TICK_MS * 1000};
        int act = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (act == SOCKET_ERROR) { print_error("select"); break; }

        // ----------------------------------------------------
        // 阶段 1: 新连接 + 配对
        // ----------------------------------------------------
        if (FD_ISSET(server_fd, &rfds)) {
            sockaddr_in caddr{};
            int clen = sizeof(caddr);
            SOCKET fd = accept(server_fd, (sockaddr*)&caddr, &clen);

            if (fd != INVALID_SOCKET) {
                auto* p = new Player();
                p->socket = fd;
                players.push_back(p);
                wait_queue.push(p);

                std::cout << "[JOIN] " << inet_ntoa(caddr.sin_addr)
                          << " (queue:" << wait_queue.size() << ")" << std::endl;

                // 通知等待
                send(fd, "WAIT\n", 5, 0);

                // 凑够人就开局
                if ((int)wait_queue.size() >= 2 && !game_active) {
                    game_active = true;
                    entities.clear();
                    next_entity_id = 1;

                    int count = std::min((int)wait_queue.size(), MAX_PLAYERS);
                    for (int i = 0; i < count; i++) {
                        Player* p = wait_queue.front();
                        wait_queue.pop();
                        p->in_game = true;
                        p->player_index = i;

                        // 分散出生位置
                        float sx = TANK_SIZE + 50 +
                                   (float)(rand() % (int)(ARENA_W - TANK_SIZE*2 - 100));
                        float sy = TANK_SIZE + 50 +
                                   (float)(rand() % (int)(ARENA_H - TANK_SIZE*2 - 100));

                        int eid = spawn_entity(TANK, sx, sy, 0, 0, 0, i);
                        p->entity_id = eid;

                        char start_msg[32];
                        snprintf(start_msg, sizeof(start_msg),
                                 "START %d %d\n", i, count);
                        send(p->socket, start_msg, (int)strlen(start_msg), 0);

                        std::cout << "  Player " << i << " tank=" << eid
                                  << " at (" << (int)sx << "," << (int)sy << ")" << std::endl;
                    }
                    std::cout << "[GAME] Started with " << count << " players!" << std::endl;
                }
            }
        }

        // ----------------------------------------------------
        // 阶段 2: 处理玩家输入
        // ----------------------------------------------------
        for (auto* p : players) {
            if (p->socket == INVALID_SOCKET) continue;
            if (!FD_ISSET(p->socket, &rfds)) continue;

            char buf[BUFFER_SIZE];
            int bytes = recv(p->socket, buf, BUFFER_SIZE - 1, 0);

            if (bytes <= 0) {
                std::cout << "[LEAVE] Player disconnected" << std::endl;
                // 摧毁该玩家的坦克
                for (auto& e : entities) {
                    if (e.owner_id >= 0 && find_tank_by_owner(e.owner_id) == &e) {
                        // Mark player's tank as dead
                    }
                }
                closesocket(p->socket);
                p->socket = INVALID_SOCKET;
                p->in_game = false;
                continue;
            }

            buf[bytes] = '\0';
            char* line = strtok(buf, "\n");
            while (line) {
                if (line[0] == 'M' && line[1] == ' ') {
                    float vx, vy;
                    if (sscanf(line+2, "%f %f", &vx, &vy) == 2) {
                        // 归一化
                        float len = std::sqrt(vx*vx + vy*vy);
                        if (len > 1.0f) { vx /= len; vy /= len; }
                        p->input_vx = vx;
                        p->input_vy = vy;
                    }
                }
                else if (line[0] == 'F') {
                    p->want_fire = true;
                }
                line = strtok(nullptr, "\n");
            }
        }

        // ----------------------------------------------------
        // 阶段 3: 应用玩家输入到坦克实体
        // ----------------------------------------------------
        for (auto* p : players) {
            if (!p->in_game) continue;

            Entity* tank = find_entity(p->entity_id);
            if (!tank || !tank->alive) {
                p->want_fire = false;
                continue;
            }

            // 移动
            if (p->input_vx != 0 || p->input_vy != 0) {
                tank->vx = p->input_vx * TANK_SPEED;
                tank->vy = p->input_vy * TANK_SPEED;
                // 朝向 = 移动方向
                tank->angle = std::atan2(p->input_vy, p->input_vx);
            } else {
                tank->vx = tank->vy = 0;
            }

            // 射击
            if (p->want_fire && tank->fire_cooldown <= 0) {
                float bx = tank->x + std::cos(tank->angle) * (TANK_SIZE + 5);
                float by = tank->y + std::sin(tank->angle) * (TANK_SIZE + 5);
                float bvx = std::cos(tank->angle) * BULLET_SPEED;
                float bvy = std::sin(tank->angle) * BULLET_SPEED;
                spawn_entity(BULLET, bx, by, bvx, bvy, tank->angle, p->player_index);

                tank->fire_cooldown = FIRE_COOLDOWN;

                std::cout << "[FIRE] Player " << p->player_index
                          << " bullet at (" << (int)bx << "," << (int)by << ")" << std::endl;
            }
            p->want_fire = false;
        }

        // ----------------------------------------------------
        // 阶段 4: 物理 + 碰撞 + 重生
        // ----------------------------------------------------
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_tick).count();
        last_tick = now;
        if (dt > MAX_DT) dt = MAX_DT;

        if (game_active) {
            update_physics(dt);

            char kill_msg[256];
            check_collisions(kill_msg, sizeof(kill_msg));

            process_respawns(dt);

            // 发送击杀消息
            if (kill_msg[0]) {
                for (auto* p : players) {
                    if (p->socket != INVALID_SOCKET && p->in_game) {
                        send(p->socket, kill_msg, (int)strlen(kill_msg), 0);
                    }
                }
            }
        }

        // ----------------------------------------------------
        // 阶段 5: 状态快照（每3 tick ≈ 50ms）
        // ----------------------------------------------------
        tick_count++;
        if (game_active && tick_count % SNAPSHOT_INTERVAL == 0) {
            char snapshot[BUFFER_SIZE];
            build_snapshot(snapshot, sizeof(snapshot));

            for (auto* p : players) {
                if (p->socket != INVALID_SOCKET && p->in_game) {
                    send(p->socket, snapshot, (int)strlen(snapshot), 0);
                }
            }
        }

        // --- 清理断开玩家 ---
        players.erase(
            std::remove_if(players.begin(), players.end(),
                           [](Player* p) {
                               if (p->socket == INVALID_SOCKET) { delete p; return true; }
                               return false;
                           }),
            players.end()
        );

        // 检查是否所有人都离开了
        int alive_count = 0;
        for (auto* p : players) {
            if (p->in_game) alive_count++;
        }
        if (game_active && alive_count == 0) {
            game_active = false;
            entities.clear();
            std::cout << "[GAME] All players left. Game ended." << std::endl;
        }
    }

    // 清理
    for (auto* p : players) { closesocket(p->socket); delete p; }
    closesocket(server_fd);
    WSACleanup();
    return 0;
}
