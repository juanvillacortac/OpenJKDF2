set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_AR aarch64-linux-gnu-ar)
set(CMAKE_RANLIB aarch64-linux-gnu-ranlib)

# Do not set CMAKE_SYSROOT: Ubuntu/Debian cross linkers break when sysroot is
# /usr/aarch64-linux-gnu (ld looks for $SYSROOT/usr/aarch64-linux-gnu/lib/...).
# The compiler still resolves aarch64 libs/headers via CMAKE_FIND_ROOT_PATH.
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu /usr/aarch64-openssl)
if(EXISTS "/usr/aarch64-openssl/lib/libcrypto.so")
    set(OPENSSL_ROOT_DIR /usr/aarch64-openssl CACHE PATH "aarch64 OpenSSL for GNS cross-build")
endif()

# Evitar headers/librerías del host x86_64 durante el cross-compile
set(CMAKE_IGNORE_PATH "/usr/include" "/usr/lib" "/usr/local/include" "/usr/local/lib")

set(ENV{PKG_CONFIG_LIBDIR} "/usr/aarch64-linux-gnu/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

message(STATUS "Linux aarch64 cross-compile toolchain invoked")
