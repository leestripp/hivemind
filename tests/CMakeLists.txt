cmake_minimum_required(VERSION 3.28)

# Tests link hivemind_core (defined in hive-server/) which PUBLIC-propagates
# all include dirs and deps (OmniNet, hive-brain, sqlite3, curl, nlohmann_json).

macro(hivemind_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE hivemind_core)
    # Tests rely on assert(); Release builds define NDEBUG which compiles
    # every assert() away and turns the suite into a no-op. Undefine it for
    # test targets so assertions are always live.
    target_compile_options(${name} PRIVATE -UNDEBUG)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

hivemind_test(test_agent_store)
hivemind_test(test_thread_store)
hivemind_test(test_approval_store)
hivemind_test(test_routine_store)
hivemind_test(test_files)
hivemind_test(test_vault)
hivemind_test(test_connector_store)
hivemind_test(test_worker_store)
hivemind_test(test_auth_store)
hivemind_test(test_secret_store)
hivemind_test(test_roster_routes)
hivemind_test(test_screen_routes)
hivemind_test(test_thread_nudges)
hivemind_test(test_exec_audit)
hivemind_test(test_server_config)
hivemind_test(test_worker_channel)

# tests/v2/test_fleet.cpp — Phase B standalone harness, now wired in so the
# PURE pre-gate tests (resolved_image mapping, resolve_model default
# resolution) run under ctest like everything else. Container spawning
# only happens when HIVE_TEST_API_KEY is set; without it the harness
# early-returns after the pure block. Links the worker sources directly
# (no hivemind_core — the worker must not drag the hub in).
add_executable(test_fleet v2/test_fleet.cpp
    ${CMAKE_SOURCE_DIR}/hive-worker/src/fleet.cpp
    ${CMAKE_SOURCE_DIR}/hive-worker/src/runner.cpp)
target_link_libraries(test_fleet PRIVATE OmniNet nlohmann_json::nlohmann_json)
target_compile_options(test_fleet PRIVATE -UNDEBUG)   # keep assert() live
add_test(NAME test_fleet COMMAND test_fleet)
set_tests_properties(test_fleet PROPERTIES SKIP_RETURN_CODE 77)
