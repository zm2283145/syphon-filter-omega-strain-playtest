# Port-only (132ps5c, research/PS5.md "Build"): the PS5 homebrew toolchain (ps5-payload-sdk v0.42, ps5-opengl SDK
# 1.0.1) for WSL. tools/build_ps5.ps1 configures with it:
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/ps5/ps5.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
# The objects are static libraries; tools/ps5/mkapp.sh links them into the folder title (the omega_ps5_package target).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(OMEGA_PS5 ON CACHE BOOL "Build the PS5 target (port-only, research/PS5.md)" FORCE)

get_filename_component(OMEGA_PS5_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CMAKE_CXX_COMPILER "${OMEGA_PS5_ROOT}/tools/ps5/ps5-clang++.sh")
find_program(OMEGA_PS5_AR NAMES llvm-ar-18 llvm-ar REQUIRED)
find_program(OMEGA_PS5_RANLIB NAMES llvm-ranlib-18 llvm-ranlib REQUIRED)
set(CMAKE_AR "${OMEGA_PS5_AR}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${OMEGA_PS5_RANLIB}" CACHE FILEPATH "" FORCE)
# The compiler cannot link a program without the title's runtime (mkapp.sh does that): checks compile only.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(BUILD_TESTING OFF CACHE BOOL "The core tests run on the PC and the Vita" FORCE)

set(PS5_NATIVE_APP_TEMPLATE "$ENV{HOME}/ps5/bp" CACHE PATH "ps5-native-app-boilerplate (with .deps prepared)")
set(PS5_OPENGL_PREFIX "$ENV{HOME}/ps5/ps5-opengl-sdk-1.0.1/sdk" CACHE PATH "The ps5-opengl SDK prefix")
set(PS5_OPENGL_SRC "$ENV{HOME}/ps5/gl/ps5-opengl" CACHE PATH "The ps5-opengl source tree (native-app files)")