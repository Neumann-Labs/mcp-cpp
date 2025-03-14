# Tests CMakeLists.txt for MCP-CPP

# Create test executable
add_executable(mcp_tests
  # Unit tests
  unit/error_tests.cpp
  unit/json_utils_tests.cpp
  unit/session_tests.cpp
  unit/additional_session_tests.cpp
  unit/transport_tests.cpp
  unit/client_server_impl_tests.cpp
  
  # Integration tests
  integration/client_server_tests.cpp
)

# Link dependencies
target_link_libraries(mcp_tests
  PRIVATE
    mcp
    GTest::gtest_main
    GTest::gmock_main
)

# Add tests to CTest
include(GoogleTest)
gtest_discover_tests(mcp_tests)

# Enable code coverage if requested
if(ENABLE_COVERAGE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mcp_tests PRIVATE --coverage)
    target_link_libraries(mcp_tests PRIVATE --coverage)
  endif()
endif()