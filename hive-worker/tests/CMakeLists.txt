cmake_minimum_required(VERSION 3.28)

# hive-worker unit tests — one binary per area, assert()-based.
# Tests link hive_worker_core (defined in the parent hive-worker/).

macro(hive_worker_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE hive_worker_core)
    # Keep assert() live even in Release (NDEBUG) builds.
    target_compile_options(${name} PRIVATE -UNDEBUG)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

# Phase B fleet harness: the PURE pre-gate tests (resolved_image mapping,
# resolve_model default resolution) always run under ctest; container
# spawning only happens when HIVE_TEST_API_KEY is set (skip = rc 77).
hive_worker_test(test_fleet)
set_tests_properties(test_fleet PROPERTIES SKIP_RETURN_CODE 77)

# First-run workspace seeding (layout + default index.md, never overwrite).
hive_worker_test(test_workspace_seed)

# Per-agent service-port registry + advertised host address (2026-10-09).
hive_worker_test(test_service_ports)
hive_worker_test(test_migration_ops)
