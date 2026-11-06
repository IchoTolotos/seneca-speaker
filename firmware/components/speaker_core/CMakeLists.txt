# speaker_core: hardware-independent firmware logic (portable C11).
# Works as an ESP-IDF component and as a plain CMake library for host tests.
set(SC_SRCS
    src/sc_util.c
    src/jwt.c
    src/set_cookie.c
    src/cookie_store.c
    src/multipart.c
    src/session_json.c
    src/wm_events.c
    src/session_fsm.c
    src/voice_control.c
)

if(ESP_PLATFORM)
    # cJSON: bundled `json` component up to IDF 5.x; managed `espressif/cjson`
    # (declared in idf_component.yml) from IDF 6.0 on.
    if(DEFINED IDF_VERSION_MAJOR AND IDF_VERSION_MAJOR GREATER_EQUAL 6)
        set(SC_JSON_REQ espressif__cjson)
    else()
        set(SC_JSON_REQ json)
    endif()
    idf_component_register(SRCS ${SC_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES ${SC_JSON_REQ})
    target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11)
else()
    # Host build: the parent project must provide a `cjson` target
    # (test/host vendors cJSON 1.7.18).
    add_library(speaker_core STATIC ${SC_SRCS})
    target_include_directories(speaker_core PUBLIC include)
    target_link_libraries(speaker_core PUBLIC cjson)
    set_target_properties(speaker_core PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
endif()
