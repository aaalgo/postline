cmake_minimum_required(VERSION 3.20)

project(postline VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "postline supports Linux only")
endif()

include(FetchContent)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fPIC>)
add_compile_options($<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Debug>>:-g>)
add_compile_options($<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Debug>>:-O0>)

if(NOT CMAKE_CONFIGURATION_TYPES)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug
        Release
        RelWithDebInfo
        MinSizeRel
    )
endif()

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Install path prefix" FORCE)
endif()

set(POSTLINE_INSTALL_BIN_DIR "${CMAKE_BINARY_DIR}/install/bin")

option(BUILD_TESTS "Build postline tests" OFF)

find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(NOT GIT_COMMIT_HASH)
    set(GIT_COMMIT_HASH "unknown")
endif()

string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S")

set(POSTLINE_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
if(NOT POSTLINE_BUILD_TYPE)
    set(POSTLINE_BUILD_TYPE "unknown")
endif()

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.3
)

FetchContent_Declare(
    CRCpp
    GIT_REPOSITORY https://github.com/d-bahr/CRCpp.git
    GIT_TAG release-1.2.1.0
)

FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.6.1
)

FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v2.13.6
)

FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v6.1.9
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.28.0
    GIT_SHALLOW TRUE
)

set(BUILD_DOC OFF CACHE BOOL "Disable CRCpp documentation" FORCE)
set(BUILD_TEST OFF CACHE BOOL "Disable CRCpp tests" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "Disable FTXUI documentation" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "Disable FTXUI examples" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "Disable FTXUI tests" FORCE)
set(FTXUI_BUILD_TESTS_FUZZER OFF CACHE BOOL "Disable FTXUI fuzz tests" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "Disable FTXUI install rules" FORCE)

FetchContent_MakeAvailable(nlohmann_json spdlog CRCpp CLI11 pybind11 ftxui cpp_httplib)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/postline/build_info.hpp.in
    ${CMAKE_BINARY_DIR}/generated/build_info.hpp
    @ONLY
)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/arena.json
    ${CMAKE_BINARY_DIR}/install/arena.json
    COPYONLY
)

add_library(postline
    src/common.cpp
    src/parser.cpp
    src/environ.cpp
    src/runtime.cpp
    src/program.cpp
    src/ui.cpp
    src/tui/global_tab.cpp
    src/tui/message_editor.cpp
    src/tui/message_reader.cpp
    src/observer.cpp
    src/tui/render.cpp
    src/tui/split_pane.cpp
    src/tui/stat_tab.cpp
    src/tui/thread_nav.cpp
    src/tui/thread_tab.cpp
    src/tui/tui.cpp
    src/cli.cpp
    src/web.cpp
)
add_library(postline::postline ALIAS postline)

target_include_directories(postline
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/generated
        ${CRCpp_SOURCE_DIR}/inc
)

target_link_libraries(postline
    PUBLIC
        CLI11::CLI11
        httplib::httplib
        nlohmann_json::nlohmann_json
        spdlog::spdlog_header_only
    PRIVATE
        ftxui::screen
        ftxui::dom
        ftxui::component
)

add_library(postline_install_bin_deps INTERFACE)
target_include_directories(postline_install_bin_deps INTERFACE
    ${CMAKE_BINARY_DIR}/generated
)
target_link_libraries(postline_install_bin_deps INTERFACE
    postline::postline
    CLI11::CLI11
    spdlog::spdlog
)

add_library(postline_tui_deps INTERFACE)
add_library(postline::tui_deps ALIAS postline_tui_deps)
target_link_libraries(postline_tui_deps INTERFACE
    ftxui::screen
    ftxui::dom
    ftxui::component
)

add_executable(postline_bin
    src/main.cpp src/banners.cpp
)
target_link_libraries(postline_bin PRIVATE postline_install_bin_deps)
target_link_libraries(postline_bin PRIVATE postline::tui_deps)
set_target_properties(postline_bin PROPERTIES
    OUTPUT_NAME pl
    RUNTIME_OUTPUT_DIRECTORY "${POSTLINE_INSTALL_BIN_DIR}"
)

add_executable(echo_actor
    src/adapters/echo.cpp
)
target_link_libraries(echo_actor PRIVATE postline::postline CLI11::CLI11)
set_target_properties(echo_actor PROPERTIES
    OUTPUT_NAME echo
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/install/bin/adapters"
)

add_executable(chaos_actor
    src/adapters/chaos.cpp
)
target_link_libraries(chaos_actor PRIVATE postline::postline CLI11::CLI11)
set_target_properties(chaos_actor PROPERTIES
    OUTPUT_NAME chaos
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/install/bin/adapters"
)

add_executable(shell_actor
    src/adapters/shell.cpp
)
target_link_libraries(shell_actor PRIVATE postline::postline CLI11::CLI11)
set_target_properties(shell_actor PROPERTIES
    OUTPUT_NAME shell
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/install/bin/adapters"
)

add_executable(benchmark_actor
    src/adapters/benchmark.cpp
)
target_link_libraries(benchmark_actor PRIVATE postline::postline CLI11::CLI11)
set_target_properties(benchmark_actor PROPERTIES
    OUTPUT_NAME benchmark
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/install/bin/adapters"
)

#add_executable(ftxcli_actor
#    src/adapters/ftxcli.cpp
#)
#target_link_libraries(ftxcli_actor PRIVATE postline::postline CLI11::CLI11)
#target_link_libraries(ftxcli_actor PRIVATE postline::tui_deps)
#set_target_properties(ftxcli_actor PROPERTIES
#    OUTPUT_NAME ftxcli
#    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/install/bin/adapters"
#)

add_executable(login_actor
    src/adapters/login.cpp
)
target_link_libraries(login_actor PRIVATE postline::postline CLI11::CLI11)
set_target_properties(login_actor PROPERTIES
    OUTPUT_NAME login
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/install/bin/adapters"
)


add_executable(dump_journal
    src/dump_journal.cpp
)
target_link_libraries(dump_journal PRIVATE postline_install_bin_deps)
set_target_properties(dump_journal PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${POSTLINE_INSTALL_BIN_DIR}"
)

add_executable(inspect_journal
    src/inspect_journal.cpp
)
target_link_libraries(inspect_journal PRIVATE postline_install_bin_deps)
set_target_properties(inspect_journal PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${POSTLINE_INSTALL_BIN_DIR}"
)

file(GLOB POSTLINE_SHELL_TOOL_SOURCES
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${CMAKE_CURRENT_SOURCE_DIR}/src/shell/*.cpp"
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/python/shell/read_file.py")
    list(REMOVE_ITEM POSTLINE_SHELL_TOOL_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shell/read_file.cpp"
    )
endif()

set(POSTLINE_SHELL_TOOL_TARGETS)
foreach(shell_tool_source IN LISTS POSTLINE_SHELL_TOOL_SOURCES)
    get_filename_component(shell_tool_name "${shell_tool_source}" NAME_WE)
    set(shell_tool_target "shell_${shell_tool_name}")
    add_executable("${shell_tool_target}" "${shell_tool_source}")
    target_link_libraries("${shell_tool_target}" PRIVATE CLI11::CLI11)
    set_target_properties("${shell_tool_target}" PROPERTIES
        OUTPUT_NAME "${shell_tool_name}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/install/bin/shell"
    )
    list(APPEND POSTLINE_SHELL_TOOL_TARGETS "${shell_tool_target}")
endforeach()

file(GLOB POSTLINE_PYTHON_SHELL_SCRIPTS
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${CMAKE_CURRENT_SOURCE_DIR}/python/shell/*.py"
)

add_custom_target(copy_python_shell_scripts ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_BINARY_DIR}/install/bin/shell"
)

foreach(script IN LISTS POSTLINE_PYTHON_SHELL_SCRIPTS)
    get_filename_component(script_name "${script}" NAME_WE)
    add_custom_command(
        TARGET copy_python_shell_scripts
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${script}"
            "${CMAKE_BINARY_DIR}/install/bin/shell/${script_name}"
    )
endforeach()

add_library(_postline MODULE
    src/python-api.cpp
    src/common.cpp
)

target_include_directories(_postline
    PRIVATE
        include
        ${CRCpp_SOURCE_DIR}/inc
)

target_link_libraries(_postline
    PRIVATE
    	postline::postline
        Python3::Module
        pybind11::headers
        nlohmann_json::nlohmann_json
        spdlog::spdlog_header_only
)

set_target_properties(_postline PROPERTIES
    PREFIX ""
    OUTPUT_NAME "_postline"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/install/python/postline"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/python/postline")
    add_custom_target(copy_python_tree ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/python/postline"
            "${CMAKE_BINARY_DIR}/install/python/postline"
    )
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/python/adapters")
        add_custom_command(TARGET copy_python_tree POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/python/adapters"
                "${CMAKE_BINARY_DIR}/install/bin/adapters"
        )
    endif()
endif()

file(GLOB POSTLINE_SCRIPTS
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*"
)
file(GLOB POSTLINE_PYTHON_SCRIPTS
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${CMAKE_CURRENT_SOURCE_DIR}/python/scripts/*"
)
set(POSTLINE_INSTALL_BIN_SCRIPTS
    ${POSTLINE_SCRIPTS}
    ${POSTLINE_PYTHON_SCRIPTS}
)

add_custom_target(copy_install_bin_scripts ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${POSTLINE_INSTALL_BIN_DIR}"
)

foreach(script IN LISTS POSTLINE_INSTALL_BIN_SCRIPTS)
    add_custom_command(
        TARGET copy_install_bin_scripts
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${script}"
            "${POSTLINE_INSTALL_BIN_DIR}/"
    )
endforeach()

if(BUILD_TESTS)
    include(CTest)
    enable_testing()

    add_executable(test_thread_focus tests/test_thread_focus.cpp)
    target_include_directories(test_thread_focus PRIVATE src/tui)
    target_link_libraries(test_thread_focus PRIVATE postline::tui_deps)

    add_test(NAME test_thread_focus COMMAND test_thread_focus)

    add_executable(test_thread_unread_state tests/test_thread_unread_state.cpp)
    target_include_directories(test_thread_unread_state PRIVATE src)
    target_link_libraries(test_thread_unread_state PRIVATE postline::postline)

    add_test(NAME test_thread_unread_state COMMAND test_thread_unread_state)

    add_executable(test_data tests/test_data.cpp)
    target_link_libraries(test_data PRIVATE postline::postline)

    add_test(NAME test_data COMMAND test_data)

    add_test(
        NAME runtime_smoke
        COMMAND ${CMAKE_COMMAND}
            -DPOSTLINE_BIN=$<TARGET_FILE:postline_bin>
            -DDUMP_JOURNAL_BIN=$<TARGET_FILE:dump_journal>
            -DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-runtime-smoke
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_smoke.cmake
    )

    add_test(
        NAME echo_round_trip
        COMMAND ${CMAKE_COMMAND}
            -DPOSTLINE_BIN=$<TARGET_FILE:postline_bin>
            -DDUMP_JOURNAL_BIN=$<TARGET_FILE:dump_journal>
            -DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-echo-round-trip
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/echo_round_trip.cmake
    )

    add_test(
        NAME cli_exit_while_pending
        COMMAND ${CMAKE_COMMAND}
            -DPOSTLINE_BIN=$<TARGET_FILE:postline_bin>
            -DTEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-cli-exit-while-pending
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_exit_while_pending.cmake
    )

    set_tests_properties(runtime_smoke echo_round_trip cli_exit_while_pending
        PROPERTIES TIMEOUT 15
    )

#    add_executable(test_journal tests/test_journal.cpp)
#    target_link_libraries(test_journal PRIVATE postline::postline)
#
#    add_executable(test_actor tests/test_actor.cpp)
#    target_link_libraries(test_actor PRIVATE postline::postline CLI11::CLI11)
#
#    add_executable(test_poller tests/test_poller.cpp)
#    target_link_libraries(test_poller PRIVATE postline::postline)
#
#    add_executable(test_program tests/test_program.cpp)
#    target_link_libraries(test_program PRIVATE postline::postline)
#
#    add_executable(test_email tests/test_email.cpp)
#    target_link_libraries(test_email PRIVATE postline::postline CLI11::CLI11)
#
#    add_executable(test_shell tests/test_shell.cpp)
#    target_link_libraries(test_shell PRIVATE postline::postline)
#
#    add_executable(test_stacktrace tests/test_stacktrace.cpp)
#    target_link_libraries(test_stacktrace PRIVATE postline::postline)

    #add_executable(test_ui tests/test_ui.cpp)
    #target_link_libraries(test_ui PRIVATE postline::postline postline::tui_deps)

    #add_test(NAME test_journal COMMAND test_journal)
endif()

install(TARGETS postline_bin
    RUNTIME DESTINATION bin
)

install(FILES arena.json
    DESTINATION .
)

install(TARGETS _postline
    LIBRARY DESTINATION python/postline
)

if(POSTLINE_SHELL_TOOL_TARGETS)
    install(TARGETS ${POSTLINE_SHELL_TOOL_TARGETS}
        RUNTIME DESTINATION bin/shell
    )
endif()

foreach(script IN LISTS POSTLINE_PYTHON_SHELL_SCRIPTS)
    get_filename_component(script_name "${script}" NAME_WE)
    install(PROGRAMS "${script}"
        DESTINATION bin/shell
        RENAME "${script_name}"
    )
endforeach()

if(POSTLINE_INSTALL_BIN_SCRIPTS)
    install(PROGRAMS ${POSTLINE_INSTALL_BIN_SCRIPTS}
        DESTINATION bin
    )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/python")
    install(DIRECTORY python/
        DESTINATION python
    )
endif()
