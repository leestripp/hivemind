cmake_minimum_required(VERSION 3.28)

# Tests link hivemind_core (defined in hive-server/) which PUBLIC-propagates
# all include dirs and deps (OmniNet, daedalus, sqlite3, curl, nlohmann_json).

macro(hivemind_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE hivemind_core)
    # Tests rely on assert(); Release builds define NDEBUG which compiles
    # every assert() away and turns the suite into a no-op. Undefine it for
    # test targets so assertions are always live.
    target_compile_options(${name} PRIVATE -UNDEBUG)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

hivemind_test(test_daedalus_smoke)
hivemind_test(test_agent_store)
hivemind_test(test_thread_store)
hivemind_test(test_container_manager)
hivemind_test(test_approval_store)
hivemind_test(test_routine_store)
hivemind_test(test_llm_store)
hivemind_test(test_auth_store)
hivemind_test(test_secret_store)
hivemind_test(test_roster_routes)
hivemind_test(test_screen_routes)
hivemind_test(test_browser_tools)
hivemind_test(test_server_config)
