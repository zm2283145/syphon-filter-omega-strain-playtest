# Port-only (checkpoint 132vt1, research/VITA.md "Toolchain"): the PS Vita build's compiler settings.
# Included from CMakeLists.txt before any target when the VitaSDK toolchain is in use
# (-DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake; tools/build_vita.ps1 configures it).
#
# The toolchain's defaults are -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard -mtune=cortex-a9 -mthumb.
# Floating point: every translation unit gets -ffp-contract=off (the files MSVC builds /fp:strict get it from
# CMakeLists.txt already; the Cortex-A9's VFPv3 has no fused multiply-add, so this only keeps GCC from ever
# contracting). Without -ffast-math / -funsafe-math-optimizations GCC never vectorises float arithmetic onto NEON
# (NEON flushes denormals and has no IEEE rounding modes), so float code stays scalar IEEE VFP as on x64 SSE.
# research/VITA.md lists the remaining FP differences (host libm, float to int out of range).

set(OMEGA_VITA_TITLE_ID "SFOS00001" CACHE STRING "The Vita build's title ID (homebrew range, port-only)")
set(OMEGA_VITA_VERSION "01.00" CACHE STRING "The .vpk's APP_VER")

# -O2: the -O3 default of the toolchain more than doubles the build time of the 400-file core for no measured gain.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

# newlib declares the POSIX 2008 names (strnlen, ...) only with a feature macro under -std=c++20.
add_compile_definitions(_DEFAULT_SOURCE)
add_compile_options(-ffp-contract=off
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/port/vita/vita_compat.hpp")

# The build's host tools are Windows/console programs: on the Vita they are not built by default.
set(OMEGA_VITA_HOST_ONLY_TARGETS omega_collision omega_actor_physics omega_character omega_animation omega_server)
