# Port-only (132ps5c, research/PS5.md): the PS5 target's own libraries and the folder title. Included at the end of
# CMakeLists.txt when the PS5 toolchain is in use (tools/build_ps5.ps1).
#
#   omega_ps5_platform  the platform layer (src/port/ps5: log, sceAudioOut, scePad, clocks, the disc check, the crash
#                       reporter, the fixed-screen host window) in place of the Windows files of the PC build
#   omega_gfx_gl46      gfx::Device on OpenGL 4.6 (src/port/gfx/gfx_gl46_ps5.cpp) with the shared Cg sources and the
#                       backend-neutral render thread
#   omega_ps5_app       PPSA99720: the PS5 app (src/port/ps5/ps5_main.cpp) and the PC play host's sources
#   omega_ps5_package   links them into the folder title (tools/ps5/mkapp.sh): <build>/app/dist/PPSA99720
foreach(host_target IN LISTS OMEGA_PS5_HOST_ONLY_TARGETS)
    if(TARGET ${host_target})
        set_target_properties(${host_target} PROPERTIES EXCLUDE_FROM_ALL ON)
    endif()
endforeach()

add_library(omega_ps5_platform STATIC
    src/port/ps5/ps5_log.cpp
    src/port/ps5/ps5_libc_extras.cpp
    src/port/ps5/ps5_cwd.cpp
    src/port/ps5/ps5_imports.cpp
    src/port/ps5/audio_device_ps5.cpp
    src/port/ps5/gamepad_ps5.cpp
    src/port/ps5/frame_pacer_ps5.cpp
    src/port/ps5/iso_verify_ps5.cpp
    src/port/ps5/crash_report_ps5.cpp
    src/port/ps5/pacing_probe_ps5.cpp
    src/port/ps5/host_window_ps5.cpp)
target_link_libraries(omega_ps5_platform PUBLIC omega_core)
# ps5_imports.cpp declares the imports as bytes and must not see their C library declarations.
set_source_files_properties(src/port/ps5/ps5_imports.cpp PROPERTIES COMPILE_DEFINITIONS OMEGA_PS5_NO_COMPAT=1)

add_library(omega_gfx_gl46 STATIC
    src/port/gfx/gfx_gl46_ps5.cpp
    src/port/gfx/gfx_gxm_shaders.cpp
    src/port/gfx/gfx_render_thread.cpp)
target_include_directories(omega_gfx_gl46 PRIVATE "${PS5_OPENGL_PREFIX}/include")
target_compile_definitions(omega_gfx_gl46 PRIVATE GL_GLEXT_PROTOTYPES=1 EGL_EGLEXT_PROTOTYPES=1)
target_link_libraries(omega_gfx_gl46 PUBLIC omega_core omega_ps5_platform)

# The PC play host (CMakeLists.txt's omega_play_objects) without its Windows-only files (host_window.cpp,
# audio_device.cpp, gamepad_device.cpp, pacing_probe.cpp, launcher_mod_panel.cpp, the D3D11 backend), which the PS5
# platform layer and the GL 4.6 backend replace.
add_library(omega_ps5_app STATIC src/port/ps5/ps5_main.cpp
    src/port/app/play_main.cpp src/port/app/launcher.cpp src/port/app/frontend_app.cpp src/port/app/frontend_disc.cpp
    src/port/app/agent_preview.cpp src/port/app/loading_screen_host.cpp src/port/render/scene_renderer.cpp
    src/port/render/gui_renderer.cpp src/port/render/particle_renderer.cpp src/port/render/corona_renderer.cpp
    src/port/render/frame_overlay.cpp src/port/platform/crash_report.cpp src/port/platform/gamepad_simulation.cpp
    src/port/disc/derived_data.cpp)
target_link_libraries(omega_ps5_app PUBLIC omega_gfx_gl46 omega_ps5_platform omega_core)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config/disc_us.json" OMEGA_PS5_DISC_PROFILE)
string(JSON OMEGA_PS5_ISO_SIZE GET "${OMEGA_PS5_DISC_PROFILE}" iso_size)
string(JSON OMEGA_PS5_ISO_SHA256 GET "${OMEGA_PS5_DISC_PROFILE}" iso_sha256)
string(JSON OMEGA_PS5_EXE_SIZE GET "${OMEGA_PS5_DISC_PROFILE}" executable_size)
string(JSON OMEGA_PS5_EXE_SHA256 GET "${OMEGA_PS5_DISC_PROFILE}" executable_sha256)
string(JSON OMEGA_PS5_BOOT_EXECUTABLE GET "${OMEGA_PS5_DISC_PROFILE}" boot_executable)
target_compile_definitions(omega_ps5_app PRIVATE OMEGA_ISO_SIZE=${OMEGA_PS5_ISO_SIZE}ULL
    OMEGA_ISO_SHA256="${OMEGA_PS5_ISO_SHA256}" OMEGA_EXE_SIZE=${OMEGA_PS5_EXE_SIZE}ULL
    OMEGA_EXE_SHA256="${OMEGA_PS5_EXE_SHA256}" OMEGA_BOOT_EXECUTABLE="${OMEGA_PS5_BOOT_EXECUTABLE}")
# play_main.cpp is about 1.7 MB of source: -O1 keeps its compile time and memory bounded (as on the Vita).
set_source_files_properties(src/port/app/play_main.cpp PROPERTIES COMPILE_OPTIONS "-O1")

set(OMEGA_PS5_SDK "${PS5_NATIVE_APP_TEMPLATE}/.deps/native/ps5-payload-sdk")
set(OMEGA_PS5_EXTRA_LIBS "${OMEGA_PS5_SDK}/target/lib/libScePad.so ${OMEGA_PS5_SDK}/target/lib/libSceUserService.so ${OMEGA_PS5_SDK}/target/lib/libSceAudioOut.so ${OMEGA_PS5_SDK}/target/lib/libSceSystemService.so")
set(OMEGA_PS5_ASSETS "${CMAKE_CURRENT_BINARY_DIR}/ps5_assets")
# The C library calls that take a path, wrapped by src/port/ps5/ps5_cwd.cpp (the title's own working folder).
set(OMEGA_PS5_PATH_WRAPS "chdir getcwd realpath access fchmodat fopen freopen link lstat mkdir mkstemp open openat opendir fdopendir readdir closedir __cxa_throw pathconf readlink remove rename stat statvfs symlink truncate unlink unlinkat utimes")
add_custom_target(omega_ps5_package
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${OMEGA_PS5_ASSETS}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OMEGA_PS5_ASSETS}/config"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/config/disc_us.json" "${OMEGA_PS5_ASSETS}/config/disc_us.json"
    COMMAND ${CMAKE_COMMAND} -E env "PS5_NATIVE_APP_TEMPLATE=${PS5_NATIVE_APP_TEMPLATE}" "PS5_PAYLOAD_SDK=${OMEGA_PS5_SDK}"
        "PS5_OPENGL_PREFIX=${PS5_OPENGL_PREFIX}" "PS5_OPENGL_SRC=${PS5_OPENGL_SRC}" "PS5_APP_EXTRA_LIBS=${OMEGA_PS5_EXTRA_LIBS}"
        "PS5_APP_CONTENT_SUFFIX=OMEGASTRAIN00001" "PS5_APP_ASSETS=${OMEGA_PS5_ASSETS}"
        "PS5_APP_WRAP=${OMEGA_PS5_PATH_WRAPS}"
        bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/ps5/mkapp.sh" "${CMAKE_CURRENT_BINARY_DIR}/app" PPSA99720
        "Syphon Filter: The Omega Strain"
        $<TARGET_FILE:omega_ps5_app> $<TARGET_FILE:omega_gfx_gl46> $<TARGET_FILE:omega_ps5_platform>
        $<TARGET_FILE:omega_core> $<TARGET_FILE:omega_online>
    DEPENDS omega_ps5_app omega_gfx_gl46 omega_ps5_platform omega_core omega_online
    COMMENT "Packaging the PS5 folder title PPSA99720"
    VERBATIM)