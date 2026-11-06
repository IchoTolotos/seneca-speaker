cmake_minimum_required(VERSION 3.16)
project(speaker_core_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

option(SC_SANITIZE "Build tests with ASan/UBSan" ON)
if(SC_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Vendored cJSON (MIT, see third_party/cJSON/LICENSE). On ESP-IDF the `json` component provides it.
add_library(cjson STATIC third_party/cJSON/cJSON.c)
target_compile_options(cjson PRIVATE -w)
target_include_directories(cjson PUBLIC third_party/cJSON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. speaker_core)
target_compile_options(speaker_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Werror)

add_executable(sc_tests
    test_main.c
    test_util.c
    test_jwt.c
    test_set_cookie.c
    test_cookie_store.c
    test_multipart.c
    test_session_json.c
    test_wm_events.c
    test_session_fsm.c
    test_voice_control.c
)
target_link_libraries(sc_tests PRIVATE speaker_core cjson)
target_compile_options(sc_tests PRIVATE -Wall -Wextra)
target_compile_definitions(sc_tests PRIVATE SC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

enable_testing()
foreach(suite util jwt set_cookie cookie_store multipart session_json wm_events session_fsm voice_control)
    add_test(NAME ${suite} COMMAND sc_tests ${suite})
endforeach()
