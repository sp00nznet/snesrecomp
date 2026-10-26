# Standalone ROM-free checks: cmake -S tests -B build/cpu-tests
cmake_minimum_required(VERSION 3.16)
project(snesrecomp_cpu_tests C)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

enable_testing()
add_executable(test_cpu_status test_cpu_status.c ../src/cpu.c)
target_include_directories(test_cpu_status PRIVATE ../include)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_cpu_status PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME cpu_status_width COMMAND test_cpu_status)
