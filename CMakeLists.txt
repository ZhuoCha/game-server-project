cmake_minimum_required(VERSION 3.14)
project(GameServerProjects LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 添加各子项目
add_subdirectory(level1-echo)
# add_subdirectory(level2-chat)    # 后续阶段逐步启用
# add_subdirectory(level3-tictactoe)
# add_subdirectory(level4-pong)
# add_subdirectory(level5-tanks)
# add_subdirectory(level6-mini-mmo)
