# CMakeLists.txt — Alternative build for PHP Trace Extension
#
# Prerequisites:
#   - PHP 8.0+ development headers (php-config, phpize)
#   - libcurl development package
#   - C++17 compiler (GCC 8+, Clang 7+, MSVC 2019+)
#
# Usage:
#   mkdir build && cd build
#   cmake .. -DPHP_CONFIG=$(which php-config)
#   cmake --build .
#
# Or use phpize + config.m4 for the standard PHP extension build.

cmake_minimum_required(VERSION 3.14)
project(php_trace VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Find php-config
# ---------------------------------------------------------------------------
if(DEFINED PHP_CONFIG)
    set(PHP_CONFIG_EXECUTABLE "${PHP_CONFIG}")
else()
    find_program(PHP_CONFIG_EXECUTABLE NAMES php-config php-config8 php-config8.3)
endif()

if(NOT PHP_CONFIG_EXECUTABLE)
    message(FATAL_ERROR "php-config not found. Specify -DPHP_CONFIG=/path/to/php-config")
endif()

message(STATUS "Using php-config: ${PHP_CONFIG_EXECUTABLE}")

# ---------------------------------------------------------------------------
# Gather PHP build flags
# ---------------------------------------------------------------------------
execute_process(
    COMMAND ${PHP_CONFIG_EXECUTABLE} --include-dir
    OUTPUT_VARIABLE PHP_INCLUDE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND ${PHP_CONFIG_EXECUTABLE} --extension-dir
    OUTPUT_VARIABLE PHP_EXTENSION_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND ${PHP_CONFIG_EXECUTABLE} --ldflags
    OUTPUT_VARIABLE PHP_LDFLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND ${PHP_CONFIG_EXECUTABLE} --libs
    OUTPUT_VARIABLE PHP_LIBS
    OUTPUT_STRIP_TRAILING_WHITESPACE)

message(STATUS "PHP include:  ${PHP_INCLUDE_DIR}")
message(STATUS "PHP ext dir:  ${PHP_EXTENSION_DIR}")

# ---------------------------------------------------------------------------
# Find libcurl
# ---------------------------------------------------------------------------
find_package(CURL REQUIRED)
message(STATUS "CURL found:   ${CURL_INCLUDE_DIRS}")

# ---------------------------------------------------------------------------
# Find pthreads (Unix) or use Windows threads
# ---------------------------------------------------------------------------
if(NOT WIN32)
    find_package(Threads REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
set(SOURCES
    src/span.cpp
    src/span_buffer.cpp
    src/loki_exporter.cpp
    src/php_trace.cpp
)

# ---------------------------------------------------------------------------
# Build shared library
# ---------------------------------------------------------------------------
add_library(php_trace SHARED ${SOURCES})

target_include_directories(php_trace PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PHP_INCLUDE_DIR}
    ${PHP_INCLUDE_DIR}/main
    ${PHP_INCLUDE_DIR}/Zend
    ${PHP_INCLUDE_DIR}/TSRM
    ${PHP_INCLUDE_DIR}/ext
    ${CURL_INCLUDE_DIRS}
)

target_compile_definitions(php_trace PRIVATE
    COMPILE_DL_PHP_TRACE
    ZEND_ENABLE_STATIC_TSRMLS_CACHE=1
    HAVE_CURL=1
)

target_compile_options(php_trace PRIVATE
    -Wall -Wextra -O2
)

target_link_libraries(php_trace PRIVATE
    ${CURL_LIBRARIES}
)

if(NOT WIN32)
    target_link_libraries(php_trace PRIVATE Threads::Threads)
    # PHP extension symbols are resolved at runtime by the PHP binary.
    # Do NOT use -Wl,-z,defs — it would reject unresolved PHP symbols.
endif()

if(APPLE)
    set_target_properties(php_trace PROPERTIES
        SUFFIX ".so"
        PREFIX ""
        INSTALL_RPATH "@loader_path")
elseif(WIN32)
    set_target_properties(php_trace PROPERTIES
        SUFFIX ".dll"
        PREFIX "")
else()
    set_target_properties(php_trace PROPERTIES
        SUFFIX ".so"
        PREFIX "")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS php_trace DESTINATION ${PHP_EXTENSION_DIR})
install(FILES php_trace.ini DESTINATION ${PHP_EXTENSION_DIR}/../conf.d 
        RENAME php_trace.ini OPTIONAL)

# ---------------------------------------------------------------------------
# Print summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "========================================")
message(STATUS "  PHP Trace Extension Build Config")
message(STATUS "========================================")
message(STATUS "  Version:     ${PROJECT_VERSION}")
message(STATUS "  PHP include:  ${PHP_INCLUDE_DIR}")
message(STATUS "  Install to:   ${PHP_EXTENSION_DIR}")
message(STATUS "  CURL:         ${CURL_VERSION}")
message(STATUS "========================================")
