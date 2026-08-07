cmake_minimum_required(VERSION 3.16)
project(ngh C)

# ngh is header-only: this project exists to build the tests and examples.
# To use ngh, copy ngh_mediapipe.h and link nothing.

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(NGH_BUILD_TESTS "Build the test suite" ON)
option(NGH_BUILD_EXAMPLES "Build the examples" ON)
option(NGH_EXAMPLES_SDL3 "Build examples that need SDL3 (camera capture)" OFF)
option(NGH_WERROR "Treat warnings as errors" OFF)

add_library(ngh INTERFACE)
target_include_directories(ngh INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT WIN32 AND NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_link_libraries(ngh INTERFACE ${CMAKE_DL_LIBS} Threads::Threads)
    target_compile_definitions(ngh INTERFACE _POSIX_C_SOURCE=200809L)
endif()

if(MSVC)
    set(NGH_WARNINGS /W4)
    if(NGH_WERROR)
        list(APPEND NGH_WARNINGS /WX)
    endif()
else()
    set(NGH_WARNINGS -Wall -Wextra -Wpedantic)
    if(NGH_WERROR)
        list(APPEND NGH_WARNINGS -Werror)
    endif()
endif()

# scripts/fetch_testdata.py puts stb_image.h and the golden files here.
set(NGH_TESTDATA "${CMAKE_CURRENT_SOURCE_DIR}/vendor/testdata")
set(NGH_HAVE_TESTDATA FALSE)
if(EXISTS "${NGH_TESTDATA}/stb_image.h")
    set(NGH_HAVE_TESTDATA TRUE)
endif()

function(ngh_add_program name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ngh)
    target_compile_options(${name} PRIVATE ${NGH_WARNINGS})
endfunction()

# ngh_runtime_load(NULL) searches the executable's own directory, so putting the
# library next to the binaries is all it takes -- and it exercises that search
# path rather than the NGH_MEDIAPIPE_PATH override.
if(NOT EMSCRIPTEN)
    foreach(lib libmediapipe.so libmediapipe.dll libmediapipe.dylib)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/${lib}")
            file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/vendor/${lib}"
                 DESTINATION "${CMAKE_BINARY_DIR}")
            # Multi-config generators put binaries in a subdirectory.
            foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
                file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/vendor/${lib}"
                     DESTINATION "${CMAKE_BINARY_DIR}/${cfg}")
            endforeach()
            message(STATUS "ngh: staged vendor/${lib}")
        endif()
    endforeach()
endif()

# stb_image decodes JPEGs for the tests and two examples. Third-party, so it
# gets its own target without our warning flags.
if(NGH_HAVE_TESTDATA AND NOT EMSCRIPTEN)
    add_library(ngh_stb_image STATIC tests/stb_image_impl.c)
    target_include_directories(ngh_stb_image PUBLIC ${NGH_TESTDATA})
    if(NOT WIN32)
        target_link_libraries(ngh_stb_image PUBLIC m)
    endif()
endif()

# -------------------------------------------------- webtransport backend --
# Prebuilt shim for ngh_webtransport.h. scripts/fetch_picoquic.py stages the
# pinned, patched picoquic; without it this section is skipped silently.

set(NGH_PICOQUIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/picoquic")
if(EXISTS "${NGH_PICOQUIC}/.ngh_built" AND NOT EMSCRIPTEN)
    find_package(OpenSSL REQUIRED)
    find_package(Threads REQUIRED)
    add_library(ngh_wt_backend SHARED backend/wt_backend.c)
    target_include_directories(ngh_wt_backend PRIVATE
        "${NGH_PICOQUIC}/picoquic"
        "${NGH_PICOQUIC}/picohttp"
        "${NGH_PICOQUIC}/build/_deps/picotls-src/include")
    target_link_directories(ngh_wt_backend PRIVATE
        "${NGH_PICOQUIC}/build"
        "${NGH_PICOQUIC}/build/_deps/picotls-build"
        # Multi-config generators (MSVC) nest the archives one level deeper.
        "${NGH_PICOQUIC}/build/Release"
        "${NGH_PICOQUIC}/build/_deps/picotls-build/Release")
    # The list repeats because picoquic-core and picoquic-log are mutually
    # referential static archives.
    target_link_libraries(ngh_wt_backend PRIVATE
        picohttp-core picoquic-log picoquic-core
        picohttp-core picoquic-log picoquic-core
        picotls-openssl picotls-fusion picotls-minicrypto picotls-core
        OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
    find_library(NGH_BROTLI_DEC brotlidec)
    find_library(NGH_BROTLI_ENC brotlienc)
    if(NGH_BROTLI_DEC AND NGH_BROTLI_ENC)
        target_link_libraries(ngh_wt_backend PRIVATE
            ${NGH_BROTLI_DEC} ${NGH_BROTLI_ENC})
    endif()
    if(WIN32)
        target_link_libraries(ngh_wt_backend PRIVATE ws2_32 iphlpapi)
    else()
        target_link_libraries(ngh_wt_backend PRIVATE ${CMAKE_DL_LIBS} m)
    endif()
    if(NOT WIN32 AND NOT APPLE)
        # Keep picoquic's symbols out of the shared library's export table.
        target_link_options(ngh_wt_backend PRIVATE "LINKER:--exclude-libs,ALL")
    endif()
    set_target_properties(ngh_wt_backend PROPERTIES
        C_VISIBILITY_PRESET hidden
        POSITION_INDEPENDENT_CODE ON)

    add_executable(ngh_wt_backend_check backend/wt_backend_check.c)
    target_include_directories(ngh_wt_backend_check PRIVATE backend)
    target_link_libraries(ngh_wt_backend_check PRIVATE ngh_wt_backend)
    target_compile_options(ngh_wt_backend_check PRIVATE ${NGH_WARNINGS})
else()
    message(STATUS
        "ngh: skipping ngh_wt_backend -- run scripts/fetch_picoquic.py first")
endif()

# ---------------------------------------------------------------- tests --

if(NGH_BUILD_TESTS)
    enable_testing()

    if(EMSCRIPTEN)
        # The browser-only paths cannot run here; test_web covers the wiring,
        # error propagation and argument checks under node.
        add_executable(test_web tests/test_web.c)
        target_link_libraries(test_web PRIVATE ngh)
        target_include_directories(test_web PRIVATE tests)
        target_compile_options(test_web PRIVATE ${NGH_WARNINGS})
        set_target_properties(test_web PROPERTIES
            SUFFIX ".js"
            LINK_FLAGS "-sENVIRONMENT=node -sEXIT_RUNTIME=1")
        add_test(NAME test_web
                 COMMAND node $<TARGET_FILE:test_web>)

        # The coordinate helpers are pure math and identical on every
        # platform; run that part under node too.
        add_executable(test_space tests/test_space.c)
        target_link_libraries(test_space PRIVATE ngh)
        target_include_directories(test_space PRIVATE tests)
        target_compile_options(test_space PRIVATE ${NGH_WARNINGS})
        set_target_properties(test_space PROPERTIES
            SUFFIX ".js"
            LINK_FLAGS "-sENVIRONMENT=node -sEXIT_RUNTIME=1")
        add_test(NAME test_space
                 COMMAND node $<TARGET_FILE:test_space>)

        add_executable(test_wt_web tests/test_wt_web.c)
        target_link_libraries(test_wt_web PRIVATE ngh)
        target_include_directories(test_wt_web PRIVATE tests)
        target_compile_options(test_wt_web PRIVATE ${NGH_WARNINGS})
        set_target_properties(test_wt_web PROPERTIES
            SUFFIX ".js"
            LINK_FLAGS "-sENVIRONMENT=node -sEXIT_RUNTIME=1")
        add_test(NAME test_wt_web
                 COMMAND node $<TARGET_FILE:test_wt_web>)
    else()
        ngh_add_program(test_abi tests/test_abi.c)
        target_include_directories(test_abi PRIVATE tests)
        add_test(NAME test_abi COMMAND test_abi)

        # Needs the backend library next to the test binary; exits 77
        # (skip) when it is absent.
        ngh_add_program(test_wt tests/test_wt.c)
        target_include_directories(test_wt PRIVATE tests)
        add_test(NAME test_wt COMMAND test_wt)
        set_tests_properties(test_wt PROPERTIES SKIP_RETURN_CODE 77)

        # Cross-stack interop: our picoquic client against a quinn-based
        # wtransport echo server (tests/wt-echo, Rust). Opt-in because it
        # compiles a cargo project and opens a localhost UDP port.
        option(NGH_WT_INTEROP
            "Enable the WebTransport cross-stack interop test (needs cargo)"
            OFF)
        if(NGH_WT_INTEROP)
            if(NOT NGH_BUILD_EXAMPLES)
                message(FATAL_ERROR
                    "NGH_WT_INTEROP runs the ngh_wt_echo example; enable "
                    "NGH_BUILD_EXAMPLES")
            endif()
            find_program(NGH_CARGO cargo REQUIRED)
            find_package(Python3 REQUIRED COMPONENTS Interpreter)
            add_test(NAME test_wt_interop
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/wt_interop.py
                             $<TARGET_FILE:ngh_wt_echo>)
            set_tests_properties(test_wt_interop PROPERTIES TIMEOUT 700)
        endif()

        if(NGH_HAVE_TESTDATA)
            foreach(t test_golden test_api test_space)
                ngh_add_program(${t} tests/${t}.c)
                target_include_directories(${t} PRIVATE tests)
                target_link_libraries(${t} PRIVATE ngh_stb_image)
                add_test(NAME ${t}
                         COMMAND ${t} ${CMAKE_CURRENT_SOURCE_DIR})
                # 77 is the automake "skipped" convention: the tests use it
                # when libmediapipe cannot be found.
                set_tests_properties(${t} PROPERTIES SKIP_RETURN_CODE 77)
            endforeach()
        else()
            message(STATUS
                "ngh: skipping test_golden/test_api -- run "
                "scripts/fetch_testdata.py first")
        endif()
    endif()
endif()

# ------------------------------------------------------------- examples --

if(NGH_BUILD_EXAMPLES AND NOT EMSCRIPTEN)
    ngh_add_program(ngh_game_loop examples/desktop/game_loop.c)
    ngh_add_program(ngh_wt_echo examples/desktop/wt_echo.c)
    if(NGH_HAVE_TESTDATA)
        ngh_add_program(ngh_still_image examples/desktop/still_image.c)
        target_link_libraries(ngh_still_image PRIVATE ngh_stb_image)
    endif()

    if(NGH_EXAMPLES_SDL3)
        find_package(SDL3 CONFIG QUIET)
        if(NOT SDL3_FOUND)
            include(FetchContent)
            set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
            # The tag lub pins its SDL submodule to; keep them matched.
            FetchContent_Declare(SDL3
                GIT_REPOSITORY https://github.com/libsdl-org/SDL
                GIT_TAG release-3.2.30
                GIT_SHALLOW TRUE)
            FetchContent_MakeAvailable(SDL3)
        endif()
        ngh_add_program(ngh_camera_face examples/desktop/camera_face.c)
        target_link_libraries(ngh_camera_face PRIVATE SDL3::SDL3)
    endif()
endif()
