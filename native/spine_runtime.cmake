# SpineRuntime native plugin — shared source build logic.
#
# Each platform's spine_runtime-config.cmake (android/ windows/ ios/ mac/)
# includes this file. The engine plugin scan sets ${target}_ROOT to the plugin's
# platform directories and runs `find_package(spine_runtime REQUIRED)`, which
# loads the matching <platform>/spine_runtime-config.cmake. This builds the
# plugin as a static library `spine_runtime`; cc_plugin_entry() then links it
# into plugin_registry and cc_load_all_plugins() calls its entry point.
#
# Build note: the plugin uses vendored spine-cpp 4.3 source (native/third_party/spine-cpp),
# which shares the `spine::` namespace with the engine's built-in spine (3.8 is
# on by default).
# For static linking the linker picks the first definition it sees, so an engine
# that also compiles spine-cpp may resolve spine:: symbols against the engine's
# copy and break ABI. Verify symbol isolation (e.g. -fvisibility=hidden /
# linker wrap) if the app links both.

if(TARGET spine_runtime)
    return()
endif()

# COCOS_X_PATH is set by the app template to <engine>/native.
if(NOT COCOS_X_PATH)
    message(FATAL_ERROR "spine_runtime plugin: COCOS_X_PATH (path to engine/native) is not set")
endif()

set(SPINE_RUNTIME_ROOT "${CMAKE_CURRENT_LIST_DIR}")
set(SPINE_CPP_DIR "${SPINE_RUNTIME_ROOT}/third_party/spine-cpp")

if(NOT EXISTS "${SPINE_CPP_DIR}/include/spine/spine.h")
    message(FATAL_ERROR
        "vendored spine-cpp 4.3 source is missing at ${SPINE_CPP_DIR} (should be in the repository)")
endif()

# spine-cpp 4.3 core sources supplied by the vendored spine-cpp snapshot.
file(GLOB SPINE_4_3_CORE_SRC "${SPINE_CPP_DIR}/src/spine/*.cpp")

# Cocos links its built-in Spine runtime into cocos_engine. Compile the 4.3
# implementation under a private namespace so both versions can coexist in the
# final executable without duplicate symbols or accidental ABI cross-linking.
# The public SpineRuntime facade does not expose any spine-cpp types, so this is
# fully internal to these implementation translation units.
set_source_files_properties(
    "${SPINE_RUNTIME_ROOT}/spine-adapter/SpineRuntime.cpp"
    ${SPINE_4_3_CORE_SRC}
    PROPERTIES COMPILE_DEFINITIONS "spine=spine43"
)

add_library(spine_runtime STATIC
    "${SPINE_RUNTIME_ROOT}/spineruntime_plugin.cpp"
    "${SPINE_RUNTIME_ROOT}/jsb_spineruntime_manual.cpp"
    "${SPINE_RUNTIME_ROOT}/spine-adapter/SpineRuntime.cpp"
    ${SPINE_4_3_CORE_SRC}
)

# The engine adds its built-in Spine include directory at directory scope.
# Prepend the plugin's 4.3 headers so <spine/...> can never resolve to the
# engine's 3.8/4.2 copy. The external include sets supply the selected JS
# backend headers (for example V8's libplatform headers on Windows).
target_include_directories(spine_runtime BEFORE PUBLIC
    "${SPINE_CPP_DIR}/include"
    "${SPINE_RUNTIME_ROOT}"
    "${SPINE_RUNTIME_ROOT}/spine-adapter"
    ${CC_EXTERNAL_INCLUDES}
    ${CC_EXTERNAL_PRIVATE_INCLUDES}
    "${COCOS_X_PATH}"        # resolves "cocos/bindings/...", "cocos/plugins/..."
    "${COCOS_X_PATH}/cocos"  # resolves "plugins/bus/..." (Plugins.h) and editor-support
)

target_compile_features(spine_runtime PUBLIC cxx_std_17)

# Consume the selected script-engine target's usage requirements. In
# particular, V8 publishes its version-specific include directory here rather
# than through CC_EXTERNAL_INCLUDES.
if(se_libs_name)
    target_link_libraries(spine_runtime PRIVATE ${se_libs_name})
endif()

# CC_PLUGIN_STATIC is never defined by the engine; static plugins must define it
# so CC_PLUGIN_ENTRY emits an undecorated extern "C" cc_load_plugin_<name>().
target_compile_definitions(spine_runtime PUBLIC
    CC_PLUGIN_STATIC
    ENABLE_JSON_PARSER=1
    ENABLE_BINARY_PARSER=1
)
