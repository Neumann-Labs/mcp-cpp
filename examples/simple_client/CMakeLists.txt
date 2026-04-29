add_executable(simple_client main.cpp)
target_link_libraries(simple_client PRIVATE mcp::mcp)
target_compile_options(simple_client PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wpedantic
    >
)
