# Port-only (checkpoint 132vt1, research/VITA.md "Build"): the PS Vita build's own targets. Included at the end of
# CMakeLists.txt when the VitaSDK toolchain is in use (tools/build_vita.ps1 configures and builds it).
#
#   omega_vita_platform  the platform layer (src/port/vita: SceIo files, SceAudioOut, SceCtrl / SceTouch,
#                        clocks, the ISO check) in place of the Windows files of omega_windows / the app targets
#   omega_gfx_gxm        gfx::Device on sceGxm (src/port/gfx/gfx_gxm*.cpp, research/RENDERER.md "GXM backend") with
#                        the backend-neutral render thread
#   omega_vita           SFOS00001: the Vita app (src/port/app/vita_main.cpp) -> omega_vita.vpk
#   omega_core_tests     SFOS00002: the shared core's unit tests on the handheld (tests/vita_test_runner.cpp)
#                        -> omega_core_tests.vpk, the evidence that the deterministic paths match the x64 build
#
# The LiveArea images are drawn by tools/vita_livearea.py at configure time (port-made, nothing disc-derived).

include("${VITASDK}/share/vita.cmake")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

foreach(host_target IN LISTS OMEGA_VITA_HOST_ONLY_TARGETS)
    if(TARGET ${host_target})
        set_target_properties(${host_target} PROPERTIES EXCLUDE_FROM_ALL ON)
    endif()
endforeach()

add_library(omega_vita_platform STATIC
    src/port/vita/vita_file.cpp
    src/port/vita/audio_device_vita.cpp
    src/port/vita/frame_pacer_vita.cpp
    src/port/vita/gamepad_vita.cpp
    src/port/vita/iso_verify_vita.cpp)
target_include_directories(omega_vita_platform PUBLIC src/port/vita)
# std::thread (the worker pool, the render thread, audio) is newlib's pthread over SceKernel threads. libstdc++ only
# treats threads as active when pthread is linked whole (else std::thread throws "Enable multithreading to use
# std::thread"): omega_vita_link_threads on each executable.
target_link_libraries(omega_core PUBLIC pthread)
# The coarse OMEGA_ZONE timers (src/port/platform/zone_profile.hpp; research/VITA.md "Profiling") in the handheld
# build: the app logs them every 120 frames.
option(OMEGA_VITA_ZONES "Vita: compile the shared code's OMEGA_ZONE timers" ON)
if(OMEGA_VITA_ZONES)
    target_compile_definitions(omega_core PUBLIC OMEGA_ZONE_PROFILE=1)
endif()
# 132vt2f: the GL backend's per-draw zones (OMEGA_ZONE_FINE) cost about 130 ms a level frame on the handheld in clock
# reads; off unless asked for.
option(OMEGA_VITA_FINE_ZONES "Vita: also compile the GL backend's per-draw OMEGA_ZONE_FINE timers" OFF)
if(OMEGA_VITA_ZONES AND OMEGA_VITA_FINE_ZONES)
    target_compile_definitions(omega_core PUBLIC OMEGA_ZONE_PROFILE_FINE=1)
endif()
# __sce_headroom (VitaSDK's armvita.x): vita-elf-create appends the SCE module info (imports/exports; 0xe1c bytes for omega_vita) to
# the end of the text segment, but the RW segment starts at the next 64 KiB boundary; when the text happens to end just
# below one (0x3a0 bytes short at bbeac37f) the appended data overlaps the RW segment and vita-elf-create corrupts its
# heap (0xC0000374). 64 KiB of address-space headroom (no file or memory cost) keeps the gap for any text size.
function(omega_vita_link_threads target)
    target_link_options(${target} PRIVATE "-Wl,--whole-archive" "-lpthread" "-Wl,--no-whole-archive"
        "-Wl,--defsym,__sce_headroom=0x10000")
endfunction()
target_link_libraries(omega_vita_platform PUBLIC omega_core SceAudio_stub SceCtrl_stub SceTouch_stub SceDisplay_stub
    SceLibKernel_stub SceKernelThreadMgr_stub SceIofilemgr_stub SceSysmem_stub SceNet_stub SceNetCtl_stub SceRtc_stub
    pthread)

# The renderer behind gfx::Device (research/VITA.md "Renderer"): gxm is the designated backend (sceGxm directly);
# vitagl is the TEMPORARY bring-up backend (the same gfx_gxm.hpp interface over vitaGL's GL calls) used while the
# direct GXM backend's black output is investigated (VITA.md "The black-screen investigation").
set(OMEGA_VITA_RENDERER "vitagl" CACHE STRING "Vita renderer backend: vitagl (temporary) or gxm")
set_property(CACHE OMEGA_VITA_RENDERER PROPERTY STRINGS vitagl gxm)
if(OMEGA_VITA_RENDERER STREQUAL "gxm")
    set(omega_vita_renderer_source src/port/gfx/gfx_gxm.cpp)
    set(omega_vita_renderer_libs)
elseif(OMEGA_VITA_RENDERER STREQUAL "vitagl")
    set(omega_vita_renderer_source src/port/gfx/gfx_vitagl.cpp)
    set(omega_vita_renderer_libs vitaGL mathneon)
else()
    message(FATAL_ERROR "OMEGA_VITA_RENDERER must be vitagl or gxm (got ${OMEGA_VITA_RENDERER})")
endif()
message(STATUS "Vita renderer: ${OMEGA_VITA_RENDERER}")
add_library(omega_gfx_gxm STATIC
    ${omega_vita_renderer_source}
    src/port/gfx/gfx_gxm_shaders.cpp
    src/port/gfx/gfx_render_thread.cpp)
target_link_libraries(omega_gfx_gxm PUBLIC omega_core omega_vita_platform ${omega_vita_renderer_libs}
    vitashark SceShaccCgExt SceShaccCg_stub taihen_stub
    SceGxm_stub SceDisplay_stub SceSysmodule_stub SceCommonDialog_stub SceAppMgr_stub SceKernelDmacMgr_stub
    SceIofilemgr_stub SceLibKernel_stub m)
if(OMEGA_VITA_RENDERER STREQUAL "vitagl")
    # 132vp3 (research/VITA.md "GPU waits"): gfx_vitagl.cpp's __wrap_ functions time vitaGL's calls that can wait for
    # the GPU and size its render targets' scene slots.
    target_link_options(omega_gfx_gxm INTERFACE -Wl,--wrap=sceGxmBeginScene -Wl,--wrap=sceGxmEndScene
        -Wl,--wrap=sceGxmFinish -Wl,--wrap=sceGxmDisplayQueueAddEntry -Wl,--wrap=sceGxmNotificationWait
        -Wl,--wrap=sceGxmCreateRenderTarget -Wl,--wrap=sceGxmDestroyRenderTarget)
endif()

# Extra pairs (source, path in the package) after the LiveArea images.
# VitaSDK's vita_create_vpk builds its vita-pack-vpk flags as one string and splits it on spaces, which breaks on a
# source or build path with spaces (the repo's own checkout folder): the same two commands (param.sfo, then the vpk)
# and the same outputs (<target>.vpk_param.sfo, <target>.vpk.out copied to <target>.vpk) as list arguments instead.
function(omega_vita_package target title_id name livearea)
    vita_create_self(${target}.self ${target} UNSAFE)
    set(bin "${CMAKE_CURRENT_BINARY_DIR}")
    set(self "${bin}/${target}.self")
    set(sfo "${bin}/${target}.vpk_param.sfo")
    set(out "${bin}/${target}.vpk.out")
    set(pairs "${livearea}/icon0.png" sce_sys/icon0.png
              "${livearea}/bg.png" sce_sys/livearea/contents/bg.png
              "${livearea}/startup.png" sce_sys/livearea/contents/startup.png
              "${livearea}/template.xml" sce_sys/livearea/contents/template.xml
              ${ARGN})
    set(pack_args)
    set(resources)
    while(pairs)
        list(POP_FRONT pairs source dest)
        get_filename_component(source "${source}" ABSOLUTE)
        list(APPEND resources "${source}")
        list(APPEND pack_args -a "${source}=${dest}")
    endwhile()
    add_custom_command(OUTPUT "${sfo}"
        COMMAND ${VITA_MKSFOEX} -s APP_VER=${OMEGA_VITA_VERSION} -s TITLE_ID=${title_id} "${name}" "${sfo}"
        DEPENDS "${self}"
        COMMENT "Generating param.sfo for ${target}.vpk"
        VERBATIM)
    add_custom_command(OUTPUT "${out}"
        COMMAND ${VITA_PACK_VPK} ${pack_args} -s "${sfo}" -b "${self}" "${out}"
        DEPENDS "${sfo}" "${self}" ${resources}
        COMMENT "Building vpk ${target}.vpk"
        VERBATIM)
    add_custom_target(${target}.vpk-vpk ALL
        DEPENDS "${out}"
        COMMAND ${CMAKE_COMMAND} -E copy "${out}" "${bin}/${target}.vpk"
        BYPRODUCTS "${bin}/${target}.vpk"
        VERBATIM)
endfunction()

function(omega_vita_livearea out title subtitle)
    execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/vita_livearea.py
        --out ${out} --title "${title}" --subtitle "${subtitle}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "tools/vita_livearea.py failed (${result})")
    endif()
endfunction()

# Checkpoint 132vt2 (research/VITA.md "The play host"): the PC play host (play_main.cpp and its companions, the same
# sources as CMakeLists.txt's omega_play_objects) with the handheld's stand-ins for the Windows-only pieces:
# host_window_vita (the fixed screen as the game window), crash_report_vita, pacing_probe_vita, and
# audio_device_vita / gamepad_vita (omega_vita_platform) for audio_device.cpp / gamepad_device.cpp.
add_executable(omega_vita src/port/app/vita_main.cpp src/port/render/gui_renderer.cpp src/port/app/loading_screen_host.cpp
    src/port/app/frontend_disc.cpp src/port/app/agent_preview.cpp src/port/render/frame_overlay.cpp
    src/port/app/play_main.cpp src/port/app/launcher.cpp src/port/app/frontend_app.cpp
    src/port/render/scene_renderer.cpp src/port/render/particle_renderer.cpp src/port/render/corona_renderer.cpp
    src/port/platform/crash_report.cpp src/port/platform/gamepad_simulation.cpp src/port/disc/derived_data.cpp
    src/port/vita/crash_report_vita.cpp src/port/vita/pacing_probe_vita.cpp src/port/vita/host_window_vita.cpp)
target_link_libraries(omega_vita PRIVATE omega_gfx_gxm omega_vita_platform omega_core ScePower_stub)
omega_vita_link_threads(omega_vita)
# play_main.cpp reuses a few names as both a type and a member (accepted by MSVC; GCC's -Wchanges-meaning).
# -O1 for this one file (research/VITA.md "Build"): at -O2 GCC needs more than 4 GB and over an hour for its ~1.2 MB
# translation unit; the host's hot paths live in the core library, which keeps -O2.
set_source_files_properties(src/port/app/play_main.cpp PROPERTIES COMPILE_OPTIONS "-Wno-changes-meaning;-O1")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config/disc_us.json" OMEGA_VITA_DISC_PROFILE)
string(JSON OMEGA_VITA_ISO_SIZE GET "${OMEGA_VITA_DISC_PROFILE}" iso_size)
string(JSON OMEGA_VITA_ISO_SHA256 GET "${OMEGA_VITA_DISC_PROFILE}" iso_sha256)
string(JSON OMEGA_VITA_EXE_SIZE GET "${OMEGA_VITA_DISC_PROFILE}" executable_size)
string(JSON OMEGA_VITA_EXE_SHA256 GET "${OMEGA_VITA_DISC_PROFILE}" executable_sha256)
string(JSON OMEGA_VITA_BOOT_EXECUTABLE GET "${OMEGA_VITA_DISC_PROFILE}" boot_executable)
target_compile_definitions(omega_vita PRIVATE OMEGA_ISO_SIZE=${OMEGA_VITA_ISO_SIZE}ULL
    OMEGA_ISO_SHA256="${OMEGA_VITA_ISO_SHA256}"
    OMEGA_EXE_SIZE=${OMEGA_VITA_EXE_SIZE}ULL OMEGA_EXE_SHA256="${OMEGA_VITA_EXE_SHA256}"
    OMEGA_BOOT_EXECUTABLE="${OMEGA_VITA_BOOT_EXECUTABLE}"
    # iso_verify_vita.hpp's boot check: SHA-256 of the image's first and last MiB and its ISO 9660 root directory.
    OMEGA_ISO_QUICK_SHA256="61445c6c05423c7bc50abb94411c494332b4f318733d6cbfa4488c1360670714")
# A GXM build is labelled as such so it can sit beside the vitaGL build (give it another OMEGA_VITA_TITLE_ID).
if(OMEGA_VITA_RENDERER STREQUAL "gxm")
    set(omega_vita_app_name "Omega Strain (native port, GXM)")
    set(omega_vita_livearea_line "NATIVE PORT GXM")
else()
    set(omega_vita_app_name "Omega Strain (native port)")
    set(omega_vita_livearea_line "NATIVE PORT")
endif()
omega_vita_livearea(${CMAKE_CURRENT_BINARY_DIR}/livearea_app "OMEGA STRAIN" "${omega_vita_livearea_line}")
# config/disc_us.json (the disc profile: sizes and hashes, nothing disc-derived) goes in the package: the play host
# checks for it under its working folder (vita_main.cpp copies it to ux0:data/omega_strain/config).
omega_vita_package(omega_vita ${OMEGA_VITA_TITLE_ID} "${omega_vita_app_name}"
    ${CMAKE_CURRENT_BINARY_DIR}/livearea_app ${CMAKE_CURRENT_SOURCE_DIR}/config/disc_us.json config/disc_us.json)

# SFOS00003: the display bring-up check (src/port/app/vita_red_main.cpp; nothing from the game; VITA.md).
add_executable(omega_vita_red src/port/app/vita_red_main.cpp)
target_link_libraries(omega_vita_red PRIVATE vitaGL mathneon vitashark SceShaccCgExt SceShaccCg_stub taihen_stub
    SceGxm_stub SceDisplay_stub SceSysmodule_stub SceCommonDialog_stub SceAppMgr_stub SceKernelDmacMgr_stub
    SceIofilemgr_stub SceLibKernel_stub SceKernelThreadMgr_stub SceCtrl_stub SceAudio_stub m)
omega_vita_link_threads(omega_vita_red)
omega_vita_livearea(${CMAKE_CURRENT_BINARY_DIR}/livearea_red "OMEGA STRAIN" "DISPLAY CHECK")
omega_vita_package(omega_vita_red SFOS00003 "Omega Strain display check" ${CMAKE_CURRENT_BINARY_DIR}/livearea_red)

if(BUILD_TESTING AND TARGET omega_core_tests)
    target_sources(omega_core_tests PRIVATE tests/vita_test_runner.cpp tests/vita_windows_only_tests.cpp
        src/port/render/gui_renderer.cpp)
    target_include_directories(omega_core_tests PRIVATE tests)
    target_link_libraries(omega_core_tests PRIVATE omega_gfx_gxm omega_vita_platform SceLibKernel_stub SceKernelThreadMgr_stub)
    omega_vita_link_threads(omega_core_tests)
    # The tests that read repository files (research/, config/, tests/data) look under the Vita data folder
    # (research/VITA.md "Core tests"): tools/build_vita.ps1 -StageTests copies them there for Vita3K.
    get_target_property(test_defines omega_core_tests COMPILE_DEFINITIONS)
    list(FILTER test_defines EXCLUDE REGEX "^OMEGA_SOURCE_DIR=")
    set_target_properties(omega_core_tests PROPERTIES COMPILE_DEFINITIONS "${test_defines}")
    target_compile_definitions(omega_core_tests PRIVATE OMEGA_SOURCE_DIR="ux0:data/omega_strain/repo")
    omega_vita_livearea(${CMAKE_CURRENT_BINARY_DIR}/livearea_tests "OMEGA STRAIN" "CORE TESTS")
    omega_vita_package(omega_core_tests SFOS00002 "Omega Strain core tests" ${CMAKE_CURRENT_BINARY_DIR}/livearea_tests)
endif()
