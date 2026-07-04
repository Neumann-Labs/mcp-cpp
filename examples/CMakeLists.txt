# stdio examples are POSIX-only (StdioTransport / posix_spawn).
if(NOT WIN32)
    add_subdirectory(calculator_server)
    add_subdirectory(simple_client)
    add_subdirectory(mcp_notes_server)
endif()
add_subdirectory(http_calculator_server)
