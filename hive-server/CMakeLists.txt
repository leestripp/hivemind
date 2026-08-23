# ============================================================================
# hive-server — HiveMind 2.0 server
# C++23 + OmniNet HTTP/WS API, daedalus agent runtime, graph runner, scheduler.
# Deps (OmniNet, daedalus, nlohmann_json) are fetched by the root superbuild.
# ============================================================================

set(CORE_SOURCES
    src/Graph.cpp
    src/GraphStore.cpp
    src/NodeExecutor.cpp
    src/ExecutorRegistry.cpp
    src/LoopEngine.cpp
    src/Schedule.cpp
    src/GraphRunner.cpp
    src/Settings.cpp
    src/AgentStore.cpp
    src/ThreadStore.cpp
    src/AgentHost.cpp
    src/ContainerManager.cpp
    src/ContainerTools.cpp
    src/ApprovalStore.cpp
    src/AgentTools.cpp
    src/RoutineStore.cpp
    src/AuthStore.cpp
)

set(EXECUTOR_SOURCES
    src/executors/PromptExecutor.cpp
    src/executors/AgentExecutor.cpp
    src/executors/LoopStartExecutor.cpp
    src/executors/AgentLoopStartExecutor.cpp
    src/executors/AgentLoopEndExecutor.cpp
    src/executors/LoopEndExecutor.cpp
    src/executors/InputExecutor.cpp
    src/executors/OutputExecutor.cpp
    src/executors/LogExecutor.cpp
    src/executors/MergeExecutor.cpp
)

set(ROUTE_SOURCES
    include/hivemind/routes/GraphRoutes.cpp
    include/hivemind/routes/RunRoutes.cpp
    include/hivemind/routes/WsRunRoutes.cpp
    include/hivemind/routes/TriggerRoutes.cpp
    include/hivemind/routes/WorkflowRoutes.cpp
    include/hivemind/routes/StaticRoutes.cpp
    include/hivemind/routes/ModelRoutes.cpp
    include/hivemind/routes/ProjectRoutes.cpp
    include/hivemind/routes/AgentRoutes.cpp
    include/hivemind/routes/RosterRoutes.cpp
    include/hivemind/routes/ContainerRoutes.cpp
    include/hivemind/routes/ApprovalRoutes.cpp
    include/hivemind/routes/RoutineRoutes.cpp
    include/hivemind/routes/KeyRoutes.cpp
)

# Everything except main.cpp — shared by the server binary and the test suite.
add_library(hivemind_core STATIC
    ${CORE_SOURCES}
    ${EXECUTOR_SOURCES}
    ${ROUTE_SOURCES}
)

target_include_directories(hivemind_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SQLite3_INCLUDE_DIRS}
)

target_link_libraries(hivemind_core PUBLIC
    OmniNet
    CURL::libcurl
    nlohmann_json::nlohmann_json
    ${SQLite3_LIBRARIES}
    pthread
)

if(HIVEMIND_USE_DAEDALUS)
    target_link_libraries(hivemind_core PUBLIC daedalus)
endif()

add_executable(hive-server src/main.cpp)
target_link_libraries(hive-server PRIVATE hivemind_core)

install(TARGETS hive-server DESTINATION "bin")
