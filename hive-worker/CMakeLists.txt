# hive-worker — Phase 30: remote container executor. Links OmniNet ONLY
# (no daedalus, no LLM keys, no roster DB — dumb audited executor).
add_executable(hive-worker src/main.cpp src/runner.cpp src/mcp_bridge.cpp)
target_link_libraries(hive-worker PRIVATE OmniNet nlohmann_json::nlohmann_json)
install(TARGETS hive-worker DESTINATION "bin")
