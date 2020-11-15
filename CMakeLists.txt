cmake_minimum_required(VERSION 3.13)
project(WebServer C)

set(CMAKE_C_STANDARD 99)

add_executable(WebServer main.c)
target_compile_options(WebServer PUBLIC -O2)
target_link_options(WebServer PUBLIC -lpthread)