cmake_minimum_required(VERSION 3.22)
project(fix_parser VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

add_library(fix_iface INTERFACE)
target_include_directories(fix_iface INTERFACE include)
target_compile_options(fix_iface INTERFACE -Wall -Wextra)

enable_testing()

add_executable(test_parser tests/test_parser.cpp)
target_link_libraries(test_parser PRIVATE fix_iface)
add_test(NAME Parser COMMAND test_parser)
