# hive-agent - Phase A of the v2 refactor: daedalus brain INSIDE the
# container. Thin host around daedalus::Agent speaking stdio NDJSON with
# the hive-worker (task/delta/message/approval events). No OmniNet, no
# HTTP - stdin/stdout only.
find_package(SQLite3 REQUIRED)

add_executable(hive-agent src/main.cpp)
target_compile_definitions(hive-agent PRIVATE GIT_VERSION="${GIT_VERSION}")
target_link_libraries(hive-agent PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    ${SQLite3_LIBRARIES})
target_include_directories(hive-agent PRIVATE ${SQLite3_INCLUDE_DIRS})
install(TARGETS hive-agent DESTINATION "bin")
