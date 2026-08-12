# SpineRuntime WASM 构建
# 编译 spine-cpp 4.3 + SpineRuntime.cpp 为 spine-runtime.wasm（web 端运行时地基）。
# 用法：source /d/emsdk/emsdk_env.sh && emcmake cmake -S native/wasm -B native/wasm/build && cmake --build native/wasm/build
cmake_minimum_required(VERSION 3.16)
project(spine_runtime_wasm CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SPINE_ADAPTER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../spine-adapter")
set(SPINE_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/spine-cpp")

if(NOT EXISTS "${SPINE_CPP_DIR}/include/spine/spine.h")
    message(FATAL_ERROR
        "vendored spine-cpp 4.3 source is missing at ${SPINE_CPP_DIR} (should be in the repository)")
endif()

# 生产默认只编 web；测试时 -DEMSCRIPTEN_ENVIRONMENT=web,node 覆盖
set(EMSCRIPTEN_ENVIRONMENT "web" CACHE STRING "emscripten ENVIRONMENT setting")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions -fno-rtti -Wno-inconsistent-missing-override -DEMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES=0 -DENABLE_JSON_PARSER=1 -DENABLE_BINARY_PARSER=1")

file(GLOB SPINE_CORE_SRC "${SPINE_CPP_DIR}/src/spine/*.cpp")

# 注意：SpineRuntime.cpp 内有 getDefaultExtension 的独立构建守卫
# （SPINERUNTIME_EXTERNAL_SPINE_EXTENSION 未定义时提供默认实现）。
add_executable(spine-runtime
    ${SPINE_CORE_SRC}
    ${SPINE_ADAPTER_DIR}/SpineRuntime.cpp
    spine-runtime-bindings.cpp
)
target_include_directories(spine-runtime PRIVATE
    "${SPINE_CPP_DIR}/include"
    "${SPINE_ADAPTER_DIR}"
)

# WeChat's WXWebAssembly validator still expects the MVP call_indirect
# encoding. Recent Emscripten/LLVM versions enable reference types by default
# and can emit an overlong table index (0 encoded as 80 80 80 80 00), which
# WeChat rejects with "expected table index 0, found 0". Keep the binary on the
# legacy-compatible path and lower bulk-memory operations at link time.
target_compile_options(spine-runtime PRIVATE
    "-mno-reference-types"
    "-mno-bulk-memory"
)
target_link_options(spine-runtime PRIVATE
    "SHELL:-mno-reference-types"
    "SHELL:-mno-bulk-memory"
    # Mini-game sandboxes disallow eval/new Function. Generate Embind invokers
    # ahead of time so bindings remain fast without dynamic code execution.
    "SHELL:-s DYNAMIC_EXECUTION=0"
    "SHELL:-s EMBIND_AOT=1"
    "SHELL:-s WASM=1"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s EXPORT_ES6=1"
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_NAME=SpineRuntime"
    "SHELL:-s FILESYSTEM=0"
    "SHELL:-s NO_EXIT_RUNTIME=1"
    "SHELL:-s ENVIRONMENT=${EMSCRIPTEN_ENVIRONMENT}"
    "SHELL:-s ERROR_ON_UNDEFINED_SYMBOLS=0"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU16','HEAP16','HEAPU32','HEAP32','HEAPF32']"
    "SHELL:--bind"
)
