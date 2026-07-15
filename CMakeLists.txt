cmake_minimum_required(VERSION 3.14)
project(auto_review VERSION 1.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ==================== 查找依赖 ====================
find_package(PkgConfig REQUIRED)

# QuickJS (JS 插件引擎)
find_package(qjs REQUIRED)

# ncurses (with wide char support for Chinese)
pkg_check_modules(NCURSES REQUIRED ncursesw)
if(NOT NCURSES_FOUND)
    pkg_check_modules(NCURSES REQUIRED ncurses)
endif()

# cURL
find_package(CURL REQUIRED)

# OpenSSL (libcrypto)
find_package(OpenSSL REQUIRED)

# PCRE2
pkg_check_modules(PCRE2 REQUIRED libpcre2-8)

# nlohmann_json
find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    pkg_check_modules(NLOHMANN_JSON REQUIRED nlohmann_json)
endif()

# toml11 (header-only)
find_path(TOML11_INCLUDE_DIR toml.hpp
    HINTS /usr/include /usr/local/include /data/data/com.termux/files/usr/include
    PATH_SUFFIXES toml11)
if(NOT TOML11_INCLUDE_DIR)
    find_package(toml11 CONFIG QUIET)
    if(NOT toml11_FOUND)
        message(FATAL_ERROR "toml11 not found. Install with: pkg install toml11")
    endif()
endif()

# ==================== 源文件 ====================
set(SOURCES
    main.cc
    tui.cc
    web_panel.cc
    plugins/plugin_manager.cc
    plugins/js_bridge.cc
    plugins/js_sandbox.cc
    plugins/quickjs_helpers.cc
)

set(HEADERS
    tui.h
    web_panel.h
    plugins/plugin_manager.h
    plugins/js_bridge.h
    plugins/js_sandbox.h
    plugins/quickjs_helpers.h
)

# ==================== 可执行文件 ====================
add_executable(auto_review ${SOURCES} ${HEADERS})

target_include_directories(auto_review PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${NCURSES_INCLUDE_DIRS}
    ${PCRE2_INCLUDE_DIRS}
    ${NLOHMANN_JSON_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/plugins
)

if(TOML11_INCLUDE_DIR)
    target_include_directories(auto_review PRIVATE ${TOML11_INCLUDE_DIR})
endif()

target_link_libraries(auto_review PRIVATE
    ${NCURSES_LIBRARIES}
    ${CURL_LIBRARIES}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${PCRE2_LIBRARIES}
    pthread
    qjs
)

# ==================== 编译选项 ====================
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(auto_review PRIVATE -Wall -Wextra)
endif()

# ==================== 安装 ====================
install(TARGETS auto_review RUNTIME DESTINATION bin)