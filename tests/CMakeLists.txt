include(GoogleTest)

# Find all test files
file(GLOB_RECURSE TEST_SOURCES
  "unit/*.cpp"
  "integration/*.cpp"
)

# Create the test executable
add_executable(mcp_tests ${TEST_SOURCES})

# Link against gtest and our library
target_link_libraries(mcp_tests
  PRIVATE
    mcp
    gtest
    gtest_main
    pthread
)

# Add include directories
target_include_directories(mcp_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Register tests with CTest
gtest_discover_tests(mcp_tests)

# Enable code coverage if requested
if(ENABLE_COVERAGE)
  target_compile_options(mcp_tests PRIVATE --coverage)
  target_link_options(mcp_tests PRIVATE --coverage)
endif()