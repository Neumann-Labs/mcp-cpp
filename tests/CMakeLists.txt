include(GoogleTest)

add_executable(mcp_tests)
target_link_libraries(mcp_tests PRIVATE mcp::mcp GTest::gtest_main GTest::gmock)

target_compile_options(mcp_tests PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wpedantic
    >
)

target_sources(mcp_tests PRIVATE
    log_test.cpp
    error_test.cpp
    protocol_test.cpp
    stdio_transport_test.cpp
)

gtest_discover_tests(mcp_tests DISCOVERY_TIMEOUT 60)
