/**
 * Level 2: 多人聊天室服务器
 *
 * 在 Level 1 的 TCP 基础上，用 select() 实现一个服务器同时处理多个客户端。
 *
 * 新增学习目标：
 *   1. 理解 I/O 多路复用：为什么不用"一个客户端一个线程"？
 *      → 线程切换开销大，C10K 问题是 select/epoll 的起源
 *   2. 掌握 select() 的用法:
 *      FD_ZERO / FD_SET / FD_ISSET — 位图操作文件描述符集合
 *   3. 理解"非阻塞 accept"：select 告诉你"有人来了"才去 accept
 *   4. 理解广播：收到一条消息 → 转发给所有其他客户端
 *   5. 理解客户端生命周期管理：join / message / leave
 *
 * select() 的工作方式：
 *   while(true) {
 *       fd_set = {server_socket, client1, client2, ...};  // 感兴趣的所有 fd
 *       select(max_fd+1, &fd_set, NULL, NULL, NULL);      // 阻塞等待任意 fd 可读
 *       // select 返回后，fd_set 中只剩"就绪的" fd
 *       for each 就绪的 fd: 处理它
 *   }
 *
 * 运行方式：
 *   终端1: ./chat_server.exe
 *   终端2: ./chat_client.exe
 *   终端3: ./chat_client.exe  ← 可以开多个客户端互相聊天
 */

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <algorithm>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

// ============================================================
// 服务器配置
// ============================================================
constexpr int  SERVER_PORT   = 8080;
constexpr int  BACKLOG       = 5;
constexpr int  BUFFER_SIZE   = 4096;
constexpr int  MAX_CLIENTS   = 60;   // Windows fd_set 默认最多 64 个
constexpr int  MAX_NAME_LEN  = 32;

// ============================================================
// 客户端会话
// ============================================================
struct Client {
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in addr{};
    char name[MAX_NAME_LEN] = "anonymous";

    // 接收缓冲区 — 用于处理粘包/拆包
    char recv_buf[BUFFER_SIZE] = {};
    int  recv_len = 0;

    // 发送缓冲区（暂未使用，后续扩展）
    char send_buf[BUFFER_SIZE] = {};
    int  send_len = 0;
};

// 全局客户端列表
std::vector<Client> clients;

// ============================================================
// 辅助函数
// ============================================================
void print_error(const char* where) {
    std::cerr << "[ERROR] " << where
              << " failed, code=" << WSAGetLastError() << std::endl;
}

/**
 * 广播消息给所有客户端（可选排除发送者）
 *
 * 这是聊天室的核心操作：一个人说话，所有人听到。
 * 游戏中也是一样 — 一个玩家移动，其他人都要看到。
 *
 * @param message    要发送的消息（包含换行符）
 * @param exclude_fd 排除的 socket（消息发送者自己不需要收到）
 */
void broadcast(const char* message, SOCKET exclude_fd = INVALID_SOCKET) {
    int msg_len = (int)strlen(message);

    for (auto& client : clients) {
        if (client.socket == INVALID_SOCKET) continue;
        if (client.socket == exclude_fd)   continue;  // 不发给自己

        int sent = send(client.socket, message, msg_len, 0);
        if (sent == SOCKET_ERROR) {
            // 发送失败（客户端可能已断开），标记一下，主循环会处理
            std::cerr << "[WARN] Failed to send to " << client.name << std::endl;
        }
    }

    // 同时打印到服务器控制台，方便观察
    std::cout << "[CHAT] " << message;
}

/**
 * 发送消息给单个客户端
 */
void send_to(SOCKET fd, const char* message) {
    int len = (int)strlen(message);
    send(fd, message, len, 0);
}

/**
 * 发送系统消息（如加入/离开通知）
 */
void send_system_msg(const char* text) {
    std::cout << "[SYS] " << text << std::endl;
    broadcast(text);  // 系统消息发给所有人（无排除）
}

// ============================================================
// 主函数
// ============================================================
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Level 2: Chat Room Server (Port " << SERVER_PORT << ")" << std::endl;
    std::cout << "========================================" << std::endl;

    // --------------------------------------------------
    // 步骤 1: 初始化 Winsock
    // --------------------------------------------------
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        print_error("WSAStartup");
        return 1;
    }
    std::cout << "[OK] Winsock initialized." << std::endl;

    // --------------------------------------------------
    // 步骤 2: 创建 + 绑定 + 监听（和 Level 1 一样）
    // --------------------------------------------------
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

    std::cout << "[OK] Chat server listening on port " << SERVER_PORT << std::endl;
    std::cout << "[OK] Max clients: " << MAX_CLIENTS << std::endl;
    std::cout << "----------------------------------------" << std::endl;

    // --------------------------------------------------
    // 步骤 3: select() 主循环 — 核心新知识！
    //
    // 思路：
    //   1. 把 server_socket 和所有 client_socket 放进 fd_set
    //   2. 调用 select() 阻塞等待，直到有任意 socket 可读
    //   3. select() 返回后，遍历找出哪些 socket 就绪
    //   4. 如果是 server_socket → accept 新客户端
    //   5. 如果是 client_socket → recv 数据并广播
    //
    // select() 的参数：
    //   nfds     = 最大 fd 值 + 1（内核用它来确定扫描范围）
    //   readfds  = 关心的"可读"事件
    //   writefds = 关心的"可写"事件（我们不关心，传 NULL）
    //   exceptfds= 关心的"异常"事件（同上）
    //   timeout  = NULL 表示无限等待，有数据才返回
    // --------------------------------------------------

    while (true) {
        // --- 3a. 构建 fd_set：把所有"活着的" socket 放进去 ---
        fd_set read_fds;
        FD_ZERO(&read_fds);                          // 清空集合

        // 监听 socket — 有新连接时变为"可读"
        FD_SET(server_socket, &read_fds);
        SOCKET max_fd = server_socket;

        // 每个客户端 socket — 有数据到达时变为"可读"
        for (auto& client : clients) {
            if (client.socket != INVALID_SOCKET) {
                FD_SET(client.socket, &read_fds);
                if (client.socket > max_fd) {
                    max_fd = client.socket;          // 追踪最大 fd 值
                }
            }
        }

        // --- 3b. 调用 select() — 阻塞等待事件 ---
        // 注意：select() 会修改 read_fds！返回后只剩"就绪的"fd
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);

        if (activity == SOCKET_ERROR) {
            print_error("select");
            break;
        }
        if (activity == 0) {
            continue;  // 超时（我们设了 NULL 所以不会发生）
        }

        // --- 3c. 检查 server_socket：有新客户端连接？ ---
        if (FD_ISSET(server_socket, &read_fds)) {
            sockaddr_in client_addr{};
            int addr_len = sizeof(client_addr);
            SOCKET client_fd = accept(server_socket,
                                       (sockaddr*)&client_addr, &addr_len);

            if (client_fd == INVALID_SOCKET) {
                print_error("accept");
            }
            else if ((int)clients.size() >= MAX_CLIENTS) {
                // 服务器满员
                std::cout << "[WARN] Server full! Rejected "
                          << inet_ntoa(client_addr.sin_addr) << std::endl;
                const char* full_msg = "[SERVER] Chat room is full, try later.\n";
                send(client_fd, full_msg, (int)strlen(full_msg), 0);
                closesocket(client_fd);
            }
            else {
                // 加入客户端
                Client new_client;
                new_client.socket = client_fd;
                new_client.addr   = client_addr;
                snprintf(new_client.name, MAX_NAME_LEN, "user_%d", (int)client_fd);

                clients.push_back(new_client);

                char join_msg[256];
                snprintf(join_msg, sizeof(join_msg),
                         "[SERVER] %s joined the chat. (%d online)\n",
                         new_client.name, (int)clients.size());

                std::cout << "[JOIN] " << new_client.name
                          << " from " << inet_ntoa(client_addr.sin_addr)
                          << ":" << ntohs(client_addr.sin_port) << std::endl;

                // 给新人发欢迎消息
                send_to(client_fd, "========================================\n");
                send_to(client_fd, "  Welcome to the Chat Room!\n");
                send_to(client_fd, "  Type /help for commands\n");
                send_to(client_fd, "========================================\n");

                // 广播新人加入
                broadcast(join_msg);
            }
        }

        // --- 3d. 检查所有客户端 socket：有人发消息？ ---
        for (auto& client : clients) {
            if (client.socket == INVALID_SOCKET) continue;

            if (FD_ISSET(client.socket, &read_fds)) {
                char buffer[BUFFER_SIZE];
                int bytes = recv(client.socket, buffer, BUFFER_SIZE - 1, 0);

                if (bytes > 0) {
                    buffer[bytes] = '\0';

                    // 处理特殊命令
                    if (strncmp(buffer, "/quit", 5) == 0) {
                        // 用户主动退出
                        char leave_msg[256];
                        snprintf(leave_msg, sizeof(leave_msg),
                                 "[SERVER] %s left the chat.\n", client.name);
                        broadcast(leave_msg, client.socket);

                        // 给退出的客户端确认
                        send_to(client.socket, "[SERVER] Goodbye!\n");

                        closesocket(client.socket);
                        client.socket = INVALID_SOCKET;
                    }
                    else if (strncmp(buffer, "/name ", 6) == 0) {
                        // 修改昵称
                        char old_name[MAX_NAME_LEN];
                        strncpy(old_name, client.name, MAX_NAME_LEN);

                        // 提取新昵称（去掉末尾的换行）
                        char new_name[MAX_NAME_LEN];
                        strncpy(new_name, buffer + 6, MAX_NAME_LEN - 1);
                        new_name[MAX_NAME_LEN - 1] = '\0';
                        // 去掉换行符
                        for (int i = 0; new_name[i]; i++) {
                            if (new_name[i] == '\n' || new_name[i] == '\r') {
                                new_name[i] = '\0';
                                break;
                            }
                        }

                        strncpy(client.name, new_name, MAX_NAME_LEN - 1);

                        char name_msg[256];
                        snprintf(name_msg, sizeof(name_msg),
                                 "[SERVER] %s is now known as %s.\n",
                                 old_name, client.name);
                        broadcast(name_msg);
                    }
                    else if (strncmp(buffer, "/list", 5) == 0) {
                        // 列出在线用户
                        send_to(client.socket, "[SERVER] Online users:\n");
                        for (auto& c : clients) {
                            if (c.socket != INVALID_SOCKET) {
                                char user_line[128];
                                snprintf(user_line, sizeof(user_line),
                                         "  - %s\n", c.name);
                                send_to(client.socket, user_line);
                            }
                        }
                    }
                    else if (strncmp(buffer, "/help", 5) == 0) {
                        send_to(client.socket,
                                "[SERVER] Commands:\n"
                                "  /help       - Show this help\n"
                                "  /list       - List online users\n"
                                "  /name <new> - Change nickname\n"
                                "  /quit       - Leave chat room\n");
                    }
                    else {
                        // 普通消息 → 带昵称前缀广播
                        char chat_msg[BUFFER_SIZE + 64];
                        snprintf(chat_msg, sizeof(chat_msg),
                                 "[%s] %s", client.name, buffer);
                        broadcast(chat_msg, client.socket);
                    }
                }
                else if (bytes == 0) {
                    // 客户端断开连接
                    std::cout << "[LEAVE] " << client.name
                              << " disconnected." << std::endl;

                    char leave_msg[256];
                    snprintf(leave_msg, sizeof(leave_msg),
                             "[SERVER] %s disconnected.\n", client.name);
                    broadcast(leave_msg, client.socket);

                    closesocket(client.socket);
                    client.socket = INVALID_SOCKET;
                }
                else {
                    // 接收错误
                    std::cout << "[LEAVE] " << client.name
                              << " connection error." << std::endl;
                    closesocket(client.socket);
                    client.socket = INVALID_SOCKET;
                }
            }
        }

        // --- 3e. 清理已断开的客户端 ---
        // remove-erase idiom: 把 INVALID_SOCKET 的都删掉
        clients.erase(
            std::remove_if(clients.begin(), clients.end(),
                           [](const Client& c) { return c.socket == INVALID_SOCKET; }),
            clients.end()
        );
    }

    // --------------------------------------------------
    // 步骤 4: 清理
    // --------------------------------------------------
    for (auto& client : clients) {
        if (client.socket != INVALID_SOCKET) {
            closesocket(client.socket);
        }
    }
    closesocket(server_socket);
    WSACleanup();

    std::cout << "[OK] Server shutdown." << std::endl;
    return 0;
}
