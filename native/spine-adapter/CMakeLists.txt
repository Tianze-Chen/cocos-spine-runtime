# SpineRuntime POC —— 独立构建（不接入引擎主 CMakeLists）
# 编译 spine-cpp 4.3 + SpineRuntime + POC 可执行程序。
cmake_minimum_required(VERSION 3.16)
project(spine_runtime_poc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SPINE_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/spine-cpp")

if(NOT EXISTS "${SPINE_CPP_DIR}/include/spine/spine.h")
    message(FATAL_ERROR
        "vendored spine-cpp 4.3 source is missing at ${SPINE_CPP_DIR} (should be in the repository)")
endif()

# 1. spine-cpp 4.3 运行时（官方原样，含 include/spine + src/spine）
file(GLOB SPINE_CORE_SRC "${SPINE_CPP_DIR}/src/spine/*.cpp")
add_library(spine-cpp STATIC ${SPINE_CORE_SRC})
target_include_directories(spine-cpp PUBLIC "${SPINE_CPP_DIR}/include")

# 2. SpineRuntime 薄封装层
add_library(spine-runtime STATIC SpineRuntime.cpp)
target_include_directories(spine-runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(spine-runtime PUBLIC spine-cpp)

# 3. POC 可执行程序
add_executable(spine-runtime-poc poc/main.cpp)
target_link_libraries(spine-runtime-poc PRIVATE spine-runtime)

# 4. Realtime 压测（cache 决策）
add_executable(spine-runtime-bench poc/bench.cpp)
target_link_libraries(spine-runtime-bench PRIVATE spine-runtime)

if(MSVC)
    # spine 源码在 MSVC 下可能触发 C4996（strcpy 等）；POC 阶段宽松处理
    target_compile_options(spine-cpp PRIVATE /wd4996 /wd4244 /wd4267)
endif()
