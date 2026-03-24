cmake_minimum_required(VERSION 3.14)
project(CpuStressMonitor)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

# 查找 Python3 (matplotlibcpp.h 依赖)
find_package(Python3 COMPONENTS Interpreter Development NumPy QUIET)

if(Python3_FOUND AND Python3_NumPy_FOUND)
    message(STATUS "Python3 Found: ${Python3_EXECUTABLE}")
    message(STATUS "Python3 Include: ${Python3_INCLUDE_DIRS}")
    message(STATUS "NumPy Found: ${Python3_NumPy_INCLUDE_DIRS}")
    set(MATPLOTLIB_CPP_AVAILABLE ON)
else()
    message(WARNING "Python3 or NumPy not found - plotting will be disabled")
    set(MATPLOTLIB_CPP_AVAILABLE OFF)
endif()

add_executable(cpu_monitor cpu_monitor.cpp)

target_include_directories(cpu_monitor PRIVATE
    ${CMAKE_SOURCE_DIR}
)

if(MATPLOTLIB_CPP_AVAILABLE)
    target_include_directories(cpu_monitor PRIVATE
        ${Python3_INCLUDE_DIRS}
        ${Python3_NumPy_INCLUDE_DIRS}
    )
    target_compile_definitions(cpu_monitor PRIVATE ENABLE_MATPLOTLIB_CPP)
    target_link_libraries(cpu_monitor PRIVATE
        Threads::Threads
        ${Python3_LIBRARIES}
    )
else()
    target_link_libraries(cpu_monitor PRIVATE Threads::Threads)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(cpu_monitor PRIVATE -O3)
endif()