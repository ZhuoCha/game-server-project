# Level 1: Echo 服务器
# 最基础的游戏服务器——接收客户端消息并原样返回

# 服务器可执行文件
add_executable(echo_server server.cpp)
target_link_libraries(echo_server ws2_32)  # Windows Winsock 库

# 客户端测试工具
add_executable(echo_client client.cpp)
target_link_libraries(echo_client ws2_32)
