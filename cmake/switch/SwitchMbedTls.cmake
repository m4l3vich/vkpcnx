# mbedTLS 3.6 for the Switch, built at *configure time* so that both
# libwebsockets (feature checks) and libdatachannel (find_package) see a
# real install. The switch-mbedtls portlib is 2.28 and lacks DTLS-SRTP, and
# curl on Switch uses the libnx SSL service, so nothing else links mbedTLS.
#
# Defines the imported targets MbedTLS::MbedTLS / MbedTLS::MbedX509 /
# MbedTLS::MbedCrypto and SWITCH_MBEDTLS_PREFIX.

set(SWITCH_MBEDTLS_VERSION 3.6.4)
set(SWITCH_MBEDTLS_URL
    https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${SWITCH_MBEDTLS_VERSION}/mbedtls-${SWITCH_MBEDTLS_VERSION}.tar.bz2)

set(SWITCH_MBEDTLS_ROOT ${CMAKE_BINARY_DIR}/mbedtls-switch)
set(SWITCH_MBEDTLS_PREFIX ${SWITCH_MBEDTLS_ROOT}/prefix)
set(SWITCH_MBEDTLS_SRC ${SWITCH_MBEDTLS_ROOT}/src)
set(SWITCH_MBEDTLS_BUILD ${SWITCH_MBEDTLS_ROOT}/build)
set(SWITCH_MBEDTLS_STAMP ${SWITCH_MBEDTLS_ROOT}/installed-${SWITCH_MBEDTLS_VERSION}.stamp)
set(SWITCH_MBEDTLS_USER_CONFIG ${CMAKE_CURRENT_LIST_DIR}/mbedtls_user_config.h)

if (NOT EXISTS ${SWITCH_MBEDTLS_STAMP} OR ${SWITCH_MBEDTLS_USER_CONFIG} IS_NEWER_THAN ${SWITCH_MBEDTLS_STAMP})
    message(STATUS "Building mbedTLS ${SWITCH_MBEDTLS_VERSION} for Switch (one-time, configure step)")
    file(MAKE_DIRECTORY ${SWITCH_MBEDTLS_ROOT})

    set(TARBALL ${SWITCH_MBEDTLS_ROOT}/mbedtls-${SWITCH_MBEDTLS_VERSION}.tar.bz2)
    if (NOT EXISTS ${TARBALL})
        file(DOWNLOAD ${SWITCH_MBEDTLS_URL} ${TARBALL} STATUS DL_STATUS SHOW_PROGRESS)
        list(GET DL_STATUS 0 DL_CODE)
        if (NOT DL_CODE EQUAL 0)
            file(REMOVE ${TARBALL})
            message(FATAL_ERROR "mbedTLS download failed: ${DL_STATUS}")
        endif ()
    endif ()
    if (NOT EXISTS ${SWITCH_MBEDTLS_SRC}/CMakeLists.txt)
        file(REMOVE_RECURSE ${SWITCH_MBEDTLS_SRC})
        file(ARCHIVE_EXTRACT INPUT ${TARBALL} DESTINATION ${SWITCH_MBEDTLS_ROOT})
        file(RENAME ${SWITCH_MBEDTLS_ROOT}/mbedtls-${SWITCH_MBEDTLS_VERSION} ${SWITCH_MBEDTLS_SRC})
    endif ()

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${SWITCH_MBEDTLS_SRC} -B ${SWITCH_MBEDTLS_BUILD}
            -DCMAKE_TOOLCHAIN_FILE=${DEVKITPRO}/cmake/Switch.cmake
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX=${SWITCH_MBEDTLS_PREFIX}
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5
            -DMBEDTLS_USER_CONFIG_FILE=${SWITCH_MBEDTLS_USER_CONFIG}
            -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF
            -DGEN_FILES=OFF
            -DMBEDTLS_FATAL_WARNINGS=OFF
            -DUSE_STATIC_MBEDTLS_LIBRARY=ON -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
            # explicit CMAKE_C_FLAGS overrides the platform's *_INIT flags, so repeat them
            "-DCMAKE_C_FLAGS=-I${DEVKITPRO}/libnx/include ${NX_TARGET_FLAGS} -fzero-init-padding-bits=unions"
        RESULT_VARIABLE RC)
    if (NOT RC EQUAL 0)
        message(FATAL_ERROR "mbedTLS configure failed")
    endif ()
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${SWITCH_MBEDTLS_BUILD} --parallel --target install
        RESULT_VARIABLE RC)
    if (NOT RC EQUAL 0)
        message(FATAL_ERROR "mbedTLS build failed")
    endif ()
    file(TOUCH ${SWITCH_MBEDTLS_STAMP})
endif ()

# Every consumer must compile against the same configuration, or struct
# layouts (DTLS-SRTP, threading) diverge from the library. Applies to all
# targets defined after this point, including FetchContent sub-projects.
add_compile_definitions(MBEDTLS_USER_CONFIG_FILE="${SWITCH_MBEDTLS_USER_CONFIG}")

# The portlibs include dir (from borealis' toolchain.cmake) is first on every
# compile line and contains the 2.28 mbedTLS headers; put ours in front so
# libwebsockets/libdatachannel see the 3.6 API.
set(CMAKE_C_FLAGS "-I${SWITCH_MBEDTLS_PREFIX}/include ${CMAKE_C_FLAGS}")
set(CMAKE_CXX_FLAGS "-I${SWITCH_MBEDTLS_PREFIX}/include ${CMAKE_CXX_FLAGS}")

foreach (COMPONENT MbedTLS MbedX509 MbedCrypto)
    string(TOLOWER ${COMPONENT} LIBNAME)
    add_library(MbedTLS::${COMPONENT} STATIC IMPORTED GLOBAL)
    set_target_properties(MbedTLS::${COMPONENT} PROPERTIES
        IMPORTED_LOCATION ${SWITCH_MBEDTLS_PREFIX}/lib/lib${LIBNAME}.a
        INTERFACE_INCLUDE_DIRECTORIES ${SWITCH_MBEDTLS_PREFIX}/include
        # keep it a plain -I: gcc drops a -I that is also given as -isystem, and
        # the -isystem form would lose to the portlibs -I (mbedTLS 2.28) above
        IMPORTED_NO_SYSTEM TRUE
        INTERFACE_COMPILE_DEFINITIONS "MBEDTLS_USER_CONFIG_FILE=\"${SWITCH_MBEDTLS_USER_CONFIG}\"")
endforeach ()
set_target_properties(MbedTLS::MbedTLS PROPERTIES INTERFACE_LINK_LIBRARIES "MbedTLS::MbedX509;MbedTLS::MbedCrypto")
set_target_properties(MbedTLS::MbedX509 PROPERTIES INTERFACE_LINK_LIBRARIES "MbedTLS::MbedCrypto")

# Let find_package()/find_library() in sub-projects (libsrtp, libdatachannel) see it
list(APPEND CMAKE_FIND_ROOT_PATH ${SWITCH_MBEDTLS_PREFIX})
list(APPEND CMAKE_PREFIX_PATH ${SWITCH_MBEDTLS_PREFIX})
set(MbedTLS_ROOT ${SWITCH_MBEDTLS_PREFIX})
set(MBEDTLS_INCLUDE_DIRS ${SWITCH_MBEDTLS_PREFIX}/include)
set(MBEDTLS_LIBRARIES MbedTLS::MbedTLS MbedTLS::MbedX509 MbedTLS::MbedCrypto)
