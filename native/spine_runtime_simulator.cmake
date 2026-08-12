# SpineRuntime integration hook for Cocos Creator's native Simulator.
#
# Usage (Windows):
#   cmake -S <engine>/native/tools/simulator/frameworks/runtime-src \
#         -B <engine>/native/simulator \
#         -G "Visual Studio 18 2026" -A x64 \
#         -DCMAKE_PROJECT_INCLUDE=<this-file>
#
# The stock Simulator project does not scan project extension packages. This
# hook is loaded immediately after its top-level project() call and defers the
# plugin setup until the Simulator and engine targets have been declared.

if(NOT CMAKE_PROJECT_NAME STREQUAL "SimulatorApp")
    return()
endif()

get_property(_spine_runtime_hook_scheduled GLOBAL
    PROPERTY SPINE_RUNTIME_SIMULATOR_HOOK_SCHEDULED)
if(_spine_runtime_hook_scheduled)
    return()
endif()
set_property(GLOBAL PROPERTY SPINE_RUNTIME_SIMULATOR_HOOK_SCHEDULED TRUE)

set(SPINE_RUNTIME_SIMULATOR_ROOT "${CMAKE_CURRENT_LIST_DIR}"
    CACHE INTERNAL "SpineRuntime native root used by the Simulator hook")

function(spine_runtime_attach_to_simulator)
    if(NOT TARGET "${LIB_NAME}")
        message(FATAL_ERROR
            "spine-runtime Simulator hook: target '${LIB_NAME}' was not created")
    endif()

    # VS 18 removed the legacy <hash_map>/<hash_set> headers still referenced
    # by Simulator's bundled protobuf 2.5.  Keep the engine tree untouched and
    # let protobuf use its existing std::map/std::set fallback instead.
    if(MSVC AND MSVC_VERSION GREATER_EQUAL 1950)
        if(NOT TARGET simulator)
            message(FATAL_ERROR
                "spine-runtime Simulator hook: target 'simulator' was not created")
        endif()
        set(_spine_runtime_msvc_compat_dir
            "${SPINE_RUNTIME_SIMULATOR_ROOT}/cmake/msvc-legacy-hash")
        target_include_directories(simulator BEFORE PRIVATE
            "${_spine_runtime_msvc_compat_dir}")
        target_compile_definitions(simulator PRIVATE MISSING_HASH=1)
        message(STATUS
            "Enabled protobuf 2.5 legacy hash compatibility for MSVC ${MSVC_VERSION}")
    endif()

    # Simulator's Game derives directly from CocosApplication, so it does not
    # pass through BaseGame::init(), where normal native projects invoke the
    # generated plugin registry. Generate a build-only Game.cpp with the same
    # call at the equivalent point; the engine source tree remains untouched.
    set(_spine_runtime_simulator_game
        "${CMAKE_SOURCE_DIR}/Classes/Game.cpp")
    if(NOT EXISTS "${_spine_runtime_simulator_game}")
        message(FATAL_ERROR
            "spine-runtime Simulator hook: Game.cpp was not found")
    endif()
    file(READ "${_spine_runtime_simulator_game}"
        _spine_runtime_simulator_game_content)

    set(_spine_runtime_include_anchor
        "#include \"cocos/bindings/jswrapper/SeApi.h\"")
    string(FIND "${_spine_runtime_simulator_game_content}"
        "${_spine_runtime_include_anchor}" _spine_runtime_include_pos)
    if(_spine_runtime_include_pos EQUAL -1)
        message(FATAL_ERROR
            "spine-runtime Simulator hook: Game.cpp include anchor changed")
    endif()
    string(REPLACE "${_spine_runtime_include_anchor}"
        "${_spine_runtime_include_anchor}\n#include \"cocos/plugins/Plugins.h\""
        _spine_runtime_simulator_game_content
        "${_spine_runtime_simulator_game_content}")

    set(_spine_runtime_load_anchor
        "    cc::pipeline::GlobalDSManager::setDescriptorSetLayout();")
    string(FIND "${_spine_runtime_simulator_game_content}"
        "${_spine_runtime_load_anchor}" _spine_runtime_load_pos)
    if(_spine_runtime_load_pos EQUAL -1)
        message(FATAL_ERROR
            "spine-runtime Simulator hook: Game.cpp init anchor changed")
    endif()
    string(REPLACE "${_spine_runtime_load_anchor}"
        "${_spine_runtime_load_anchor}\n    cc_load_all_plugins();"
        _spine_runtime_simulator_game_content
        "${_spine_runtime_simulator_game_content}")

    set(_spine_runtime_autogen_dir
        "${CMAKE_BINARY_DIR}/spine_runtime_simulator_autogen")
    set(_spine_runtime_autogen_game
        "${_spine_runtime_autogen_dir}/Game.cpp")
    file(MAKE_DIRECTORY "${_spine_runtime_autogen_dir}")
    file(WRITE "${_spine_runtime_autogen_game}"
        "${_spine_runtime_simulator_game_content}")

    get_target_property(_spine_runtime_simulator_sources
        "${LIB_NAME}" SOURCES)
    set(_spine_runtime_filtered_sources)
    set(_spine_runtime_game_replaced FALSE)
    foreach(_spine_runtime_source IN LISTS _spine_runtime_simulator_sources)
        get_filename_component(_spine_runtime_source_abs
            "${_spine_runtime_source}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
        if("${_spine_runtime_source_abs}" STREQUAL
           "${_spine_runtime_simulator_game}")
            set(_spine_runtime_game_replaced TRUE)
        else()
            list(APPEND _spine_runtime_filtered_sources
                "${_spine_runtime_source}")
        endif()
    endforeach()
    if(NOT _spine_runtime_game_replaced)
        message(FATAL_ERROR
            "spine-runtime Simulator hook: Game.cpp was not in target '${LIB_NAME}'")
    endif()
    set_property(TARGET "${LIB_NAME}" PROPERTY SOURCES
        "${_spine_runtime_filtered_sources}")
    target_sources("${LIB_NAME}" PRIVATE "${_spine_runtime_autogen_game}")
    source_group("Source Files" FILES "${_spine_runtime_autogen_game}")
    message(STATUS
        "Enabled plugin registry loading in Simulator Game.cpp")

    if(NOT COCOS_X_PATH)
        set(COCOS_X_PATH "${cocosdir}")
    endif()

    include("${SPINE_RUNTIME_SIMULATOR_ROOT}/spine_runtime.cmake")

    # Reuse the same registry generator as a normal Creator native project.
    # It emits cc_load_all_plugins(), links spine_runtime, and compiles the
    # engine with CC_USE_PLUGINS=1 so its empty fallback entry is disabled.
    include("${COCOS_X_PATH}/../templates/cmake/common.cmake")
    set(CC_REGISTERED_PLUGINS spine_runtime)
    cc_plugin_entry()

    if(NOT TARGET plugin_registry)
        message(FATAL_ERROR
            "spine-runtime Simulator hook: plugin_registry was not generated")
    endif()
    # The stock Simulator CMake uses the plain target_link_libraries()
    # signature, so this call must use the same form.
    target_link_libraries("${LIB_NAME}" plugin_registry)

    message(STATUS
        "SpineRuntime plugin linked into Simulator target '${LIB_NAME}'")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
    CALL spine_runtime_attach_to_simulator)
