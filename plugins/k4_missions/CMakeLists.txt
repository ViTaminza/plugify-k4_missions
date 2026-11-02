# k4_missions — Plugify C++ port of K4ryuu/K4-Missions (SwiftlyS2, C#) on native s2sdk.
#
#   k4_missions_core       engine-free core (config/mission JSON, filters, progress accounting,
#                          reset periods, VIP amount, reward placeholders); used by
#                          tests/k4_missions_tests.cpp
#   plugify-k4_missions    the plugin shared library (entry "bin/k4_missions")
#
# Standalone on purpose (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no helper library.
#   * s2sdk binding globals       $<TARGET_OBJECTS:s2sdk_bindings>
#   * permissions (optional dep)  $<TARGET_OBJECTS:permissions_bindings>
#   * hudmenu (optional dep)      only for the player's "!menu" type choice (nullable bindings)
#   * vip consumer bindings       the checked-in generated vip.cpp (plugins/vip has no
#                                 CMakeLists.txt in an exported standalone repository)
#   * plgdb                       the only extra library: the async MySQL/SQLite store
#
# The map gate, the handle/slot guards, the command/hook/menu lifecycles and the accumulator
# clocks live in src/plugin.cpp; the files it reads are parsed by the plugin-local src/kv1.cpp.

set(K4_MISSIONS_VERSION "1.1.0")

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/k4_missions.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/k4_missions.pplugin"
)

# --- optional consumer bindings ---------------------------------------------------------------
# plugins/vip sorts AFTER k4_missions, so `vip_bindings` does not exist yet while this directory
# is configured. The fallback target (a distinct name, exactly like lr_vip_multi does) compiles
# the checked-in generated vip binding slots into this plugin.
if(TARGET vip_bindings)
    set(K4_MISSIONS_VIP_BINDINGS "$<TARGET_OBJECTS:vip_bindings>")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include/vip/vip.cpp")
    add_library(k4_missions_vip_bindings OBJECT
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include/vip/vip.cpp")
    target_include_directories(k4_missions_vip_bindings SYSTEM PUBLIC
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include"
            "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(k4_missions_vip_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(K4_MISSIONS_VIP_BINDINGS "$<TARGET_OBJECTS:k4_missions_vip_bindings>")
else()
    message(FATAL_ERROR "k4_missions needs the generated vip consumer bindings "
            "(plugins/vip/include/external/plugify/include/vip/vip.cpp)")
endif()

# hudmenu is an optional dependency (the player's own "!menu" choice). Its binding globals have to
# be compiled into the plugin too (they stay null when the plugin is absent); the monorepo target is
# reused, and a standalone export builds the checked-in generated file.
if(TARGET hudmenu_bindings)
    set(K4_MISSIONS_HUDMENU_BINDINGS "$<TARGET_OBJECTS:hudmenu_bindings>")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/sdk/include/hudmenu/hudmenu.cpp")
    add_library(k4_missions_hudmenu_bindings OBJECT "${CMAKE_SOURCE_DIR}/sdk/include/hudmenu/hudmenu.cpp")
    target_include_directories(k4_missions_hudmenu_bindings SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(k4_missions_hudmenu_bindings PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(K4_MISSIONS_HUDMENU_BINDINGS "$<TARGET_OBJECTS:k4_missions_hudmenu_bindings>")
else()
    message(FATAL_ERROR "k4_missions needs the hudmenu consumer bindings (sdk/include/hudmenu/hudmenu.cpp)")
endif()

# --- core ------------------------------------------------------------------------------------
add_library(k4_missions_core STATIC src/k4_core.cpp)
target_include_directories(k4_missions_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# nlohmann/json is bundled under sdk/include.
target_include_directories(k4_missions_core SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_options(k4_missions_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(k4_missions_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin ----------------------------------------------------------------------------------
add_library(plugify-k4_missions SHARED
        src/plugin.cpp
        src/k4_store.cpp
        src/k4_webhook.cpp
        src/kv1.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:permissions_bindings>
        ${K4_MISSIONS_HUDMENU_BINDINGS}
        ${K4_MISSIONS_VIP_BINDINGS}
)
target_include_directories(plugify-k4_missions PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/plg/permissions/hudmenu/vip headers must not fail
# -Werror on our sources.
target_include_directories(plugify-k4_missions SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/sdk/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include"
        "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include")
target_link_libraries(plugify-k4_missions PRIVATE
        k4_missions_core
        plgdb
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-k4_missions PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-k4_missions PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="k4_missions"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-k4_missions PROPERTIES OUTPUT_NAME k4_missions)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-k4_missions PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

# --- tests ------------------------------------------------------------------------------------
# The top-level CMakeLists declares the PLUGIFY_PORT_TESTS option *after* the plugin directories,
# so on a first configure it is not defined here yet: default it to the same ON. An explicit
# -DPLUGIFY_PORT_TESTS=OFF (the exported standalone build) is already in the cache and still wins.
if(NOT DEFINED PLUGIFY_PORT_TESTS)
    set(PLUGIFY_PORT_TESTS ON)
endif()
if(PLUGIFY_PORT_TESTS)
    # k4_missions_tests: engine-free. The mission/config JSON readers, the KV1 reader and the
    # phrase/language table, filter matching, progress accounting, reset period arithmetic, VIP
    # amount selection, reward placeholder expansion and the store against real SQLite (MySQL when
    # ./test.sh exports MYSQL_*). Run by ./test.sh (build/tests).
    add_executable(k4_missions_tests
            tests/k4_missions_tests.cpp
            src/k4_store.cpp
            src/kv1.cpp
    )
    target_include_directories(k4_missions_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(k4_missions_tests PRIVATE k4_missions_core plgdb -static-libstdc++ -static-libgcc)
    target_compile_options(k4_missions_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(k4_missions_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            K4_MISSIONS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
    set_target_properties(k4_missions_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

    # k4_missions_plugin_tests: no engine. The plugin TU is compiled into the test (its handlers
    # live in an anonymous namespace), every __s2sdk_* / __permissions_* / __hudmenu_* / __vip_*
    # binding slot is a recording fake, and the database half runs against a real throwaway SQLite
    # file through plgdb. It covers the map gate (B1/B2), the handle/slot guards (C1/C2), the
    # accumulator clocks (C9), the command/hook/menu lifecycles (A7/C10/C11), the event player
    # fields, deferred actions/DB callbacks keyed by SteamID64 + controller handle, and the unload
    # teardown. Run by ./test.sh (build/tests).
    add_executable(k4_missions_plugin_tests
            tests/k4_missions_plugin_tests.cpp
            src/k4_store.cpp
            src/k4_webhook.cpp
            src/kv1.cpp
            $<TARGET_OBJECTS:s2sdk_bindings>
            $<TARGET_OBJECTS:permissions_bindings>
            ${K4_MISSIONS_HUDMENU_BINDINGS}
            ${K4_MISSIONS_VIP_BINDINGS}
    )
    target_include_directories(k4_missions_plugin_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(k4_missions_plugin_tests SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/sdk/include"
            "${CMAKE_SOURCE_DIR}/plugins/vip/include"
            "${CMAKE_SOURCE_DIR}/plugins/vip/include/external/plugify/include")
    target_link_libraries(k4_missions_plugin_tests PRIVATE
            k4_missions_core
            plgdb
            -static-libstdc++ -static-libgcc
    )
    target_compile_options(k4_missions_plugin_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(k4_missions_plugin_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            PLUGIFY_MODULE="k4_missions"
            PLUGIFY_IS_DEBUG=0
            PLUGIFY_IS_RELEASE=1
            K4_MISSIONS_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    set_target_properties(k4_missions_plugin_tests PROPERTIES
            OUTPUT_NAME k4_missions_plugin_tests
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

    # k4_missions_package_tests: runs the real package.sh from an isolated copy of the plugin
    # directory, with temp BUILD_DIR/OUT_DIR and fake artifacts. It pins the credential-less
    # databases.cfg.example (never databases.cfg), the no-clobber deploy rule and the OUT_DIR guard
    # (realpath, server trees) before `rm -rf`.
    add_executable(k4_missions_package_tests tests/k4_missions_package_tests.cpp)
    target_link_libraries(k4_missions_package_tests PRIVATE -static-libstdc++ -static-libgcc)
    target_compile_options(k4_missions_package_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(k4_missions_package_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            K4_MISSIONS_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    set_target_properties(k4_missions_package_tests PROPERTIES
            OUTPUT_NAME k4_missions_package_tests
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
