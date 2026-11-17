# Build the native server directly on macOS with Homebrew LLVM and the NC4 SDK.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(WEBOS_SDK_ROOT "$ENV{WEBOS_SDK_ROOT}" CACHE PATH "Extracted Buildroot NC4 SDK")
if(NOT IS_DIRECTORY "${WEBOS_SDK_ROOT}/arm-webos-linux-gnueabi/sysroot")
    message(FATAL_ERROR "Set WEBOS_SDK_ROOT to the extracted NC4 2026.08 SDK")
endif()
find_program(WEBOS_CLANG clang HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin REQUIRED)
find_program(
    WEBOS_CLANGXX
    clang++
    HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin
    REQUIRED
)
find_program(WEBOS_LLD ld.lld HINTS /opt/homebrew/opt/lld/bin /usr/local/opt/lld/bin REQUIRED)
find_program(CMAKE_AR llvm-ar HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin REQUIRED)
find_program(
    CMAKE_RANLIB
    llvm-ranlib
    HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin
    REQUIRED
)
set(CMAKE_C_COMPILER "${WEBOS_CLANG}")
set(CMAKE_CXX_COMPILER "${WEBOS_CLANGXX}")
set(CMAKE_C_COMPILER_TARGET arm-webos-linux-gnueabi)
set(CMAKE_CXX_COMPILER_TARGET arm-webos-linux-gnueabi)
set(CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN "${WEBOS_SDK_ROOT}")
set(CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN "${WEBOS_SDK_ROOT}")
set(CMAKE_SYSROOT "${WEBOS_SDK_ROOT}/arm-webos-linux-gnueabi/sysroot")
set(CMAKE_C_FLAGS_INIT
    "-march=armv7-a -mfpu=neon -mfloat-abi=softfp -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64"
)
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "--ld-path=${WEBOS_LLD}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT}")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(OPENSSL_ROOT_DIR "${CMAKE_SYSROOT}/usr" CACHE PATH "Target OpenSSL")
