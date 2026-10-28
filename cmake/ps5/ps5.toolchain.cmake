# Port-only (132ps5c, 132ps5d; research/PS5.md "Build"): the PS5 homebrew toolchain, from public pinned sources that
# tools/ps5/fetch_toolchain.sh fetches into one prefix (PS5_TOOLCHAIN, default ~/ps5-app-toolchain):
# ps5-payload-sdk v0.45, cybervisi0n/ps5-app-sdk (crt, linker script, symbol map, ps5-native-tool, libc-prx-builder) and
# the ps5-mesa / ps5-opengnm-psbc / ps5-gl-runtime releases (OpenGL over AGC). tools/build_ps5.ps1 (WSL) and CI
# configure with it:
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/ps5/ps5.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
# The compiler is clang 18 for the x86_64-sie-ps5 target with the SDK's headers and libc++ (the flags of the SDK's
# prospero-clang, without its payload link defaults); cmake/ps5/OmegaPs5Targets.cmake links the folder title.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(OMEGA_PS5 ON CACHE BOOL "Build the PS5 target (port-only, research/PS5.md)" FORCE)

if(DEFINED ENV{PS5_TOOLCHAIN} AND NOT "$ENV{PS5_TOOLCHAIN}" STREQUAL "")
    set(OMEGA_PS5_TOOLCHAIN_DEFAULT "$ENV{PS5_TOOLCHAIN}")
else()
    set(OMEGA_PS5_TOOLCHAIN_DEFAULT "$ENV{HOME}/ps5-app-toolchain")
endif()
set(PS5_TOOLCHAIN "${OMEGA_PS5_TOOLCHAIN_DEFAULT}" CACHE PATH "The PS5 toolchain prefix (tools/ps5/fetch_toolchain.sh)")
if(NOT EXISTS "${PS5_TOOLCHAIN}/.fetched")
    message(FATAL_ERROR "No PS5 toolchain in ${PS5_TOOLCHAIN}: run tools/ps5/fetch_toolchain.sh ${PS5_TOOLCHAIN}")
endif()
set(PS5_PAYLOAD_SDK "${PS5_TOOLCHAIN}/ps5-payload-sdk")
set(PS5_APP_SDK "${PS5_TOOLCHAIN}/ps5-app-sdk")
set(PS5_GL_PREFIX "${PS5_TOOLCHAIN}/deps")
set(PS5_HOST_TOOLS "${PS5_TOOLCHAIN}/host/bin")

# clang 18, as every hardware-tested build (132ps5a's self-test, 132ps5c).
find_program(OMEGA_PS5_CLANG NAMES clang-18 REQUIRED)
find_program(OMEGA_PS5_LLVM_CONFIG NAMES llvm-config-18 REQUIRED)
find_program(OMEGA_PS5_AR NAMES llvm-ar-18 llvm-ar REQUIRED)
find_program(OMEGA_PS5_RANLIB NAMES llvm-ranlib-18 llvm-ranlib REQUIRED)
set(CMAKE_C_COMPILER "${OMEGA_PS5_CLANG}")
set(CMAKE_CXX_COMPILER "${OMEGA_PS5_CLANG}")
set(CMAKE_AR "${OMEGA_PS5_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${OMEGA_PS5_RANLIB}" CACHE FILEPATH "" FORCE)
set(OMEGA_PS5_TARGET_FLAGS "-target x86_64-sie-ps5 -fvisibility-nodllstorageclass=default -isysroot ${PS5_PAYLOAD_SDK} -fno-stack-protector -fno-plt -femulated-tls")
set(CMAKE_C_FLAGS_INIT "${OMEGA_PS5_TARGET_FLAGS} -isystem ${PS5_PAYLOAD_SDK}/target/include")
set(CMAKE_CXX_FLAGS_INIT "${OMEGA_PS5_TARGET_FLAGS} -isystem ${PS5_PAYLOAD_SDK}/target/include/c++/v1 -isystem ${PS5_PAYLOAD_SDK}/target/include")
# The compiler cannot link a program without the title's runtime (OmegaPs5Targets.cmake does that): checks compile only.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PS5_TOOLCHAIN)
set(BUILD_TESTING OFF CACHE BOOL "The core tests run on the PC and the Vita" FORCE)
