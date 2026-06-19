cmake_minimum_required(VERSION 3.16)

project(wifi-headset-server)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(miniaudio)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../components/audio audio_build)

add_executable(${PROJECT_NAME} main.c
    discovery.c
    common.h
    audio_dev.c
    control_transport.c)

target_link_libraries(${PROJECT_NAME} PRIVATE miniaudio audio)
