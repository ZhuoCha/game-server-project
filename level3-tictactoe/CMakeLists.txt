# Level 3: 井字棋对战
# 核心新概念: 游戏状态机、匹配系统、回合制协议、胜负判定

add_executable(tictactoe_server server.cpp)
target_link_libraries(tictactoe_server ws2_32)

add_executable(tictactoe_client client.cpp)
target_link_libraries(tictactoe_client ws2_32)

add_executable(tictactoe_test test_auto.cpp)
target_link_libraries(tictactoe_test ws2_32)
