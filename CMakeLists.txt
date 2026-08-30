cmake_minimum_required(VERSION 3.22)
project(fix_parser VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

option(FIX_NATIVE     "Build with -march=native (enables AVX2 path)" ON)
option(FIX_SANITIZERS "Build tests with ASan+UBSan" OFF)
option(FIX_FUZZ       "Build libFuzzer harness (requires clang)" OFF)

add_library(fix_iface INTERFACE)
target_include_directories(fix_iface INTERFACE include)
target_compile_options(fix_iface INTERFACE -Wall -Wextra -Wpedantic)
if(FIX_NATIVE)
  target_compile_options(fix_iface INTERFACE -march=native)
endif()
if(FIX_SANITIZERS)
  target_compile_options(fix_iface INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g)
  target_link_options(fix_iface INTERFACE -fsanitize=address,undefined)
endif()

# Regenerate the FIX 4.4 dictionary header from the spec when it changes.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_custom_command(
    OUTPUT  ${CMAKE_SOURCE_DIR}/include/fix/dictionary_fix44.hpp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_dictionary.py
            ${CMAKE_SOURCE_DIR}/spec/FIX44.xml > ${CMAKE_SOURCE_DIR}/include/fix/dictionary_fix44.hpp
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_dictionary.py ${CMAKE_SOURCE_DIR}/spec/FIX44.xml
    COMMENT "Generating FIX 4.4 group dictionary")
  add_custom_target(dictionary DEPENDS ${CMAKE_SOURCE_DIR}/include/fix/dictionary_fix44.hpp)
endif()

enable_testing()
foreach(t parser groups session)
  add_executable(test_${t} tests/test_${t}.cpp)
  target_link_libraries(test_${t} PRIVATE fix_iface)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()

add_executable(bench_parser bench/bench_parser.cpp)
target_link_libraries(bench_parser PRIVATE fix_iface)

add_executable(bench_baseline bench/bench_baseline.cpp)
target_link_libraries(bench_baseline PRIVATE fix_iface)
target_include_directories(bench_baseline PRIVATE vendor/hffix)

if(FIX_FUZZ)
  add_executable(fuzz_parser fuzz/fuzz_parser.cpp)
  target_link_libraries(fuzz_parser PRIVATE fix_iface)
  target_compile_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined -g -O1)
  target_link_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
