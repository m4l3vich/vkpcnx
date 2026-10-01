# Shim for the Switch build: libsrtp calls find_package(MbedTLS) and expects
# MBEDTLS_INCLUDE_DIRS / MBEDTLS_LIBRARIES. The real library is the
# configure-time build from cmake/switch/SwitchMbedTls.cmake.
if (TARGET MbedTLS::MbedTLS)
    set(MbedTLS_FOUND TRUE)
    set(MBEDTLS_FOUND TRUE)
    set(MBEDTLS_INCLUDE_DIRS ${SWITCH_MBEDTLS_PREFIX}/include)
    set(MBEDTLS_LIBRARIES MbedTLS::MbedTLS MbedTLS::MbedX509 MbedTLS::MbedCrypto)
    set(MbedTLS_VERSION 3.6)
    if (NOT TARGET MbedTLS::MbedCrypto)
        message(FATAL_ERROR "MbedTLS::MbedCrypto missing")
    endif ()
else ()
    set(MbedTLS_FOUND FALSE)
    if (MbedTLS_FIND_REQUIRED)
        message(FATAL_ERROR "MbedTLS targets are not defined (Switch build only)")
    endif ()
endif ()
