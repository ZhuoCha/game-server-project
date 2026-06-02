/**
 * Level 3: 井字棋对战服务器
 *
 * 在 Level 2 的 select() 多路复用基础上，加入游戏逻辑。
 *
 * 新增学习目标：
 *   1. 游戏状态机: WAITING（等对手）→ IN_GAME（对战中）→ 结束
 *   2. 匹配系统: 等待队列 → 2人凑齐 → 创建对局
 *   3. 回合制协议: BOARD 通知轮到谁 → MOVE 提交操作 → OK/INVALID 反馈
 *   4. 胜负判定: 8条线的检查（3行+3列+2对角线）
 *   5. 游戏状态的服务器权威: 客户端只负责展示，所有逻辑在服务端
 *
 * 协议格式（纯文本，换行分隔）:
 *   服务器 → 客户端:
 *     WAIT                    等待对手中
 *     START <X|O> <对手名>     游戏开始，告知你的棋子和对手
 *     BOARD <9格> <turn>      当前棋盘 + 轮到谁
 *     OK                      你的落子被接受
 *     INVALID <原因>           落子被拒绝
 *     WIN <board>             你赢了
 *     LOSE <board>            你输了
 *     DRAW <board>            平局
 *     OPPONENT_LEFT           对手断线，你获胜
 *
 *   客户端 → 服务器:
 *     MOVE <0-8>              在指定位置落子
 *     QUIT                    退出
 */

#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <cstring>
#include <algorithm>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

// ============================================================
// 常量
// ============================================================
constexpr int  SERVER_PORT   = 8080;
constexpr int  BACKLOG       = 5;
constexpr int  BUFFER_SIZE   = 1024;
constexpr int  MAX_CLIENTS   = 60;
constexpr int  MAX_NAME_LEN  = 32;

constexpr int  BOARD_SIZE    = 9;   // 3x3

// 8条胜利线: 3行 + 3列 + 2对角线
constexpr int WIN_LINES[8][3] = {
    {0, 1, 2}, {3, 4, 5}, {6, 7, 8},  // 行
    {0, 3, 6}, {1, 4, 7}, {2, 5, 8},  // 列
    {0, 4, 8}, {2, 4, 6}               // 对角线
};

// ============================================================
// 游戏状态机
// ============================================================
enum class PlayerState {
    WAITING,    // 在匹配队列中等待对手
    IN_GAME,    // 正在对局中
};

// ============================================================
// 玩家
// ============================================================
struct Player {
    SOCKET      socket = INVALID_SOCKET;
    sockaddr_in addr{};
    char        name[MAX_NAME_LEN] = "Player";
    PlayerState state = PlayerState::WAITING;

    // 指向当前对局（IN_GAME 时有效）
    struct Game* game = nullptr;
    char symbol = ' ';  // 'X' or 'O'
};

// ============================================================
// 对局
// ============================================================
struct Game {
    Player* player_x = nullptr;  // X 先手
    Player* player_o = nullptr;  // O 后手

    char board[BOARD_SIZE];      // '.'=空, 'X', 'O'
    char turn = 'X';             // 当前轮到谁
    bool game_over = false;
    int  move_count = 0;

    Game() {
        for (int i = 0; i < BOARD_SIZE; i++) board[i] = '.';
    }
};

// 全局数据
std::vector<Player*> players;      // 所有已连接的玩家
std::queue<Player*>  wait_queue;   // 匹配等待队列
std::vector<Game*>   games;        // 进行中的对局

// ============================================================
// 辅助函数
// ============================================================
void print_error(const char* where) {
    std::cerr << "[ERROR] " << where
              << " failed, code=" << WSAGetLastError() << std::endl;
}

/**
 * 发送一行协议消息（自动追加换行）
 */
void send_msg(SOCKET fd, const char* msg) {
    int len = (int)strlen(msg);
    char buf[BUFFER_SIZE];
    snprintf(buf, sizeof(buf), "%s\n", msg);
    send(fd, buf, (int)strlen(buf), 0);
}

/**
 * 检查是否有玩家获胜
 * 返回 'X', 'O'，或 0 表示无人获胜
 */
char check_winner(const char board[BOARD_SIZE]) {
    for (int i = 0; i < 8; i++) {
        char a = board[WIN_LINES[i][0]];
        char b = board[WIN_LINES[i][1]];
        char c = board[WIN_LINES[i][2]];
        if (a != '.' && a == b && b == c) {
            return a;  // 三子连珠！
        }
    }
    return 0;
}

/**
 * 检查棋盘是否已满（平局）
 */
bool is_board_full(const char board[BOARD_SIZE]) {
    for (int i = 0; i < BOARD_SIZE; i++) {
        if (board[i] == '.') return false;
    }
    return true;
}

/**
 * 构建 BOARD 消息: "BOARD ......... X"
 */
void build_board_msg(const char board[BOARD_SIZE], char turn, char* out, int out_size) {
    char board_str[BOARD_SIZE + 1];
    for (int i = 0; i < BOARD_SIZE; i++) board_str[i] = board[i];
    board_str[BOARD_SIZE] = '\0';
    snprintf(out, out_size, "BOARD %s %c", board_str, turn);
}

/**
 * 发送棋盘状态给对局的两位玩家
 */
void send_board_to_both(Game* game) {
    char msg[BUFFER_SIZE];
    build_board_msg(game->board, game->turn, msg, sizeof(msg));

    char msg_x[BUFFER_SIZE];
    snprintf(msg_x, sizeof(msg_x), "%s", msg);
    send_msg(game->player_x->socket, msg_x);

    char msg_o[BUFFER_SIZE];
    snprintf(msg_o, sizeof(msg_o), "%s", msg);
    send_msg(game->player_o->socket, msg_o);
}

/**
 * 结束对局，清理资源
 */
void end_game(Game* game) {
    game->game_over = true;

    // 断开双方连接（简单处理：让他们重新连接来玩下一局）
    if (game->player_x) {
        game->player_x->state = PlayerState::WAITING;
        game->player_x->game  = nullptr;
        game->player_x->symbol = ' ';
        closesocket(game->player_x->socket);
        game->player_x->socket = INVALID_SOCKET;
    }
    if (game->player_o) {
        game->player_o->state = PlayerState::WAITING;
        game->player_o->game  = nullptr;
        game->player_o->symbol = ' ';
        closesocket(game->player_o->socket);
        game->player_o->socket = INVALID_SOCKET;
    }
}

/**
 * 处理玩家落子请求
 */
void handle_move(Player* player, int pos) {
    Game* game = player->game;
    if (!game || game->game_over) return;

    // 检查 1: 是不是你的回合？
    if (player->symbol != game->turn) {
        send_msg(player->socket, "INVALID Not your turn");
        return;
    }

    // 检查 2: 位置是否合法？
    if (pos < 0 || pos >= BOARD_SIZE) {
        send_msg(player->socket, "INVALID Position 0-8 only");
        return;
    }

    // 检查 3: 位置是否已被占用？
    if (game->board[pos] != '.') {
        send_msg(player->socket, "INVALID Position already taken");
        return;
    }

    // --- 落子有效 ---
    game->board[pos] = player->symbol;
    game->move_count++;
    send_msg(player->socket, "OK");

    std::cout << "[GAME] " << player->name << " (" << player->symbol
              << ") placed at " << pos << std::endl;

    // 检查胜负
    char winner = check_winner(game->board);
    if (winner != 0) {
        // 有人赢了！
        Player* win_player = (winner == 'X') ? game->player_x : game->player_o;
        Player* lose_player = (winner == 'X') ? game->player_o : game->player_x;

        char board_str[BOARD_SIZE + 1];
        for (int i = 0; i < BOARD_SIZE; i++) board_str[i] = game->board[i];
        board_str[BOARD_SIZE] = '\0';

        char win_msg[BUFFER_SIZE], lose_msg[BUFFER_SIZE];
        snprintf(win_msg, sizeof(win_msg), "WIN %s", board_str);
        snprintf(lose_msg, sizeof(lose_msg), "LOSE %s", board_str);

        send_msg(win_player->socket, win_msg);
        send_msg(lose_player->socket, lose_msg);

        std::cout << "[GAME] " << win_player->name << " wins! "
                  << lose_player->name << " loses." << std::endl;

        end_game(game);
        return;
    }

    // 检查平局
    if (is_board_full(game->board)) {
        char board_str[BOARD_SIZE + 1];
        for (int i = 0; i < BOARD_SIZE; i++) board_str[i] = game->board[i];
        board_str[BOARD_SIZE] = '\0';

        char draw_msg[BUFFER_SIZE];
        snprintf(draw_msg, sizeof(draw_msg), "DRAW %s", board_str);

        send_msg(game->player_x->socket, draw_msg);
        send_msg(game->player_o->socket, draw_msg);

        std::cout << "[GAME] Draw!" << std::endl;

        end_game(game);
        return;
    }

    // 切换回合
    game->turn = (game->turn == 'X') ? 'O' : 'X';
    send_board_to_both(game);
}

/**
 * 创建对局：从等待队列取出两个玩家配对
 */
void create_game() {
    if (wait_queue.size() < 2) return;

    Player* p1 = wait_queue.front(); wait_queue.pop();
    Player* p2 = wait_queue.front(); wait_queue.pop();

    Game* game = new Game();
    game->player_x = p1;
    game->player_o = p2;
    game->turn     = 'X';  // X 先手

    p1->state  = PlayerState::IN_GAME;
    p1->game   = game;
    p1->symbol = 'X';

    p2->state  = PlayerState::IN_GAME;
    p2->game   = game;
    p2->symbol = 'O';

    games.push_back(game);

    // 通知双方游戏开始
    char start_x[BUFFER_SIZE], start_o[BUFFER_SIZE];
    snprintf(start_x, sizeof(start_x), "START X %s", p2->name);
    snprintf(start_o, sizeof(start_o), "START O %s", p1->name);
    send_msg(p1->socket, start_x);
    send_msg(p2->socket, start_o);

    std::cout << "[MATCH] " << p1->name << " (X) vs "
              << p2->name << " (O)" << std::endl;

    // 发送初始棋盘
    send_board_to_both(game);
}

// ============================================================
// 主函数
// ============================================================
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Level 3: Tic-Tac-Toe Server (Port " << SERVER_PORT << ")" << std::endl;
    std::cout << "========================================" << std::endl;

    // --- Winsock 初始化 ---
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        print_error("WSAStartup");
        return 1;
    }

    // --- 创建服务器 socket ---
    SOCKET server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == INVALID_SOCKET) {
        print_error("socket");
        WSACleanup();
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(SERVER_PORT);

    if (bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        print_error("bind");
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    if (listen(server_socket, BACKLOG) == SOCKET_ERROR) {
        print_error("listen");
        closesocket(server_socket);
        WSACleanup();
        return 1;
    }

    std::cout << "[OK] Tic-Tac-Toe server listening on port " << SERVER_PORT << std::endl;
    std::cout << "[OK] Waiting for players to connect..." << std::endl;
    std::cout << "----------------------------------------" << std::endl;

    // ============================================================
    // select() 主循环
    // ============================================================
    while (true) {
        // --- 构建 fd_set ---
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_socket, &read_fds);
        SOCKET max_fd = server_socket;

        for (auto* player : players) {
            if (player->socket != INVALID_SOCKET) {
                FD_SET(player->socket, &read_fds);
                if (player->socket > max_fd) max_fd = player->socket;
            }
        }

        int activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (activity == SOCKET_ERROR) {
            print_error("select");
            break;
        }

        // --- 新连接 ---
        if (FD_ISSET(server_socket, &read_fds)) {
            sockaddr_in client_addr{};
            int addr_len = sizeof(client_addr);
            SOCKET client_fd = accept(server_socket, (sockaddr*)&client_addr, &addr_len);

            if (client_fd != INVALID_SOCKET && (int)players.size() < MAX_CLIENTS) {
                Player* player = new Player();
                player->socket = client_fd;
                player->addr   = client_addr;
                player->state  = PlayerState::WAITING;
                snprintf(player->name, MAX_NAME_LEN, "P%d", (int)client_fd);

                players.push_back(player);
                wait_queue.push(player);

                std::cout << "[JOIN] " << player->name
                          << " from " << inet_ntoa(client_addr.sin_addr)
                          << " (queue: " << wait_queue.size() << ")" << std::endl;

                send_msg(client_fd, "WAIT");

                // 检查是否可以配对
                create_game();
            }
        }

        // --- 客户端消息 ---
        for (auto* player : players) {
            if (player->socket == INVALID_SOCKET) continue;
            if (!FD_ISSET(player->socket, &read_fds)) continue;

            char buffer[BUFFER_SIZE];
            int bytes = recv(player->socket, buffer, BUFFER_SIZE - 1, 0);

            if (bytes <= 0) {
                // 玩家断开连接
                std::cout << "[LEAVE] " << player->name << " disconnected." << std::endl;

                // 如果正在对局中，通知对手
                if (player->state == PlayerState::IN_GAME && player->game) {
                    Game* game = player->game;
                    Player* opponent = (game->player_x == player)
                                       ? game->player_o : game->player_x;

                    if (opponent && opponent->socket != INVALID_SOCKET) {
                        send_msg(opponent->socket, "OPPONENT_LEFT");
                    }
                    end_game(game);
                }

                // 从等待队列中移除（如果还在排队）
                // 简单处理：标记为无效，稍后清理
                closesocket(player->socket);
                player->socket = INVALID_SOCKET;
                continue;
            }

            buffer[bytes] = '\0';

            // 去掉末尾换行符
            for (int i = 0; buffer[i]; i++) {
                if (buffer[i] == '\n' || buffer[i] == '\r') {
                    buffer[i] = '\0';
                    break;
                }
            }

            // 解析客户端消息
            if (strncmp(buffer, "MOVE ", 5) == 0) {
                int pos = atoi(buffer + 5);
                handle_move(player, pos);
            }
            else if (strncmp(buffer, "QUIT", 4) == 0) {
                if (player->state == PlayerState::IN_GAME && player->game) {
                    Game* game = player->game;
                    Player* opponent = (game->player_x == player)
                                       ? game->player_o : game->player_x;
                    if (opponent && opponent->socket != INVALID_SOCKET) {
                        send_msg(opponent->socket, "OPPONENT_LEFT");
                    }
                    end_game(game);
                }
                closesocket(player->socket);
                player->socket = INVALID_SOCKET;
            }
        }

        // --- 清理已断开的玩家 ---
        players.erase(
            std::remove_if(players.begin(), players.end(),
                           [](Player* p) {
                               if (p->socket == INVALID_SOCKET) {
                                   delete p;
                                   return true;
                               }
                               return false;
                           }),
            players.end()
        );

        // --- 清理已结束的对局 ---
        games.erase(
            std::remove_if(games.begin(), games.end(),
                           [](Game* g) {
                               if (g->game_over) {
                                   delete g;
                                   return true;
                               }
                               return false;
                           }),
            games.end()
        );

        // --- 检查是否有新对局可以创建 ---
        // 清理无效的队列条目
        std::queue<Player*> clean_queue;
        while (!wait_queue.empty()) {
            Player* p = wait_queue.front(); wait_queue.pop();
            if (p->socket != INVALID_SOCKET && p->state == PlayerState::WAITING) {
                clean_queue.push(p);
            }
        }
        wait_queue = clean_queue;
        create_game();
    }

    // --- 清理 ---
    for (auto* p : players) { if (p->socket != INVALID_SOCKET) closesocket(p->socket); delete p; }
    for (auto* g : games) delete g;
    closesocket(server_socket);
    WSACleanup();
    std::cout << "[OK] Server shutdown." << std::endl;
    return 0;
}
