cmake_minimum_required(VERSION 3.20)
project(mcp-cpp
    VERSION 0.1.0
    DESCRIPTION "C++20 SDK for the Model Context Protocol"
    HOMEPAGE_URL "https://github.com/Neumann-Labs/mcp-cpp"
    LANGUAGES CXX
)

# ----- Standards & top-level options ---------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Treat us-as-subdirectory differently from us-as-root.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(MCP_IS_TOP_LEVEL ON)
else()
    set(MCP_IS_TOP_LEVEL OFF)
endif()

option(MCP_BUILD_TESTS    "Build the mcp-cpp test suite"     ${MCP_IS_TOP_LEVEL})
option(MCP_BUILD_EXAMPLES "Build the mcp-cpp examples"        ${MCP_IS_TOP_LEVEL})
option(MCP_WARNINGS_AS_ERRORS "Treat warnings as errors"      ${MCP_IS_TOP_LEVEL})
option(MCP_USE_SYSTEM_DEPS "Use find_package for deps"        OFF)
option(MCP_ENABLE_HTTP    "Build the Streamable HTTP transport (depends on cpp-httplib)" ON)
option(MCP_HTTP_NO_TLS    "Build the HTTP transport without OpenSSL (plaintext only; for hosts that must not link a second OpenSSL, e.g. game engines)" OFF)
option(MCP_ENABLE_ASAN    "Build with AddressSanitizer + UBSan" OFF)
option(MCP_ENABLE_TSAN    "Build with ThreadSanitizer"           OFF)
option(MCP_ENABLE_COVERAGE "Build with code-coverage flags"      OFF)

if(MCP_ENABLE_ASAN AND MCP_ENABLE_TSAN)
    message(FATAL_ERROR "MCP_ENABLE_ASAN and MCP_ENABLE_TSAN are mutually exclusive")
endif()

if(MCP_ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()
if(MCP_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()
if(MCP_ENABLE_COVERAGE)
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

# Shared object/static library export bookkeeping:
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ----- Dependencies --------------------------------------------------------

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

if(MCP_USE_SYSTEM_DEPS)
    find_package(nlohmann_json 3.11 REQUIRED)
else()
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install    ON  CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)
endif()

if(MCP_BUILD_TESTS)
    if(MCP_USE_SYSTEM_DEPS)
        find_package(GTest REQUIRED)
    else()
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0
            GIT_SHALLOW    TRUE)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        set(INSTALL_GTEST          OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
endif()

if(MCP_ENABLE_HTTP)
    if(MCP_USE_SYSTEM_DEPS)
        find_package(httplib CONFIG REQUIRED)
    else()
        FetchContent_Declare(httplib
            GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
            GIT_TAG        v0.18.5
            GIT_SHALLOW    TRUE)
        set(HTTPLIB_INSTALL ON  CACHE INTERNAL "")
        # Pin httplib's optional compression deps OFF so the build is
        # deterministic regardless of what's installed on the host. Without
        # this, httplib auto-detects a system brotli and then fails to
        # compile on `#include <brotli/decode.h>` when the headers aren't on
        # the include path. OpenSSL detection is left ON so HTTPS works.
        set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE INTERNAL "")
        set(HTTPLIB_USE_ZLIB_IF_AVAILABLE   OFF CACHE INTERNAL "")
        # MCP_HTTP_NO_TLS drops the OpenSSL dependency entirely: https://
        # URLs then fail at runtime, which plaintext-loopback deployments
        # (and hosts that terminate TLS elsewhere) don't care about — and
        # embedders like Unreal Engine must NOT link a second OpenSSL
        # into a process that already ships its own.
        if(MCP_HTTP_NO_TLS)
            set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE INTERNAL "")
        endif()
        FetchContent_MakeAvailable(httplib)
    endif()
endif()

# ----- Library -------------------------------------------------------------

# Shared warning set, applied to every first-party target (mcp, mcp-http).
function(mcp_target_warnings tgt)
    target_compile_options(${tgt} PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
            -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
            -Wformat=2
        >
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    )
    if(MCP_WARNINGS_AS_ERRORS)
        target_compile_options(${tgt} PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>
            $<$<CXX_COMPILER_ID:MSVC>:/WX>
        )
    endif()
endfunction()

# Core library: stdio transport + protocol/session/server/client. Depends only
# on nlohmann_json — deliberately free of httplib/OpenSSL/brotli so stdio-only
# servers stay lean and start in single-digit ms. The HTTP transport lives in a
# separate `mcp::http` target (see src/CMakeLists.txt).
add_library(mcp)
add_library(mcp::mcp ALIAS mcp)

target_compile_features(mcp PUBLIC cxx_std_20)

target_include_directories(mcp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(mcp PUBLIC nlohmann_json::nlohmann_json)

mcp_target_warnings(mcp)

# Sources are added by include via add_subdirectory of src/, which also defines
# the optional mcp-http target.
add_subdirectory(src)

# ----- Tests / examples ----------------------------------------------------

# Examples first so the calculator_server target exists when tests pick
# it up for the subprocess integration test.
if(MCP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(MCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ----- Install -------------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(_mcp_install_targets mcp)
if(MCP_ENABLE_HTTP)
    list(APPEND _mcp_install_targets mcp-http)
endif()

install(TARGETS ${_mcp_install_targets}
    EXPORT  mcp-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT mcp-targets
    FILE        mcp-targets.cmake
    NAMESPACE   mcp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcp
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/mcp-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mcp-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/mcp-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcp
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/mcp-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/mcp-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcp
)
