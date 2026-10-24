# hive-worker — Phase 30: remote container executor. Links OmniNet ONLY
# (no hive-brain, no LLM keys, no roster DB — dumb audited executor).

# Core lib (everything except main.cpp) so tests link the exact objects
# the binary ships — same convention as hive-agent/hive_agent_core.
add_library(hive_worker_core STATIC src/runner.cpp src/fleet.cpp src/hostagent.cpp src/migration_ops.cpp)
target_include_directories(hive_worker_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hive_worker_core PUBLIC OmniNet nlohmann_json::nlohmann_json)

add_executable(hive-worker src/main.cpp)
target_link_libraries(hive-worker PRIVATE hive_worker_core)
install(TARGETS hive-worker DESTINATION "bin")

# Unit tests — ./tests/
add_subdirectory(tests)
