cmake_minimum_required(VERSION 3.24)
project(webos-jetkvm VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
include(CTest)

set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_DEPS_LIBS OFF CACHE BOOL "" FORCE)
set(USE_NICE OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_SRTP OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_JUICE OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_USRSCTP OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
FetchContent_Declare(
    datachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG 2afeedc70878043bcd59d2d726e1476824f687c3
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(datachannel)
# Apply reviewed source fixes before compilation, including offline source caches.
set(SOURCE_DIR "${datachannel_SOURCE_DIR}")
set(PATCH_FILE "${CMAKE_SOURCE_DIR}/patches/native/0001-remove-unused-dependency-counters.patch")
include("${CMAKE_SOURCE_DIR}/cmake/apply_source_patch.cmake")
FetchContent_Declare(
    httplib
    URL
        https://raw.githubusercontent.com/yhirose/cpp-httplib/546e821c65a8bd8426c351d7e3171a99231b6456/httplib.h
    URL_HASH SHA256=913d769206b150e49712ad7e4d6fbe8de727df0bfda8f695b7682a77d03f35db
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(httplib)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
if(CMAKE_CROSSCOMPILING)
    if(
        NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"
        OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4
        OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^arm"
    )
        message(FATAL_ERROR "The runtime requires the ARM32 webOS toolchain")
    endif()
    # Migrate the former vendored UI default in existing build directories.
    if(JETKVM_WEB_DIR STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/ui/dist")
        unset(JETKVM_WEB_DIR CACHE)
    endif()
    set(JETKVM_WEB_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/build/jetkvm/ui/dist"
        CACHE PATH
        "Built JetKVM frontend"
    )
    if(NOT EXISTS "${JETKVM_WEB_DIR}/assets/jetkvm-source.tar.gz")
        message(FATAL_ERROR "Build the frontend first: npm run build:ui")
    endif()
    add_custom_target(
        web-assets
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_BINARY_DIR}/service/web"
        COMMAND
            "${CMAKE_COMMAND}" -E copy_directory "${JETKVM_WEB_DIR}"
            "${CMAKE_BINARY_DIR}/service/web"
    )
    add_executable(
        webos-jetkvm
        native/main.cpp
        native/server.cpp
        native/capture.cpp
        native/capture_lg.cpp
        native/capture_realtek.cpp
        native/input.cpp
        native/webos_input.cpp
        native/cursor.cpp
        native/annexb.cpp
        native/settings.cpp
        native/jetkvm.cpp
    )
    add_dependencies(webos-jetkvm web-assets)
    target_include_directories(
        webos-jetkvm
        PRIVATE "${httplib_SOURCE_DIR}" "${datachannel_SOURCE_DIR}/deps/json/single_include"
    )
    target_compile_options(webos-jetkvm PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(webos-jetkvm PRIVATE CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH=131072)
    # Bound a silent peer's close handshake well within the service's shutdown deadline.
    target_compile_definitions(webos-jetkvm PRIVATE CPPHTTPLIB_WEBSOCKET_CLOSE_TIMEOUT_SECOND=1)
    target_link_libraries(
        webos-jetkvm
        PRIVATE
            LibDataChannel::LibDataChannelStatic
            OpenSSL::SSL
            OpenSSL::Crypto
            Threads::Threads
            ${CMAKE_DL_LIBS}
    )
    set_target_properties(
        webos-jetkvm
        PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/service"
            BUILD_WITH_INSTALL_RPATH ON
            INSTALL_RPATH "$ORIGIN/lib"
    )

    add_subdirectory(native/stubs)
    target_link_libraries(webos-jetkvm PRIVATE webos_im)
    target_link_libraries(webos-jetkvm PRIVATE atomic rt)
    find_library(WAYLAND_CLIENT_LIBRARY NAMES wayland-client REQUIRED)
    find_library(WAYLAND_WEBOS_LIBRARY NAMES wayland-webos-client REQUIRED)
    target_link_libraries(
        webos-jetkvm
        PRIVATE "${WAYLAND_CLIENT_LIBRARY}" "${WAYLAND_WEBOS_LIBRARY}"
    )
    find_path(GLIB_INCLUDE_DIR glib.h PATH_SUFFIXES glib-2.0 REQUIRED)
    find_path(
        GLIB_CONFIG_DIR
        glibconfig.h
        PATHS "${CMAKE_SYSROOT}/usr/lib/glib-2.0/include"
        REQUIRED
    )
    find_library(GLIB_LIBRARY NAMES glib-2.0 REQUIRED)
    find_library(LS2_LIBRARY NAMES luna-service2 REQUIRED)
    find_library(PBNJSON_LIBRARY NAMES pbnjson_cpp REQUIRED)
    # Keep the optional firmware ABI out of the server's dynamic dependencies.
    add_library(webos-jetkvm-nyx SHARED native/capture_nyx.cpp)
    target_compile_options(webos-jetkvm-nyx PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(webos-jetkvm-nyx PRIVATE webos_nyx "${PBNJSON_LIBRARY}")
    set_target_properties(
        webos-jetkvm-nyx
        PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/service/lib"
            BUILD_WITH_INSTALL_RPATH ON
            INSTALL_RPATH "$ORIGIN"
    )
    add_dependencies(webos-jetkvm webos-jetkvm-nyx)
    # Keep the SDK immutable. A reviewed source overlay replaces GLib's GNU-only
    # C++ boolean expression with a lambda while retaining assignment diagnostics.
    set(WEBOS_GLIB_HEADERS "${CMAKE_BINARY_DIR}/sdk-include")
    configure_file(
        "${GLIB_INCLUDE_DIR}/glib/gmacros.h"
        "${WEBOS_GLIB_HEADERS}/glib/gmacros.h"
        COPYONLY
    )
    set(SOURCE_DIR "${WEBOS_GLIB_HEADERS}")
    set(PATCH_FILE "${CMAKE_SOURCE_DIR}/patches/native/0002-glib-cxx-boolean-expression.patch")
    include("${CMAKE_SOURCE_DIR}/cmake/apply_source_patch.cmake")
    target_include_directories(
        webos-jetkvm
        PRIVATE "${WEBOS_GLIB_HEADERS}" "${GLIB_INCLUDE_DIR}" "${GLIB_CONFIG_DIR}"
    )
    target_include_directories(
        webos-jetkvm-nyx
        PRIVATE "${WEBOS_GLIB_HEADERS}" "${GLIB_INCLUDE_DIR}" "${GLIB_CONFIG_DIR}"
    )
    add_library(glib-boolean-check OBJECT native/tests/glib_boolean_test.cpp)
    target_include_directories(
        glib-boolean-check
        PRIVATE "${WEBOS_GLIB_HEADERS}" "${GLIB_INCLUDE_DIR}" "${GLIB_CONFIG_DIR}"
    )
    target_compile_options(glib-boolean-check PRIVATE -Wall -Wextra -Wpedantic)
    add_executable(webos-jetkvm-service service/main.cpp service/control.cpp native/settings.cpp)
    add_dependencies(webos-jetkvm-service glib-boolean-check)
    target_include_directories(
        webos-jetkvm-service
        PRIVATE
            native
            "${WEBOS_GLIB_HEADERS}"
            "${GLIB_INCLUDE_DIR}"
            "${GLIB_CONFIG_DIR}"
            "${httplib_SOURCE_DIR}"
            "${datachannel_SOURCE_DIR}/deps/json/single_include"
    )
    target_compile_options(webos-jetkvm-service PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(
        webos-jetkvm-service
        PRIVATE "${LS2_LIBRARY}" "${GLIB_LIBRARY}" Threads::Threads atomic rt
    )
    set_target_properties(
        webos-jetkvm-service
        PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/service"
            BUILD_WITH_INSTALL_RPATH ON
            INSTALL_RPATH "$ORIGIN/lib"
    )
    foreach(
        runtime
        IN
        ITEMS libssl.so.3 libcrypto.so.3 libstdc++.so.6 libgcc_s.so.1 libatomic.so.1
    )
        string(MAKE_C_IDENTIFIER "${runtime}" runtime_variable)
        find_library(${runtime_variable} NAMES "${runtime}" REQUIRED)
        configure_file(
            "${${runtime_variable}}"
            "${CMAKE_BINARY_DIR}/service/lib/${runtime}"
            COPYONLY
        )
    endforeach()
    configure_file(services.json "${CMAKE_BINARY_DIR}/service/services.json" COPYONLY)
    configure_file(autostart.sh "${CMAKE_BINARY_DIR}/service/autostart.sh" COPYONLY)
    set(WEBOS_STAGE_DIR
        "${CMAKE_SOURCE_DIR}/build/package/service"
        CACHE PATH
        "IPK service staging directory"
    )
    add_custom_target(
        stage-service
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${WEBOS_STAGE_DIR}"
        COMMAND
            "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_BINARY_DIR}/service" "${WEBOS_STAGE_DIR}"
        DEPENDS webos-jetkvm webos-jetkvm-service
    )
    foreach(component IN ITEMS libjuice libsrtp plog)
        configure_file(
            "${datachannel_SOURCE_DIR}/deps/${component}/LICENSE"
            "${CMAKE_BINARY_DIR}/service/licenses/${component}.txt"
            COPYONLY
        )
    endforeach()
    configure_file(
        "${datachannel_SOURCE_DIR}/LICENSE"
        "${CMAKE_BINARY_DIR}/service/licenses/libdatachannel.txt"
        COPYONLY
    )
    configure_file(
        "${datachannel_SOURCE_DIR}/deps/usrsctp/LICENSE.md"
        "${CMAKE_BINARY_DIR}/service/licenses/usrsctp.txt"
        COPYONLY
    )
    configure_file(
        "${datachannel_SOURCE_DIR}/deps/json/LICENSE.MIT"
        "${CMAKE_BINARY_DIR}/service/licenses/json.txt"
        COPYONLY
    )
    configure_file(
        native/httplib-LICENSE
        "${CMAKE_BINARY_DIR}/service/licenses/httplib.txt"
        COPYONLY
    )
    configure_file(
        native/THIRD_PARTY.md
        "${CMAKE_BINARY_DIR}/service/licenses/THIRD_PARTY.md"
        COPYONLY
    )
    configure_file(LICENSE "${CMAKE_BINARY_DIR}/service/licenses/webos-jetkvm.txt" COPYONLY)
endif()
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(settings-test native/tests/settings_test.cpp native/settings.cpp)
    target_include_directories(
        settings-test
        PRIVATE native "${datachannel_SOURCE_DIR}/deps/json/single_include"
    )
    add_test(NAME settings COMMAND settings-test)
    add_executable(annexb-test native/tests/annexb_test.cpp native/annexb.cpp)
    target_include_directories(annexb-test PRIVATE native)
    add_test(NAME annexb COMMAND annexb-test)
    add_executable(
        jetkvm-test
        native/tests/jetkvm_test.cpp
        native/jetkvm.cpp
        native/input.cpp
        native/settings.cpp
    )
    target_include_directories(
        jetkvm-test
        PRIVATE native "${datachannel_SOURCE_DIR}/deps/json/single_include"
    )
    target_link_libraries(jetkvm-test PRIVATE Threads::Threads)
    add_test(NAME jetkvm-protocol COMMAND jetkvm-test)
    add_library(capture-backend-fixture MODULE native/tests/capture_backend_fixture.cpp)
    target_include_directories(capture-backend-fixture PRIVATE native)
    add_executable(
        lifecycle-test
        native/tests/lifecycle_test.cpp
        native/tests/capture_test.cpp
        native/capture.cpp
        native/capture_realtek.cpp
    )
    add_dependencies(lifecycle-test capture-backend-fixture)
    target_include_directories(lifecycle-test PRIVATE native)
    target_link_libraries(lifecycle-test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME lifecycle COMMAND lifecycle-test "$<TARGET_FILE:capture-backend-fixture>")
    add_executable(
        control-test
        native/tests/control_test.cpp
        service/control.cpp
        native/settings.cpp
    )
    target_include_directories(
        control-test
        PRIVATE
            native
            service
            "${httplib_SOURCE_DIR}"
            "${datachannel_SOURCE_DIR}/deps/json/single_include"
    )
    target_link_libraries(control-test PRIVATE Threads::Threads)
    add_test(NAME service-control COMMAND control-test)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(NODE_EXECUTABLE node REQUIRED)
    add_test(
        NAME async-tools
        COMMAND "${NODE_EXECUTABLE}" --test "${CMAKE_SOURCE_DIR}/tools/tests/async.test.mjs"
    )
    add_test(
        NAME tv-target
        COMMAND "${NODE_EXECUTABLE}" --test "${CMAKE_SOURCE_DIR}/tools/tests/tv_target.test.mjs"
    )
    add_test(
        NAME link-stub-generator
        COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_SOURCE_DIR}/tools/tests" -v
    )
    set_tests_properties(
        settings
        annexb
        jetkvm-protocol
        lifecycle
        service-control
        link-stub-generator
        async-tools
        tv-target
        PROPERTIES TIMEOUT 20
    )
endif()
