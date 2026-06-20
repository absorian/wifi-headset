cmake_minimum_required(VERSION 3.16)

project(wifi-headset-server)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(miniaudio)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../components/audio audio_build)

# liblc3 ships no CMake build, so compile its sources into a static library.
file(GLOB LC3_SRC ${CMAKE_CURRENT_SOURCE_DIR}/liblc3/src/*.c)
add_library(lc3 STATIC ${LC3_SRC})
target_include_directories(lc3
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/liblc3/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/liblc3/src)

add_executable(${PROJECT_NAME} main.c
    discovery.c
    common.h
    audio_dev.c
    control_transport.c)

target_link_libraries(${PROJECT_NAME} PRIVATE miniaudio audio lc3 m)
