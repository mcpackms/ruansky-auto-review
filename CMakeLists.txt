cmake_minimum_required(VERSION 3.14)
project(auto_review VERSION 1.3.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(NCURSES REQUIRED ncursesw)
if(NOT NCURSES_FOUND)
    pkg_check_modules(NCURSES REQUIRED ncurses)
endif()

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(PCRE2 REQUIRED libpcre2-8)

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    pkg_check_modules(NLOHMANN_JSON REQUIRED nlohmann_json)
endif()

find_path(TOML11_INCLUDE_DIR toml.hpp
    HINTS /usr/include /usr/local/include
    PATH_SUFFIXES toml11)
if(NOT TOML11_INCLUDE_DIR)
    find_package(toml11 CONFIG QUIET)
    if(NOT toml11_FOUND)
    endif()
endif()

set(SOURCES
    src/main.cc
    src/tui.cc
    src/web_panel.cc
    src/plugin_manager.cc
)

set(HEADERS
    include/tui.h
    include/web_panel.h
    include/plugin_manager.h
    include/version.h
)

add_executable(auto_review ${SOURCES} ${HEADERS})

target_include_directories(auto_review PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${NCURSES_INCLUDE_DIRS}
    ${PCRE2_INCLUDE_DIRS}
    ${NLOHMANN_JSON_INCLUDE_DIRS}
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
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(auto_review PRIVATE -Wall -Wextra)
endif()

install(TARGETS auto_review RUNTIME DESTINATION bin)