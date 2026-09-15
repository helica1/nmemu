cmake_minimum_required(VERSION 3.16)

project(nmmDspTest)

add_executable(nmmDspTest main.cpp diff.cpp)

target_link_libraries(nmmDspTest PUBLIC dsp56kEmu)
