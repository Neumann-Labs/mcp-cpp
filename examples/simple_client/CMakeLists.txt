# Create echo client executable
add_executable(echo_client echo_client.cpp)

# Link against our library
target_link_libraries(echo_client
  PRIVATE
    mcp
    pthread
)

# Install the example
install(TARGETS echo_client
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/examples
)