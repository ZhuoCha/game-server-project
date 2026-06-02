# Level 6: Mini MMO
# 核心新概念: AOI兴趣区域, 空间哈希网格, 视野管理, 怪物AI, 持久化存盘

add_executable(mmo_server server.cpp)
target_link_libraries(mmo_server ws2_32)

add_executable(mmo_client client.cpp)
target_link_libraries(mmo_client ws2_32)

add_executable(mmo_test test_auto.cpp)
target_link_libraries(mmo_test ws2_32)
