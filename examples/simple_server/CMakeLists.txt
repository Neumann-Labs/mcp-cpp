# Create echo server executable
add_executable(echo_server echo_server.cpp)

# Link against our library
target_link_libraries(echo_server
  PRIVATE
    mcp
    pthread
)

# Install the example
install(TARGETS echo_server
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/examples
)