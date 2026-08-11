cmake_minimum_required(VERSION 3.28)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLite3 REQUIRED sqlite3)
pkg_check_modules(PNG REQUIRED libpng)
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)

# OmniNet and daedalus are built via FetchContent in the parent
# CMakeLists.txt and we link against the static archives directly.

macro(hivemind_test name)
    add_executable(${name} ${name}.cpp
        ${CMAKE_SOURCE_DIR}/src/Graph.cpp
        ${CMAKE_SOURCE_DIR}/src/GraphStore.cpp
        ${CMAKE_SOURCE_DIR}/src/LoopEngine.cpp
        ${CMAKE_SOURCE_DIR}/src/GraphRunner.cpp
        ${CMAKE_SOURCE_DIR}/src/Settings.cpp
        ${CMAKE_SOURCE_DIR}/src/ExecutorRegistry.cpp
        ${CMAKE_SOURCE_DIR}/src/NodeExecutor.cpp
        ${CMAKE_SOURCE_DIR}/src/executors/PromptExecutor.cpp
        ${CMAKE_SOURCE_DIR}/src/executors/AgentExecutor.cpp
        ${CMAKE_SOURCE_DIR}/src/executors/LoopStartExecutor.cpp
        ${CMAKE_SOURCE_DIR}/src/executors/AgentLoopStartExecutor.cpp
        ${CMAKE_SOURCE_DIR}/src/executors/AgentLoopEndExecutor.cpp
        ${CMAKE_SOURCE_DIR}/src/executors/LoopEndExecutor.cpp
        ${CMAKE_SOURCE_DIR}/src/executors/InputExecutor.cpp
        ${CMAKE_SOURCE_DIR}/src/executors/OutputExecutor.cpp
        ${CMAKE_SOURCE_DIR}/src/executors/LogExecutor.cpp
        ${CMAKE_SOURCE_DIR}/src/executors/MergeExecutor.cpp
    )
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${SQLite3_INCLUDE_DIRS}
        ${PNG_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/_deps/nlohmann_json-src/include
        ${CMAKE_SOURCE_DIR}/build/_deps/daedalus-src/include
        ${CMAKE_SOURCE_DIR}/build/_deps/omninet-src/include
        ${CMAKE_SOURCE_DIR}/build/_deps/omninet-src/src
    )
    target_link_libraries(${name} PRIVATE
        -Wl,--start-group
        nlohmann_json::nlohmann_json
        ${SQLite3_LIBRARIES}
        ${PNG_LIBRARIES}
        CURL::libcurl
        OpenSSL::Crypto
        OpenSSL::SSL
        pthread
        ${CMAKE_SOURCE_DIR}/build/_deps/daedalus-build/libdaedalus.a
        ${CMAKE_SOURCE_DIR}/build/_deps/omninet-build/libOmniNet.a
        -Wl,--end-group
    )
    # Tests rely on assert(); Release builds define NDEBUG which compiles
    # every assert() away and turns the suite into a no-op. Undefine it for
    # test targets so assertions are always live.
    target_compile_options(${name} PRIVATE -UNDEBUG)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

hivemind_test(test_loop_engine)
hivemind_test(test_node_ports)
hivemind_test(test_daedalus_smoke)
hivemind_test(test_graph_runner)
