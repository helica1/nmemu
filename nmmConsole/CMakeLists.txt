cmake_minimum_required(VERSION 3.16)

project(nmmConsole)

add_executable(nmmConsole main.cpp)

target_link_libraries(nmmConsole PUBLIC nmmLib)
