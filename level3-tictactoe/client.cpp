/**
 * Level 3: 井字棋客户端
 *
 * 接收服务器协议消息，渲染棋盘，处理用户输入。
 *
 * 协议消息处理:
 *   WAIT        → 显示"等待对手中..."
 *   START X/O名 → 显示游戏开始
 *   BOARD ... X → 渲染棋盘，提示轮到谁
 *   OK          → （内部确认，不显示）
 *   INVALID 原因 → 显示错误，等待重新输入
 *   WIN/LOSE/DRAW → 显示结果
 *   OPPONENT_LEFT → 对手断线
 */

#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <cstring>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

constexpr int  SERVER_PORT = 8080;
constexpr char SERVER_IP[] = "127.0.0.1";
constexpr int  BUFFER_SIZE = 1024;

std::atomic<bool> running(true);
SOCKET g_sock = INVALID_SOCKET;

// 客户端游戏状态
char my_symbol = ' ';       // 'X' or 'O'
char current_turn = ' ';    // 当前该谁下
bool my_turn = false;       // 是否轮到我
bool in_game = false;

void print_error(const char* where) {
    std::cerr << "[ERROR] " << where
              << " failed, code=" << WSAGetLastError() << std::endl;
}

// ============================================================
// 渲染 3×3 棋盘
// ============================================================
void render_board(const char* board_str) {
    std::cout << std::endl;
    std::cout << "  Current Board:" << std::endl;
    std::cout << std::endl;

    for (int row = 0; row < 3; row++) {
        std::cout << "    ";
        for (int col = 0; col < 3; col++) {
            int idx = row * 3 + col;
            char cell = board_str[idx];
            if (cell == '.') {
                // 空位显示数字（方便选择）
                std::cout << idx;
            } else {
                std::cout << cell;
            }
            if (col < 2) std::cout << " | ";
        }
        std::cout << std::endl;
        if (row < 2) std::cout << "   ---+---+---" << std::endl;
    }
    std::cout << std::endl;
}

// ============================================================
// 接收线程：处理服务器发来的协议消息
// ============================================================
void recv_thread() {
    char buffer[BUFFER_SIZE];
    char line[BUFFER_SIZE];

    while (running) {
        int bytes = recv(g_sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) {
            if (bytes == 0) {
                std::cout << "\n[SERVER] Connection closed." << std::endl;
            } else {
                print_error("recv");
            }
            running = false;
            break;
        }

        buffer[bytes] = '\0';

        // 按行分割（服务器发来的消息用 \n 分隔）
        char* token = strtok(buffer, "\n");
        while (token != nullptr) {
            strncpy(line, token, BUFFER_SIZE - 1);
            line[BUFFER_SIZE - 1] = '\0';

            // --- 解析协议消息 ---
            if (strncmp(line, "WAIT", 4) == 0) {
                std::cout << "[...] Waiting for an opponent..." << std::endl;
            }
            else if (strncmp(line, "START ", 6) == 0) {
                char symbol;
                char opp_name[64];
                sscanf(line + 6, "%c %63s", &symbol, opp_name);

                my_symbol = symbol;
                in_game = true;

                std::cout << std::endl;
                std::cout << "========================================" << std::endl;
                std::cout << "  Game Start!" << std::endl;
                std::cout << "  You play: " << my_symbol << std::endl;
                std::cout << "  Opponent: " << opp_name << std::endl;
                std::cout << "========================================" << std::endl;
            }
            else if (strncmp(line, "BOARD ", 6) == 0) {
                // 格式: BOARD ......... X
                char board_str[10], turn;
                sscanf(line + 6, "%9s %c", board_str, &turn);

                current_turn = turn;
                my_turn = (turn == my_symbol);

                render_board(board_str);

                if (my_turn) {
                    std::cout << ">>> YOUR TURN (" << my_symbol
                              << ") — enter position 0-8: ";
                } else {
                    std::cout << "[...] Waiting for opponent's move..." << std::endl;
                }
            }
            else if (strncmp(line, "OK", 2) == 0) {
                // 落子被接受，静默处理
            }
            else if (strncmp(line, "INVALID ", 8) == 0) {
                std::cout << "[ERROR] " << (line + 8) << std::endl;
                std::cout << ">>> Try again (0-8): ";
            }
            else if (strncmp(line, "WIN ", 4) == 0) {
                char final_board[10];
                sscanf(line + 4, "%9s", final_board);
                render_board(final_board);
                std::cout << std::endl;
                std::cout << "  ★ ★ ★  YOU WIN!  ★ ★ ★" << std::endl;
                std::cout << std::endl;
                in_game = false;
            }
            else if (strncmp(line, "LOSE ", 5) == 0) {
                char final_board[10];
                sscanf(line + 5, "%9s", final_board);
                render_board(final_board);
                std::cout << std::endl;
                std::cout << "  ✗ ✗ ✗  YOU LOSE!  ✗ ✗ ✗" << std::endl;
                std::cout << std::endl;
                in_game = false;
            }
            else if (strncmp(line, "DRAW ", 5) == 0) {
                char final_board[10];
                sscanf(line + 5, "%9s", final_board);
                render_board(final_board);
                std::cout << std::endl;
                std::cout << "  ===  IT'S A DRAW!  ===" << std::endl;
                std::cout << std::endl;
                in_game = false;
            }
            else if (strncmp(line, "OPPONENT_LEFT", 13) == 0) {
                std::cout << "[INFO] Your opponent disconnected. You win by forfeit!"
                          << std::endl;
                in_game = false;
            }

            token = strtok(nullptr, "\n");
        }
    }
}

// ============================================================
// 主函数
// ============================================================
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Level 3: Tic-Tac-Toe Client" << std::endl;
    std::cout << "========================================" << std::endl;

    // --- Winsock 初始化 ---
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        print_error("WSAStartup");
        return 1;
    }

    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock == INVALID_SOCKET) {
        print_error("socket");
        WSACleanup();
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons(SERVER_PORT);
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    std::cout << "[...] Connecting to server..." << std::endl;

    if (connect(g_sock, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        print_error("connect");
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }
    std::cout << "[OK] Connected!" << std::endl;

    // --- 启动接收线程 ---
    std::thread recv(recv_thread);

    // --- 主线程：读取用户输入 ---
    std::string input;
    while (running && std::getline(std::cin, input)) {
        if (input.empty()) continue;

        if (input == "/quit" || input == "/q") {
            send(g_sock, "QUIT\n", 5, 0);
            running = false;
            break;
        }

        if (!in_game) {
            std::cout << "[...] Game not in progress. Waiting..." << std::endl;
            continue;
        }

        if (!my_turn) {
            std::cout << "[...] Not your turn. Please wait." << std::endl;
            continue;
        }

        // 尝试解析数字 0-8
        int pos = atoi(input.c_str());
        if (pos >= 0 && pos <= 8) {
            char move_cmd[32];
            snprintf(move_cmd, sizeof(move_cmd), "MOVE %d\n", pos);
            send(g_sock, move_cmd, (int)strlen(move_cmd), 0);
        } else {
            std::cout << "[ERROR] Enter a position 0-8, or /quit to leave."
                      << std::endl;
            std::cout << ">>> Your move (0-8): ";
        }
    }

    // --- 清理 ---
    running = false;
    closesocket(g_sock);
    WSACleanup();
    if (recv.joinable()) recv.join();
    std::cout << "[OK] Client shutdown." << std::endl;
    return 0;
}
