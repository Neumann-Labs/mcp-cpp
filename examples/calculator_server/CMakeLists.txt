add_executable(calculator_server main.cpp)
target_link_libraries(calculator_server PRIVATE mcp::mcp)

target_compile_options(calculator_server PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wpedantic
    >
)
