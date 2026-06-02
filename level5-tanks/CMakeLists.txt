# Level 5: 坦克大战
# 核心新概念: 多实体系统, 射击/碰撞/死亡/重生, 状态快照

add_executable(tank_server server.cpp)
target_link_libraries(tank_server ws2_32)

add_executable(tank_client client.cpp)
target_link_libraries(tank_client ws2_32)

add_executable(tank_test test_auto.cpp)
target_link_libraries(tank_test ws2_32)
