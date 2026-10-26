# Port-only (132ps5c, research/PS5.md): the PS5 build's compiler settings, included from CMakeLists.txt before any
# target when the PS5 toolchain (cmake/ps5/ps5.toolchain.cmake) is in use. The PS5 is its own target: OMEGA_PS5 is
# defined and __vita__ is not (src/port/ps5 is its platform layer, src/port/gfx/gfx_gl46_ps5.cpp its renderer).
#
# Floating point: x86-64 SSE2 scalar code as the PC's MSVC build (no -march: no FMA or AVX), -ffp-contract=off on
# every translation unit (the PC compiles the deterministic files /fp:strict), no fast-math.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
add_compile_definitions(OMEGA_PS5=1)
add_compile_options(-ffp-contract=off -fexceptions -frtti -fPIC -Wno-unknown-attributes
    "SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/src/port/ps5/ps5_compat.hpp\"")

# The host tools (Windows console programs) are not built for the PS5.
set(OMEGA_PS5_HOST_ONLY_TARGETS omega_collision omega_actor_physics omega_character omega_animation omega_server)