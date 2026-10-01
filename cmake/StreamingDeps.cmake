# Third-party dependencies of the streaming client:
#   libdatachannel (WebRTC: ICE/DTLS/SRTP/SCTP) + its bundled libjuice/usrsctp/libsrtp/plog
#   zlib   (input protocol, cursor/clipboard payloads)
#   opus   (audio decode)
#   FFmpeg (H.264 decode; libavcodec/libavutil)
#   miniaudio (desktop audio output; Switch uses audout from libnx)
#
# Produces the INTERFACE target `vkpcnx_streaming_deps` carrying everything
# the app needs to link.

include(FetchContent)
find_package(PkgConfig REQUIRED)

add_library(vkpcnx_streaming_deps INTERFACE)

# ---- zlib --------------------------------------------------------------------
if (PLATFORM_SWITCH)
    # switch-zlib portlib is already in APP_PLATFORM_LIB (curl dependency)
    target_link_libraries(vkpcnx_streaming_deps INTERFACE z)
else ()
    find_package(ZLIB REQUIRED)
    target_link_libraries(vkpcnx_streaming_deps INTERFACE ZLIB::ZLIB)
endif ()

# ---- opus --------------------------------------------------------------------
if (PLATFORM_SWITCH)
    # pacman -S switch-libopus
    pkg_check_modules(OPUS REQUIRED opus)
    target_include_directories(vkpcnx_streaming_deps INTERFACE ${OPUS_STATIC_INCLUDE_DIRS})
    target_link_libraries(vkpcnx_streaming_deps INTERFACE ${OPUS_STATIC_LDFLAGS})
else ()
    pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
    target_link_libraries(vkpcnx_streaming_deps INTERFACE PkgConfig::OPUS)
endif ()

# ---- FFmpeg ------------------------------------------------------------------
if (PLATFORM_SWITCH)
    # pacman -S switch-ffmpeg (7.1, built with the nvtegra hwaccel). The static
    # libavcodec pulls in every codec library it was built with, so take the
    # full static link line from pkg-config instead of guessing.
    # (the toolchain's aarch64-none-elf-pkg-config already forces --static)
    pkg_check_modules(FFMPEG REQUIRED libavcodec libavutil libswresample)
    target_include_directories(vkpcnx_streaming_deps INTERFACE ${FFMPEG_STATIC_INCLUDE_DIRS})
    target_link_libraries(vkpcnx_streaming_deps INTERFACE ${FFMPEG_STATIC_LDFLAGS})
else ()
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswresample)
    target_link_libraries(vkpcnx_streaming_deps INTERFACE PkgConfig::FFMPEG)
endif ()

# ---- miniaudio (desktop only) -----------------------------------------------
if (NOT PLATFORM_SWITCH)
    # We only use miniaudio.h/miniaudio.c directly; skip its libvorbis/libopus
    # decoder extras. Left on, miniaudio's CMakeLists find_library()s a system
    # vorbisfile/opusfile without adding the matching include dir, which can
    # fail the build if one happens to be installed (e.g. via a Homebrew dep).
    set(MINIAUDIO_NO_LIBVORBIS ON CACHE BOOL "" FORCE)
    set(MINIAUDIO_NO_LIBOPUS ON CACHE BOOL "" FORCE)
    FetchContent_Declare(miniaudio
        GIT_REPOSITORY https://github.com/mackron/miniaudio.git
        GIT_TAG 0.11.22
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(miniaudio)
    target_include_directories(vkpcnx_streaming_deps INTERFACE ${miniaudio_SOURCE_DIR})
    if (APPLE)
        target_link_libraries(vkpcnx_streaming_deps INTERFACE
            "-framework CoreAudio" "-framework AudioToolbox" "-framework AudioUnit")
    elseif (UNIX)
        target_link_libraries(vkpcnx_streaming_deps INTERFACE dl pthread m)
    endif ()
endif ()

# ---- libdatachannel ----------------------------------------------------------
# Built from source everywhere (no portlib exists). The Switch port lives in
# cmake/patches/libdatachannel-*.patch; every hunk is guarded by __SWITCH__ so
# applying them on desktop is a no-op.
set(LIBDATACHANNEL_PATCH_DIR ${CMAKE_CURRENT_LIST_DIR}/patches/libdatachannel)

FetchContent_Declare(libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.24.5
    GIT_SHALLOW TRUE
    GIT_SUBMODULES deps/plog deps/usrsctp deps/libjuice deps/libsrtp
    GIT_SUBMODULES_RECURSE FALSE
    PATCH_COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=<SOURCE_DIR>
        -DPATCH_DIR=${LIBDATACHANNEL_PATCH_DIR}
        -P ${CMAKE_CURRENT_LIST_DIR}/ApplyPatches.cmake
    UPDATE_DISCONNECTED TRUE)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)   # signalling uses libwebsockets
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
set(NO_MEDIA OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_JSON OFF CACHE BOOL "" FORCE)
set(USE_NICE OFF CACHE BOOL "" FORCE)
set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_WARNINGS OFF CACHE BOOL "" FORCE) # libsrtp -Werror

if (PLATFORM_SWITCH)
    # TLS comes from the mbedTLS 3.6 build in cmake/switch/SwitchMbedTls.cmake,
    # which must have been included before this file (it defines MbedTLS::MbedTLS).
    if (NOT TARGET MbedTLS::MbedTLS)
        message(FATAL_ERROR "SwitchMbedTls.cmake must be included before StreamingDeps.cmake")
    endif ()
    set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
    # libsrtp's own FindMbedTLS.cmake would look for a system install; the shim in
    # cmake/Modules maps it onto the same targets instead.
    list(PREPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/Modules)
else ()
    set(USE_MBEDTLS OFF CACHE BOOL "" FORCE)
    if (APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
        # Homebrew keeps OpenSSL out of the default search path
        execute_process(COMMAND brew --prefix openssl@3
            OUTPUT_VARIABLE BREW_OPENSSL OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if (BREW_OPENSSL)
            set(OPENSSL_ROOT_DIR ${BREW_OPENSSL} CACHE PATH "" FORCE)
        endif ()
    endif ()
endif ()

FetchContent_MakeAvailable(libdatachannel)

target_link_libraries(vkpcnx_streaming_deps INTERFACE LibDataChannel::LibDataChannelStatic)
