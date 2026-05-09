cmake_minimum_required(VERSION 3.16)

project(wifi-headset-server)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(${PROJECT_NAME} main.c discovery.c control_transport.c)
