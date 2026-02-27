cmake_minimum_required(VERSION 3.14)
project(CpuStressMonitor)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 选项: 启用/禁用 matplotlib 支持 (默认启用，通过Python脚本生成图表)
option(ENABLE_MATPLOTLIB "Enable matplotlib visualization support" ON)

find_package(Threads REQUIRED)

# 检查Python3是否可用（用于生成图表）
find_package(Python3 COMPONENTS Interpreter QUIET)

if(ENABLE_MATPLOTLIB AND Python3_Interpreter_FOUND)
    message(STATUS "Python3 Found: ${Python3_EXECUTABLE}")
    message(STATUS "matplotlib support is ENABLED (will use Python script for plotting)")
else()
    if(ENABLE_MATPLOTLIB)
        message(STATUS "matplotlib support is DISABLED (Python3 not found or option disabled)")
    endif()
endif()

add_executable(cpu_monitor cpu_monitor.cpp)

target_include_directories(cpu_monitor PRIVATE
    ${CMAKE_SOURCE_DIR}
)

if(ENABLE_MATPLOTLIB AND Python3_Interpreter_FOUND)
    target_compile_definitions(cpu_monitor PRIVATE ENABLE_MATPLOTLIB)
endif()

target_link_libraries(cpu_monitor PRIVATE Threads::Threads)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(cpu_monitor PRIVATE -O3)
endif()