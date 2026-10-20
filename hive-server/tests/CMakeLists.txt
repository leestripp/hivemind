cmake_minimum_required(VERSION 3.28)

# hive-server unit tests — one binary per area, assert()-based.
# Tests link hivemind_core (defined in the parent hive-server/) which
# PUBLIC-propagates all include dirs and deps (OmniNet, sqlite3, curl,
# nlohmann_json).

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
hivemind_test(test_thread_groups)
hivemind_test(test_delivery_store)
hivemind_test(test_approval_store)
hivemind_test(test_routine_store)
hivemind_test(test_routine_bridge)
hivemind_test(test_agent_directory)
hivemind_test(test_files)
hivemind_test(test_vault)
hivemind_test(test_connector_store)
hivemind_test(test_worker_store)
hivemind_test(test_auth_store)
hivemind_test(test_secret_store)
hivemind_test(test_roster_routes)
hivemind_test(test_screen_routes)
hivemind_test(test_thread_nudges)
hivemind_test(test_message_stats)
hivemind_test(test_exec_audit)
hivemind_test(test_server_config)
hivemind_test(test_worker_channel)

# Phase 65: loop-storm fan-in policy + mute/breaker
hivemind_test(test_fanin_policy)
hivemind_test(test_chat_throttle)
hivemind_test(test_agent_relay)
hivemind_test(test_turn_attachments)
hivemind_test(test_user_inbox)
hivemind_test(test_secret_create)
hivemind_test(test_secret_delete)
hivemind_test(test_secret_routes)

# Phase 43: hub LlmStore (provider/model registry, dispatch resolution).
hivemind_test(test_llmstore)
hivemind_test(test_share_store)
