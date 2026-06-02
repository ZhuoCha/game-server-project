# Level 2: 多人聊天室
# 核心新概念: select() I/O 多路复用、广播、客户端会话管理

add_executable(chat_server server.cpp)
target_link_libraries(chat_server ws2_32)

add_executable(chat_client client.cpp)
target_link_libraries(chat_client ws2_32)

add_executable(chat_test test_client.cpp)
target_link_libraries(chat_test ws2_32)
