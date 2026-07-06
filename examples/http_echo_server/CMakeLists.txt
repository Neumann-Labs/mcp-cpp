if(NOT MCP_ENABLE_HTTP)
    return()
endif()

add_executable(http_echo_server main.cpp)
target_link_libraries(http_echo_server PRIVATE mcp::http)
target_compile_options(http_echo_server PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wpedantic
    >
)
