# Level 4: 实时乒乓游戏
# 核心新概念: 游戏主循环(select超时), 固定时间步, 实时状态同步, 碰撞检测

add_executable(pong_server server.cpp)
target_link_libraries(pong_server ws2_32)

add_executable(pong_client client.cpp)
target_link_libraries(pong_client ws2_32)

add_executable(pong_test test_auto.cpp)
target_link_libraries(pong_test ws2_32)
