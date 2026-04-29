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
    in_memory_transport.cpp
    session_test.cpp
    server_client_integration_test.cpp
    resources_test.cpp
    prompts_test.cpp
    utilities_test.cpp
    sampling_roots_completion_test.cpp
    pagination_test.cpp
    audit_regression_test.cpp
)

if(MCP_ENABLE_HTTP)
    target_sources(mcp_tests PRIVATE
        http_client_transport_test.cpp
        http_e2e_test.cpp
    )
    target_link_libraries(mcp_tests PRIVATE httplib::httplib)
endif()

# The calculator_server subprocess test only builds in if the example is
# being built, since it needs the path of that binary at runtime.
if(MCP_BUILD_EXAMPLES AND TARGET calculator_server)
    target_sources(mcp_tests PRIVATE calculator_subprocess_test.cpp)
endif()

gtest_discover_tests(mcp_tests
    DISCOVERY_TIMEOUT 60
    PROPERTIES ENVIRONMENT
        "MCP_CALCULATOR_SERVER=$<TARGET_FILE:calculator_server>"
)
