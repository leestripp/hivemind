# ============================================================================
# hive-server — HiveMind 2.0 server
# C++23 + OmniNet HTTP/WS API hub: roster, threads, workers, scheduler.
# Deps (OmniNet, nlohmann_json) are fetched by the root superbuild.
# ============================================================================

set(CORE_SOURCES
    src/Schedule.cpp
    src/SystemMonitor.cpp
    src/Files.cpp
    src/Vault.cpp
    src/ConnectorStore.cpp
    src/WorkerStore.cpp
    src/WorkerChannel.cpp
    src/WorkerRouter.cpp
    src/Settings.cpp
    src/AgentStore.cpp
    src/ThreadStore.cpp
    src/ContainerManager.cpp
    src/ExecAuditStore.cpp
    src/ApprovalStore.cpp
    src/AgentTools.cpp
    src/ChatService.cpp
    src/HubRouter.cpp
    src/RoutineStore.cpp
    src/AuthStore.cpp
    src/SecretStore.cpp
    src/ServerConfig.cpp
)

set(EXECUTOR_SOURCES
)

set(ROUTE_SOURCES
    include/hivemind/routes/StaticRoutes.cpp
    include/hivemind/routes/SystemRoutes.cpp
    include/hivemind/routes/RosterRoutes.cpp
    include/hivemind/routes/FileRoutes.cpp
    include/hivemind/routes/ConnectorRoutes.cpp
    include/hivemind/routes/WorkerRoutes.cpp
    include/hivemind/routes/SkillRoutes.cpp
    include/hivemind/routes/SecretRoutes.cpp
    include/hivemind/routes/ContainerRoutes.cpp
    include/hivemind/routes/ApprovalRoutes.cpp
    include/hivemind/routes/RoutineRoutes.cpp
    include/hivemind/routes/KeyRoutes.cpp
    include/hivemind/routes/ConfigRoutes.cpp
    include/hivemind/routes/ScreenRoutes.cpp
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
    nlohmann_json::nlohmann_json
    ${SQLite3_LIBRARIES}
    pthread
)

if(HIVEMIND_USE_DAEDALUS)
    # Phase C: hive-brain linkage REMOVED - brains run on workers
    # (hive-agent). The server is a pure hub: OmniNet + SQLite only.
endif()

add_executable(hive-server src/main.cpp)
target_link_libraries(hive-server PRIVATE hivemind_core)

install(TARGETS hive-server DESTINATION "bin")
