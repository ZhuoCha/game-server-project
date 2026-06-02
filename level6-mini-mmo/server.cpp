/**
 * Level 6: Mini MMO 服务器 — 收官之作
 *
 * 融合前5个Level的全部知识，新增 MMO 核心概念。
 *
 * 新增学习目标：
 *   1. AOI (Area of Interest): 玩家只接收视野范围内的实体更新
 *      为什么？假设1000个玩家在线，全图广播 = 1000×999条消息/帧
 *      AOI 只发附近~10个玩家 = 1000×10条消息 → 100倍带宽节省
 *
 *   2. 空间哈希网格 (Spatial Hashing):
 *      地图切成10×10格子，实体放进对应格子
 *      查找附近实体只查当前格+8邻格，O(1)定位，不用遍历全部
 *
 *   3. 怪物 AI: 随机漫游 + 被击杀后重生
 *
 *   4. 持久化: 玩家数据存盘 (save/load)，重连恢复进度
 *
 *   5. 视野管理: Enter/Leave 通知 + 视野内 Move 更新
 *
 * 世界参数:
 *   地图: 3000×3000 单位
 *   网格: 10×10 (每格300×300)
 *   视野: 半径600单位 (≈4格范围)
 *
 * 协议:
 *   客户端 → 服务器:
 *     M <vx> <vy>        移动方向
 *     A                   攻击最近怪物
 *
 *   服务器 → 客户端:
 *     START <id> <name> <x> <y> <hp> <xp> <lvl>   登录成功
 *     S                    快照开始
 *     P <id> <x> <y> <name> <hp> <lvl>            视野内玩家
 *     M <id> <x> <y> <hp> <mtype>                  视野内怪物
 *     E                    快照结束
 *     HIT <dmg> <mob_hp>  攻击命中
 *     KILL <mob_id> <xp>  击杀怪物
 *     STAT <hp> <xp> <lvl> 你的状态更新
 */

#include <iostream>
#include <vector>
#include <queue>
#include <cmath>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <unordered_set>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

// ============================================================
// 世界常量
// ============================================================
constexpr float WORLD_W      = 3000.0f;
constexpr float WORLD_H      = 3000.0f;
constexpr int   GRID_COLS    = 10;
constexpr int   GRID_ROWS    = 10;
constexpr float CELL_W       = WORLD_W / GRID_COLS;  // 300
constexpr float CELL_H       = WORLD_H / GRID_ROWS;  // 300
constexpr float VIEW_RANGE   = 600.0f;    // 玩家视野半径

constexpr float PLAYER_SPEED = 200.0f;
constexpr float MONSTER_SPEED = 60.0f;
constexpr int   PLAYER_BASE_HP = 10;
constexpr int   MONSTER_HP[]  = {3, 5, 8};  // 按怪物类型
constexpr int   MONSTER_XP[]  = {10, 25, 50};
constexpr float ATTACK_RANGE  = 60.0f;
constexpr int   ATTACK_DAMAGE = 2;
constexpr float ATTACK_COOLDOWN = 0.8f;
constexpr float MONSTER_RESPAWN = 8.0f;  // 秒
constexpr int   MAX_MONSTERS  = 30;
constexpr int   XP_PER_LEVEL  = 100;

constexpr int   SERVER_PORT   = 8080;
constexpr int   BACKLOG       = 10;
constexpr int   BUFFER_SIZE   = 16384;
constexpr int   TICK_MS       = 16;
constexpr float MAX_DT        = 0.05f;
constexpr int   SNAPSHOT_INTERVAL = 3;  // ~50ms

// ============================================================
// 实体
// ============================================================
struct Entity {
    int  id = 0;
    int  type = 0;     // 0=player, 1=monster
    float x = 0, y = 0;
    float vx = 0, vy = 0;
    int   hp = 10;
    int   max_hp = 10;
    int   level = 1;
    int   xp = 0;
    int   mtype = 0;   // monster subtype
    float attack_cd = 0;
    float respawn_timer = 0;
    bool  alive = true;
    char  name[32] = "Unknown";

    // 目标（怪物AI）
    float target_x = 0, target_y = 0;
    bool  has_target = false;
};

std::vector<Entity> entities;
int next_entity_id = 1;

// ============================================================
// 空间网格
// ============================================================
// cells[row][col] = 该格子里有哪些 entity 的 index
std::vector<int> grid[GRID_ROWS][GRID_COLS];

int get_cell_col(float x) {
    int c = (int)(x / CELL_W);
    if (c < 0) c = 0;
    if (c >= GRID_COLS) c = GRID_COLS - 1;
    return c;
}
int get_cell_row(float y) {
    int r = (int)(y / CELL_H);
    if (r < 0) r = 0;
    if (r >= GRID_ROWS) r = GRID_ROWS - 1;
    return r;
}

/** 从网格中移除实体 */
void grid_remove(int entity_idx, int col, int row) {
    auto& cell = grid[row][col];
    cell.erase(std::remove(cell.begin(), cell.end(), entity_idx), cell.end());
}

/** 将实体加入网格 */
void grid_insert(int entity_idx) {
    auto& e = entities[entity_idx];
    int col = get_cell_col(e.x);
    int row = get_cell_row(e.y);
    grid[row][col].push_back(entity_idx);
}

/** 查询某位置视野范围内的所有实体索引 */
std::vector<int> query_aoi(float cx, float cy, float range) {
    std::vector<int> result;
    int min_col = get_cell_col(cx - range);
    int max_col = get_cell_col(cx + range);
    int min_row = get_cell_row(cy - range);
    int max_row = get_cell_row(cy + range);

    float range_sq = range * range;

    for (int r = min_row; r <= max_row; r++) {
        for (int c = min_col; c <= max_col; c++) {
            for (int idx : grid[r][c]) {
                auto& e = entities[idx];
                if (!e.alive) continue;
                float dx = e.x - cx, dy = e.y - cy;
                if (dx*dx + dy*dy <= range_sq) {
                    result.push_back(idx);
                }
            }
        }
    }
    return result;
}

// ============================================================
// 玩家
// ============================================================
struct Player {
    SOCKET socket = INVALID_SOCKET;
    int    entity_idx = -1;    // 对应 entities 中的索引
    float  input_vx = 0, input_vy = 0;
    bool   want_attack = false;
    bool   logged_in = false;
    char   name[32] = "Player";

    // AOI 追踪：上次快照时视野内有哪些实体
    std::unordered_set<int> known_entities;
};

std::vector<Player*> players;
std::queue<Player*>  login_queue;

// ============================================================
// 持久化
// ============================================================
constexpr char SAVE_DIR[] = "mmo_saves";

void ensure_save_dir() {
    CreateDirectoryA(SAVE_DIR, NULL);
}

std::string player_filename(const char* name) {
    return std::string(SAVE_DIR) + "/" + name + ".sav";
}

void save_player(Player* p) {
    if (!p || p->entity_idx < 0) return;
    auto& e = entities[p->entity_idx];
    ensure_save_dir();

    std::ofstream f(player_filename(p->name));
    if (!f) return;
    f << "name=" << e.name << "\n";
    f << "x=" << e.x << "\n";
    f << "y=" << e.y << "\n";
    f << "hp=" << e.hp << "\n";
    f << "max_hp=" << e.max_hp << "\n";
    f << "xp=" << e.xp << "\n";
    f << "level=" << e.level << "\n";
    f.close();
    std::cout << "[SAVE] " << p->name << " saved." << std::endl;
}

bool load_player(Player* p) {
    ensure_save_dir();
    std::ifstream f(player_filename(p->name));
    if (!f) return false;

    Entity e;
    e.type = 0;
    e.alive = true;
    e.id = next_entity_id++;

    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "name") strncpy(e.name, val.c_str(), 31);
        else if (key == "x") e.x = (float)atof(val.c_str());
        else if (key == "y") e.y = (float)atof(val.c_str());
        else if (key == "hp") e.hp = atoi(val.c_str());
        else if (key == "max_hp") e.max_hp = atoi(val.c_str());
        else if (key == "xp") e.xp = atoi(val.c_str());
        else if (key == "level") e.level = atoi(val.c_str());
    }
    f.close();

    e.max_hp = PLAYER_BASE_HP + (e.level - 1) * 5;
    if (e.hp > e.max_hp) e.hp = e.max_hp;
    if (e.hp <= 0) e.hp = PLAYER_BASE_HP;

    p->entity_idx = (int)entities.size();
    entities.push_back(e);
    grid_insert(p->entity_idx);

    std::cout << "[LOAD] " << p->name << " loaded (Lv." << e.level
              << ", XP:" << e.xp << ")" << std::endl;
    return true;
}

// ============================================================
// 怪物管理
// ============================================================
void spawn_monster() {
    if ((int)entities.size() >= MAX_MONSTERS + (int)players.size()) return;

    Entity m;
    m.id = next_entity_id++;
    m.type = 1;  // monster
    m.x = 50.0f + (float)(rand() % (int)(WORLD_W - 100));
    m.y = 50.0f + (float)(rand() % (int)(WORLD_H - 100));
    m.mtype = rand() % 3;
    m.hp = MONSTER_HP[m.mtype];
    m.max_hp = m.hp;
    m.alive = true;
    snprintf(m.name, 31, "Mob%d", m.id);
    m.has_target = false;

    entities.push_back(m);
    int idx = (int)entities.size() - 1;
    grid_insert(idx);
}

void respawn_monsters(float dt) {
    for (auto& e : entities) {
        if (e.type == 1 && !e.alive) {
            e.respawn_timer -= dt;
            if (e.respawn_timer <= 0) {
                e.alive = true;
                e.hp = MONSTER_HP[e.mtype];
                e.x = 50.0f + (float)(rand() % (int)(WORLD_W - 100));
                e.y = 50.0f + (float)(rand() % (int)(WORLD_H - 100));
                e.has_target = false;
                grid_insert((int)(&e - &entities[0]));
            }
        }
    }
}

// ============================================================
// 游戏逻辑
// ============================================================
void update_monster_ai(float dt) {
    for (size_t i = 0; i < entities.size(); i++) {
        auto& e = entities[i];
        if (e.type != 1 || !e.alive) continue;

        // 选新目标
        if (!e.has_target || (std::abs(e.x - e.target_x) < 5 && std::abs(e.y - e.target_y) < 5)) {
            e.target_x = 50.0f + (float)(rand() % (int)(WORLD_W - 100));
            e.target_y = 50.0f + (float)(rand() % (int)(WORLD_H - 100));
            e.has_target = true;
        }

        // 朝目标移动
        float dx = e.target_x - e.x;
        float dy = e.target_y - e.y;
        float dist = std::sqrt(dx*dx + dy*dy);
        if (dist > 1.0f) {
            e.x += (dx / dist) * MONSTER_SPEED * dt;
            e.y += (dy / dist) * MONSTER_SPEED * dt;
        }

        // 更新网格位置
        int old_col = get_cell_col(e.x - (dx/dist) * MONSTER_SPEED * dt);
        int old_row = get_cell_row(e.y - (dy/dist) * MONSTER_SPEED * dt);
        int new_col = get_cell_col(e.x);
        int new_row = get_cell_row(e.y);
        if (old_col != new_col || old_row != new_row) {
            grid_remove((int)i, old_col, old_row);
            grid_insert((int)i);
        }
    }
}

void update_player_movement(float dt) {
    for (size_t i = 0; i < entities.size(); i++) {
        auto& e = entities[i];
        if (e.type != 0 || !e.alive) continue;

        // 找到对应的 player
        Player* p = nullptr;
        for (auto* pl : players) {
            if (pl->entity_idx == (int)i) { p = pl; break; }
        }
        if (!p) continue;

        // 应用输入
        if (p->input_vx != 0 || p->input_vy != 0) {
            float old_x = e.x, old_y = e.y;
            e.x += p->input_vx * PLAYER_SPEED * dt;
            e.y += p->input_vy * PLAYER_SPEED * dt;

            // 边界
            if (e.x < 10) e.x = 10;
            if (e.x > WORLD_W - 10) e.x = WORLD_W - 10;
            if (e.y < 10) e.y = 10;
            if (e.y > WORLD_H - 10) e.y = WORLD_H - 10;

            // 更新网格
            int oc = get_cell_col(old_x), or_ = get_cell_row(old_y);
            int nc = get_cell_col(e.x), nr = get_cell_row(e.y);
            if (oc != nc || or_ != nr) {
                grid_remove((int)i, oc, or_);
                grid_insert((int)i);
            }
        }

        // 攻击冷却递减
        if (e.attack_cd > 0) e.attack_cd -= dt;
    }
}

/** 处理玩家攻击：找最近怪物 */
void handle_attack(Player* p, char* out_msg, int out_size) {
    out_msg[0] = '\0';
    if (!p || p->entity_idx < 0) return;
    auto& player_ent = entities[p->entity_idx];
    if (player_ent.attack_cd > 0) return;

    // 查询附近实体
    auto nearby = query_aoi(player_ent.x, player_ent.y, ATTACK_RANGE);
    int closest_monster = -1;
    float closest_dist = ATTACK_RANGE + 1;

    for (int idx : nearby) {
        auto& e = entities[idx];
        if (e.type != 1 || !e.alive) continue;
        float d = std::sqrt(
            (e.x - player_ent.x)*(e.x - player_ent.x) +
            (e.y - player_ent.y)*(e.y - player_ent.y));
        if (d < closest_dist) {
            closest_dist = d;
            closest_monster = idx;
        }
    }

    if (closest_monster >= 0) {
        auto& mob = entities[closest_monster];
        mob.hp -= ATTACK_DAMAGE;
        player_ent.attack_cd = ATTACK_COOLDOWN;

        if (mob.hp <= 0) {
            // 击杀！
            mob.alive = false;
            mob.respawn_timer = MONSTER_RESPAWN;
            int xp_gain = MONSTER_XP[mob.mtype];
            player_ent.xp += xp_gain;

            // 升级检查
            int new_level = 1 + player_ent.xp / XP_PER_LEVEL;
            if (new_level > player_ent.level) {
                player_ent.level = new_level;
                player_ent.max_hp = PLAYER_BASE_HP + (new_level - 1) * 5;
                player_ent.hp = player_ent.max_hp;
            }

            // 从网格移除
            int mc = get_cell_col(mob.x), mr = get_cell_row(mob.y);
            grid_remove(closest_monster, mc, mr);

            snprintf(out_msg, out_size, "KILL %d %d\n", mob.id, xp_gain);
        } else {
            snprintf(out_msg, out_size, "HIT %d %d\n", ATTACK_DAMAGE, mob.hp);
        }
    }
}

// ============================================================
// AOI 快照构建
// ============================================================
void build_aoi_snapshot(Player* p, char* out, int out_size) {
    if (!p || p->entity_idx < 0) return;
    auto& player_ent = entities[p->entity_idx];

    auto visible = query_aoi(player_ent.x, player_ent.y, VIEW_RANGE);

    // 比较新旧集合
    std::unordered_set<int> new_set(visible.begin(), visible.end());

    int pos = 0;
    pos += snprintf(out + pos, out_size - pos, "S\n");

    // 发送视野内的实体
    for (int idx : visible) {
        auto& e = entities[idx];
        if (&e == &player_ent) continue;  // 不发送自己

        if (e.type == 0) {
            pos += snprintf(out + pos, out_size - pos,
                           "P %d %.0f %.0f %s %d %d\n",
                           e.id, e.x, e.y, e.name, e.hp, e.level);
        } else if (e.type == 1) {
            pos += snprintf(out + pos, out_size - pos,
                           "M %d %.0f %.0f %d %d\n",
                           e.id, e.x, e.y, e.hp, e.mtype);
        }

        if (pos >= out_size - 200) break;
    }

    // 自己的状态
    pos += snprintf(out + pos, out_size - pos,
                   "STAT %d %d %d\n",
                   player_ent.hp, player_ent.xp, player_ent.level);

    pos += snprintf(out + pos, out_size - pos, "E\n");
    p->known_entities = new_set;
}

// ============================================================
// 工具函数
// ============================================================
void print_error(const char* w) {
    std::cerr << "[ERROR] " << w << " code=" << WSAGetLastError() << std::endl;
}

// ============================================================
// 主函数
// ============================================================
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Level 6: Mini MMO Server" << std::endl;
    std::cout << "  World: " << (int)WORLD_W << "x" << (int)WORLD_H << std::endl;
    std::cout << "  Grid: " << GRID_COLS << "x" << GRID_ROWS
              << " cells, View: " << (int)VIEW_RANGE << " units" << std::endl;
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

    // 初始怪物
    for (int i = 0; i < 15; i++) spawn_monster();

    auto last_tick = std::chrono::steady_clock::now();
    int tick_count = 0;

    while (true) {
        // --- fd_set ---
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

        // --- 新连接 ---
        if (FD_ISSET(server_fd, &rfds)) {
            sockaddr_in caddr{};
            int clen = sizeof(caddr);
            SOCKET fd = accept(server_fd, (sockaddr*)&caddr, &clen);
            if (fd != INVALID_SOCKET) {
                auto* p = new Player();
                p->socket = fd;
                snprintf(p->name, 31, "Hero%d", rand() % 10000);
                players.push_back(p);

                // 尝试加载存盘
                if (!load_player(p)) {
                    // 新玩家
                    Entity e;
                    e.type = 0;
                    e.alive = true;
                    e.id = next_entity_id++;
                    e.x = 100.0f + (float)(rand() % 500);
                    e.y = 100.0f + (float)(rand() % 500);
                    e.hp = PLAYER_BASE_HP;
                    e.max_hp = PLAYER_BASE_HP;
                    e.level = 1;
                    e.xp = 0;
                    strncpy(e.name, p->name, 31);
                    p->entity_idx = (int)entities.size();
                    entities.push_back(e);
                    grid_insert(p->entity_idx);
                }

                auto& ent = entities[p->entity_idx];
                p->logged_in = true;

                char start[256];
                snprintf(start, sizeof(start),
                         "START %d %s %.0f %.0f %d %d %d\n",
                         ent.id, ent.name, ent.x, ent.y,
                         ent.hp, ent.xp, ent.level);
                send(fd, start, (int)strlen(start), 0);

                std::cout << "[LOGIN] " << p->name
                          << " (Lv." << ent.level << ") at ("
                          << (int)ent.x << "," << (int)ent.y << ")" << std::endl;
            }
        }

        // --- 客户端消息 ---
        for (auto* p : players) {
            if (p->socket == INVALID_SOCKET || !FD_ISSET(p->socket, &rfds)) continue;

            char buf[1024];
            int bytes = recv(p->socket, buf, 1023, 0);
            if (bytes <= 0) {
                // 断开连接
                std::cout << "[LOGOUT] " << p->name << std::endl;
                if (p->logged_in && p->entity_idx >= 0) {
                    save_player(p);
                    int idx = p->entity_idx;
                    auto& e = entities[idx];
                    int c = get_cell_col(e.x), r = get_cell_row(e.y);
                    grid_remove(idx, c, r);
                    e.alive = false;
                }
                closesocket(p->socket);
                p->socket = INVALID_SOCKET;
                p->logged_in = false;
                continue;
            }

            buf[bytes] = '\0';
            char* line = strtok(buf, "\n");
            while (line) {
                if (line[0] == 'M' && line[1] == ' ') {
                    float vx, vy;
                    if (sscanf(line+2, "%f %f", &vx, &vy) == 2) {
                        float len = std::sqrt(vx*vx + vy*vy);
                        if (len > 1.0f) { vx /= len; vy /= len; }
                        p->input_vx = vx;
                        p->input_vy = vy;
                    }
                }
                else if (line[0] == 'A') {
                    p->want_attack = true;
                }
                line = strtok(nullptr, "\n");
            }
        }

        // --- 物理更新 ---
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_tick).count();
        last_tick = now;
        if (dt > MAX_DT) dt = MAX_DT;

        update_player_movement(dt);
        update_monster_ai(dt);
        respawn_monsters(dt);

        // 处理攻击
        for (auto* p : players) {
            if (!p->want_attack || p->socket == INVALID_SOCKET) continue;
            char msg[256];
            handle_attack(p, msg, sizeof(msg));
            if (msg[0]) send(p->socket, msg, (int)strlen(msg), 0);
            p->want_attack = false;
        }

        // 补充怪物
        int monster_count = 0;
        for (auto& e : entities) {
            if (e.type == 1 && e.alive) monster_count++;
        }
        if (monster_count < 15) spawn_monster();

        // --- AOI 快照 ---
        tick_count++;
        if (tick_count % SNAPSHOT_INTERVAL == 0) {
            for (auto* p : players) {
                if (p->socket == INVALID_SOCKET || !p->logged_in) continue;
                char snapshot[BUFFER_SIZE];
                build_aoi_snapshot(p, snapshot, sizeof(snapshot));
                send(p->socket, snapshot, (int)strlen(snapshot), 0);
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
    }

    // 保存所有在线玩家
    for (auto* p : players) {
        if (p->logged_in) save_player(p);
        closesocket(p->socket);
        delete p;
    }
    closesocket(server_fd);
    WSACleanup();
    return 0;
}
