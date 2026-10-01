cmake_minimum_required(VERSION 3.10)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(BOREALIS_LIBRARY ${CMAKE_CURRENT_SOURCE_DIR}/library/borealis/library)

# build options
include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)

# Dependencies
option(USE_SHARED_LIB "Whether to use shared libs provided by system" OFF)
cmake_dependent_option(USE_SYSTEM_FMT "" OFF "NOT USE_SHARED_LIB" ON)
cmake_dependent_option(USE_SYSTEM_TINYXML2 "" OFF "NOT USE_SHARED_LIB" ON)
cmake_dependent_option(USE_SYSTEM_TWEENY "" OFF "NOT USE_SHARED_LIB" ON)

if (APPLE AND PLATFORM_DESKTOP)
    option(BUNDLE_MACOS_APP "Bundle a app for macOS" OFF)
    if (BUNDLE_MACOS_APP)
        # Universal binary for distribution. Every linked library must be
        # universal too (Homebrew's OpenSSL is single-arch, which makes the
        # libwebsockets feature checks fail), so only force it when bundling.
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    elseif (CMAKE_OSX_ARCHITECTURES STREQUAL "arm64;x86_64")
        # Drop the universal setting an earlier configure left in the cache
        unset(CMAKE_OSX_ARCHITECTURES CACHE)
    endif ()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
    set(CMAKE_XCODE_ATTRIBUTE_MACOSX_DEPLOYMENT_TARGET[arch=arm64] "11.0" CACHE STRING "" FORCE)
    message(STATUS "CMAKE_OSX_ARCHITECTURES: ${CMAKE_OSX_ARCHITECTURES}")
endif ()

# toolchain
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

if (PLATFORM_SWITCH)
    # borealis' toolchain.cmake replaces CMAKE_C_FLAGS before project(), which
    # discards the flags devkitPro's NintendoSwitch platform would have
    # initialised them with. Restore them so every sub-project (protobuf,
    # libwebsockets, libdatachannel, …) is built the way libnx expects:
    # software thread pointer, local-exec TLS and the Tegra X1 ISA.
    set(NX_TARGET_FLAGS "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec -ffunction-sections -fdata-sections -D__SWITCH__ -D_GNU_SOURCE")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${NX_TARGET_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${NX_TARGET_FLAGS}")
    # Switch.cmake is include()d rather than passed as CMAKE_TOOLCHAIN_FILE, so
    # try_compile() projects (check_type_size etc. in libsrtp) would evaluate
    # Platform/NintendoSwitch.cmake with these unset and fail to link.
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES DEVKITPRO NX_ROOT)
endif ()

# project info
project(vkpcnx)
set(VERSION_MAJOR "1")
set(VERSION_MINOR "0")
set(VERSION_ALTER "0")
set(VERSION_BUILD "0")
set(PROJECT_DISPLAY_NAME "VK Play Cloud NX")
set(PROJECT_AUTHOR "m4l3vich")
set(PROJECT_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/icon.jpg)
set(PROJECT_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/resources)

if (USE_LIBROMFS)
    add_libromfs(${PROJECT_NAME} ${PROJECT_RESOURCES})
endif ()

add_subdirectory(library)

find_package(Threads REQUIRED)
list(APPEND APP_PLATFORM_LIB ${CMAKE_THREAD_LIBS_INIT})

# nlohmann/json (settings storage)
include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# nayuki/QR-Code-generator (QR code generation)
FetchContent_Declare(qrcodegen
    GIT_REPOSITORY https://github.com/nayuki/QR-Code-generator.git
    GIT_TAG v1.8.0)
FetchContent_MakeAvailable(qrcodegen)
add_library(qrcodegen STATIC ${qrcodegen_SOURCE_DIR}/cpp/qrcodegen.cpp)
target_include_directories(qrcodegen PUBLIC ${qrcodegen_SOURCE_DIR}/cpp)


# setting src and include
list(APPEND APP_PLATFORM_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/vkpcnx/include)
file(GLOB_RECURSE MAIN_SRC vkpcnx/*.cpp)

set(PLATFORM_OPTION)
if (PLATFORM_DESKTOP)
    if (WIN32)
        configure_file("${CMAKE_SOURCE_DIR}/vkpcnx/resource.rc.in" "${CMAKE_BINARY_DIR}/resource.rc")
        list(APPEND MAIN_SRC ${CMAKE_BINARY_DIR}/resource.rc)
        if (MINGW)
            set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++ -static" CACHE STRING "" FORCE)
        endif ()
    elseif (BUNDLE_MACOS_APP)
        list(APPEND MAIN_SRC ${CMAKE_SOURCE_DIR}/vkpcnx/vkpcnx.icns)
    endif ()
elseif (PLATFORM_SWITCH)
    set(APP_PLATFORM_LIB
        # needed by borealis
        glfw3 EGL glapi drm_nouveau
        # base lib
        nx m
    )
    list(APPEND MAIN_SRC ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)
endif ()

# libcurl (https client)
if (PLATFORM_SWITCH)
    # devkitPro portlibs: pacman -S switch-curl switch-zlib switch-libopus switch-ffmpeg
    # (switch-curl uses the libnx SSL service, not mbedTLS)
    add_definitions(-DCURL_STATICLIB)
    list(APPEND APP_PLATFORM_INCLUDE ${DEVKITPRO}/portlibs/switch/include)
    # <endian.h> for the generated protobuf headers (newlib only has machine/endian.h)
    list(APPEND APP_PLATFORM_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/switch/compat)
    # must precede nx (sockets) in link order
    list(PREPEND APP_PLATFORM_LIB curl z)

    # mbedTLS 3.6 (DTLS-SRTP capable) for libwebsockets and libdatachannel
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/switch/SwitchMbedTls.cmake)
else ()
    find_package(CURL REQUIRED)
    list(APPEND APP_PLATFORM_LIB CURL::libcurl)
endif ()

# libwebsockets (websocket client)
# No devkitPro portlib exists for this on Switch, so it's built from source
# on every platform for a single, consistent codepath.
FetchContent_Declare(libwebsockets
    GIT_REPOSITORY https://github.com/warmcat/libwebsockets.git
    GIT_TAG v4.3.3
    # Switch: no /dev/urandom, randomness comes from libnx randomGet()
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=<SOURCE_DIR>
        -DPATCH_DIR=${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/libwebsockets
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyPatches.cmake
    UPDATE_DISCONNECTED TRUE)

set(LWS_WITH_STATIC ON CACHE BOOL "" FORCE)
set(LWS_WITH_SHARED OFF CACHE BOOL "" FORCE)
set(LWS_WITHOUT_SERVER ON CACHE BOOL "" FORCE)
set(LWS_WITHOUT_TESTAPPS ON CACHE BOOL "" FORCE)
set(LWS_WITH_MINIMAL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LWS_WITH_SSL ON CACHE BOOL "" FORCE)

if (PLATFORM_SWITCH)
    # libnx has no unix sockets / daemons / signals worth speaking of
    set(LWS_WITH_UNIX_SOCK OFF CACHE BOOL "" FORCE)
    set(LWS_WITHOUT_DAEMONIZE ON CACHE BOOL "" FORCE)
    set(LWS_WITH_SYS_SMD OFF CACHE BOOL "" FORCE)
    set(LWS_WITH_MBEDTLS ON CACHE BOOL "" FORCE)
    set(LWS_MBEDTLS_INCLUDE_DIRS ${SWITCH_MBEDTLS_PREFIX}/include CACHE PATH "" FORCE)
    set(LWS_MBEDTLS_LIBRARIES
        ${SWITCH_MBEDTLS_PREFIX}/lib/libmbedtls.a
        ${SWITCH_MBEDTLS_PREFIX}/lib/libmbedx509.a
        ${SWITCH_MBEDTLS_PREFIX}/lib/libmbedcrypto.a
        CACHE STRING "" FORCE)
endif ()

FetchContent_MakeAvailable(libwebsockets)

if (PLATFORM_SWITCH)
    # lws exports its target for install and CMake refuses build-tree paths in
    # INTERFACE_INCLUDE_DIRECTORIES; the mbedTLS install lives in the build
    # tree, so mark those entries build-only.
    get_target_property(LWS_IFACE_INC websockets INTERFACE_INCLUDE_DIRECTORIES)
    set(LWS_IFACE_INC_FIXED)
    foreach (INC ${LWS_IFACE_INC})
        if (INC MATCHES "^${CMAKE_BINARY_DIR}" AND NOT INC MATCHES "^\\$<")
            list(APPEND LWS_IFACE_INC_FIXED "$<BUILD_INTERFACE:${INC}>")
        else ()
            list(APPEND LWS_IFACE_INC_FIXED "${INC}")
        endif ()
    endforeach ()
    set_target_properties(websockets PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${LWS_IFACE_INC_FIXED}")
    # Stubs for the Unix headers lws includes unconditionally but libnx lacks
    target_include_directories(websockets PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/switch/compat)
    # Linux-only socket-option names used by lws's unix platform code; SOL_TCP
    # is IPPROTO_TCP on BSD stacks, SO_PRIORITY is only used when a priority
    # is explicitly configured (never here), so any unused value will do.
    target_compile_definitions(websockets PRIVATE SOL_TCP=IPPROTO_TCP SO_PRIORITY=0x7fff)
    # Unix-isms lws assumes (private mbedTLS 3 symbol, rlimits); see the header
    target_compile_options(websockets PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/cmake/switch/compat/lws_libnx_compat.h)
endif ()

# protobuf
# No devkitPro portlib exists for this on Switch, so it's built from source on
# every platform for a single, consistent codepath. protoc must always run on
# the host, so when cross-compiling (Switch) a native protoc is built first
# via ExternalProject and handed to the target-only (runtime library) build.
# Pinned to the last pre-Abseil release (v21.x) to keep the cross build simple.
set(PROTOBUF_GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git)
set(PROTOBUF_GIT_TAG v21.12)

set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)

if (PLATFORM_SWITCH)
    include(ExternalProject)
    set(PROTOC_HOST_PREFIX ${CMAKE_BINARY_DIR}/protoc-host)
    ExternalProject_Add(protoc_host
        GIT_REPOSITORY ${PROTOBUF_GIT_REPOSITORY}
        GIT_TAG ${PROTOBUF_GIT_TAG}
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR cmake
        CMAKE_ARGS
            -DCMAKE_TOOLCHAIN_FILE=
            -DCMAKE_BUILD_TYPE=Release
            -Dprotobuf_BUILD_TESTS=OFF
            -Dprotobuf_BUILD_EXAMPLES=OFF
            -Dprotobuf_BUILD_LIBPROTOC=ON
            -Dprotobuf_BUILD_PROTOC_BINARIES=ON
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target protoc --config Release
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS <BINARY_DIR>/protoc
    )
    ExternalProject_Get_Property(protoc_host BINARY_DIR)
    set(protobuf_BUILD_PROTOC_BINARIES OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_LIBPROTOC OFF CACHE BOOL "" FORCE)
    set(protobuf_PROTOC_EXE ${BINARY_DIR}/protoc CACHE FILEPATH "" FORCE)
endif ()

FetchContent_Declare(protobuf
    GIT_REPOSITORY ${PROTOBUF_GIT_REPOSITORY}
    GIT_TAG ${PROTOBUF_GIT_TAG}
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR cmake)
FetchContent_MakeAvailable(protobuf)

if (PLATFORM_SWITCH)
    add_dependencies(libprotobuf protoc_host)
    # newlib has no <endian.h>
    target_include_directories(libprotobuf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/switch/compat)
endif ()

# protobuf_generate() isn't available in v21.x without find_package(), so
# .proto -> .pb.cc/.pb.h generation is wired up by hand here.
if (TARGET protobuf::protoc)
    set(PROTOC_COMMAND protobuf::protoc)
else ()
    set(PROTOC_COMMAND ${protobuf_PROTOC_EXE})
endif ()

set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})
file(GLOB PROTO_FILES ${CMAKE_SOURCE_DIR}/vkpcnx/proto/*.proto)
foreach (PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    set(PROTO_GEN_SRC ${PROTO_GEN_DIR}/${PROTO_NAME}.pb.cc)
    set(PROTO_GEN_HDR ${PROTO_GEN_DIR}/${PROTO_NAME}.pb.h)
    add_custom_command(
        OUTPUT ${PROTO_GEN_SRC} ${PROTO_GEN_HDR}
        COMMAND ${PROTOC_COMMAND}
            --cpp_out=${PROTO_GEN_DIR}
            -I ${CMAKE_SOURCE_DIR}/vkpcnx/proto
            ${PROTO_FILE}
        DEPENDS ${PROTO_FILE} ${PROTOC_COMMAND}
        COMMENT "Generating protobuf sources from ${PROTO_NAME}.proto"
        VERBATIM
    )
    list(APPEND MAIN_SRC ${PROTO_GEN_SRC})
    list(APPEND PROTO_GEN_SRCS ${PROTO_GEN_SRC})
endforeach ()

# streaming client dependencies (libdatachannel, FFmpeg, opus, zlib, audio)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/StreamingDeps.cmake)

# libwebp + libwebpdemux (animated WebP playback, view/animated_webp.hpp)
if (PLATFORM_SWITCH)
    # pacman -S switch-libwebp
    pkg_check_modules(WEBP REQUIRED libwebpdemux libwebp)
    list(APPEND APP_PLATFORM_INCLUDE ${WEBP_STATIC_INCLUDE_DIRS})
    # static archives: must precede m/pthread providers (nx) in link order
    list(PREPEND APP_PLATFORM_LIB ${WEBP_STATIC_LDFLAGS})
else ()
    pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebpdemux libwebp)
    list(APPEND APP_PLATFORM_LIB PkgConfig::WEBP)
endif ()

# Build stamp for diag::buildInfo(): `git describe` is re-read on every build
set(VKPCNX_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${VKPCNX_GEN_DIR})
add_custom_target(vkpcnx_version
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUT=${VKPCNX_GEN_DIR}/vkpcnx_version.h
        -P ${CMAKE_SOURCE_DIR}/cmake/GitVersion.cmake
    BYPRODUCTS ${VKPCNX_GEN_DIR}/vkpcnx_version.h
    COMMENT "Stamping git version"
    VERBATIM)
set(VKPCNX_BUILD_DEFINES
    VKPCNX_VERSION="${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}"
    VKPCNX_BUILD_TYPE="${CMAKE_BUILD_TYPE}")

# building target
program_target(${PROJECT_NAME} "${MAIN_SRC}")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)
add_dependencies(${PROJECT_NAME} vkpcnx_version)
target_include_directories(${PROJECT_NAME} PRIVATE ${VKPCNX_GEN_DIR})
target_compile_definitions(${PROJECT_NAME} PRIVATE ${VKPCNX_BUILD_DEFINES})
# Release builds keep debug info so crash addresses can be symbolized; CI
# splits it off into the archived symbol files (vkpcnx-*-symbols), and the
# Switch .nro never contains it. --build-id gives each binary the ID that
# crash logs and Atmosphère reports quote.
target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:-g>)
if (NOT APPLE)
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,--build-id)
endif ()
target_link_libraries(${PROJECT_NAME} PRIVATE
    nlohmann_json::nlohmann_json qrcodegen websockets vkpcnx_streaming_deps)


# building release file
if (PLATFORM_DESKTOP)
    if (BUNDLE_MACOS_APP)
        set_target_properties(${PROJECT_NAME} PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME ${PROJECT_NAME}
            MACOSX_BUNDLE_BUNDLE_VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}"
            MACOSX_BUNDLE_LONG_VERSION_STRING "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${VERSION_MAJOR}.${VERSION_MINOR}"
            MACOSX_BUNDLE_ICON_FILE "vkpcnx.icns"
            MACOSX_BUNDLE_COPYRIGHT "Copyright 2026 ${PROJECT_AUTHOR}"
            RESOURCE "${CMAKE_SOURCE_DIR}/vkpcnx/vkpcnx.icns"
        )
        add_custom_target(${PROJECT_NAME}.data
                COMMAND "${CMAKE_COMMAND}" -E copy_directory ${PROJECT_RESOURCES} ${PROJECT_NAME}.app/Contents/Resources/resources
        )
    else ()
        add_custom_target(${PROJECT_NAME}.data
                COMMAND "${CMAKE_COMMAND}" -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_CURRENT_BINARY_DIR}/resources
        )
    endif ()
    if (NOT USE_LIBROMFS)
        add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}.data)
    endif ()
elseif (PLATFORM_SWITCH)
    add_custom_target(${PROJECT_NAME}.nro DEPENDS ${PROJECT_NAME}
        COMMAND ${NX_NACPTOOL_EXE} --create "${PROJECT_DISPLAY_NAME}" "${PROJECT_AUTHOR}" "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_ALTER}" ${PROJECT_NAME}.nacp --titleid=${PROJECT_TITLEID}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_BINARY_DIR}/resources
        COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_BINARY_DIR}/resources/font/switch_icons.ttf
        COMMAND ${NX_ELF2NRO_EXE} ${PROJECT_NAME}.elf ${PROJECT_NAME}.nro --icon=${PROJECT_ICON} --nacp=${PROJECT_NAME}.nacp --romfsdir=${CMAKE_BINARY_DIR}/resources
    )
endif ()

if (PLATFORM_SWITCH)
    add_dependencies(${PROJECT_NAME} protoc_host)
    # libnx's getnameinfo ignores NI_NUMERICHOST; see switch_resolver.cpp.
    # Bigger UDP receive buffers for the video stream; see switch_socket_config.cpp
    target_link_options(${PROJECT_NAME} PRIVATE
        -Wl,--wrap=getnameinfo -Wl,--wrap=socketInitialize)
endif ()

target_include_directories(${PROJECT_NAME} PRIVATE ${PROTO_GEN_DIR})
target_include_directories(${PROJECT_NAME} PRIVATE vkpcnx ${APP_PLATFORM_INCLUDE})
target_compile_options(${PROJECT_NAME} PRIVATE -ffunction-sections -fdata-sections ${APP_PLATFORM_OPTION})
target_link_libraries(${PROJECT_NAME} PRIVATE borealis ${APP_PLATFORM_LIB} protobuf::libprotobuf)

# Unit tests for the pure protocol pieces (desktop only, `ctest` or run the binary)
option(VKPCNX_BUILD_TESTS "Build protocol unit tests" OFF)
if (VKPCNX_BUILD_TESTS AND PLATFORM_DESKTOP)
    add_executable(stream_protocol_test
        tests/stream_protocol_test.cpp
        vkpcnx/src/core/stream/wire.cpp
        vkpcnx/src/core/stream/play_url.cpp
        vkpcnx/src/core/stream/input_encoder.cpp
        vkpcnx/src/core/stream/sdp_munging.cpp
        vkpcnx/src/core/stream/text_keystrokes.cpp
        ${PROTO_GEN_DIR}/protocol_common.pb.cc)
    set_target_properties(stream_protocol_test PROPERTIES CXX_STANDARD 17)
    target_include_directories(stream_protocol_test PRIVATE vkpcnx/include ${PROTO_GEN_DIR})
    target_link_libraries(stream_protocol_test PRIVATE protobuf::libprotobuf)
    enable_testing()
    add_test(NAME stream_protocol_test COMMAND stream_protocol_test)

    add_executable(diag_test
        tests/diag_test.cpp
        vkpcnx/src/core/diag/redact.cpp
        vkpcnx/src/core/diag/zip_writer.cpp)
    set_target_properties(diag_test PROPERTIES CXX_STANDARD 17)
    target_include_directories(diag_test PRIVATE vkpcnx/include)
    target_link_libraries(diag_test PRIVATE ZLIB::ZLIB)
    add_test(NAME diag_test COMMAND diag_test)

    # Headless end-to-end run against tests/mock_server.py (not registered with ctest)
    file(GLOB_RECURSE E2E_SRC vkpcnx/src/core/*.cpp)
    add_executable(stream_e2e_test tests/stream_e2e_test.cpp ${E2E_SRC} ${PROTO_GEN_SRCS})
    set_target_properties(stream_e2e_test PROPERTIES CXX_STANDARD 17)
    target_include_directories(stream_e2e_test PRIVATE vkpcnx/include ${PROTO_GEN_DIR} ${VKPCNX_GEN_DIR} ${APP_PLATFORM_INCLUDE})
    target_compile_definitions(stream_e2e_test PRIVATE ${VKPCNX_BUILD_DEFINES})
    add_dependencies(stream_e2e_test vkpcnx_version)
    target_compile_options(stream_e2e_test PRIVATE ${APP_PLATFORM_OPTION})
    target_link_libraries(stream_e2e_test PRIVATE
        borealis nlohmann_json::nlohmann_json websockets vkpcnx_streaming_deps protobuf::libprotobuf ${APP_PLATFORM_LIB})
endif ()
