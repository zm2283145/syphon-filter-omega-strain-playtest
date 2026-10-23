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

# One function per package: vita_create_vpk is a macro that accumulates its flags in the calling scope.
function(omega_vita_package target title_id name livearea)
    vita_create_self(${target}.self ${target} UNSAFE)
    vita_create_vpk(${target}.vpk ${title_id} ${target}.self
        VERSION ${OMEGA_VITA_VERSION}
        NAME "${name}"
        FILE ${livearea}/icon0.png sce_sys/icon0.png
             ${livearea}/bg.png sce_sys/livearea/contents/bg.png
             ${livearea}/startup.png sce_sys/livearea/contents/startup.png
             ${livearea}/template.xml sce_sys/livearea/contents/template.xml)
endfunction()

function(omega_vita_livearea out title subtitle)
    execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/vita_livearea.py
        --out ${out} --title "${title}" --subtitle "${subtitle}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "tools/vita_livearea.py failed (${result})")
    endif()
endfunction()

add_executable(omega_vita src/port/app/vita_main.cpp src/port/render/gui_renderer.cpp src/port/app/loading_screen_host.cpp
    src/port/app/frontend_disc.cpp src/port/app/agent_preview.cpp src/port/render/frame_overlay.cpp)
target_link_libraries(omega_vita PRIVATE omega_gfx_gxm omega_vita_platform omega_core ScePower_stub)
omega_vita_link_threads(omega_vita)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config/disc_us.json" OMEGA_VITA_DISC_PROFILE)
string(JSON OMEGA_VITA_ISO_SIZE GET "${OMEGA_VITA_DISC_PROFILE}" iso_size)
string(JSON OMEGA_VITA_ISO_SHA256 GET "${OMEGA_VITA_DISC_PROFILE}" iso_sha256)
target_compile_definitions(omega_vita PRIVATE OMEGA_ISO_SIZE=${OMEGA_VITA_ISO_SIZE}ULL
    OMEGA_ISO_SHA256="${OMEGA_VITA_ISO_SHA256}"
    # iso_verify_vita.hpp's boot check: SHA-256 of the image's first and last MiB and its ISO 9660 root directory.
    OMEGA_ISO_QUICK_SHA256="61445c6c05423c7bc50abb94411c494332b4f318733d6cbfa4488c1360670714")
omega_vita_livearea(${CMAKE_CURRENT_BINARY_DIR}/livearea_app "OMEGA STRAIN" "NATIVE PORT")
omega_vita_package(omega_vita ${OMEGA_VITA_TITLE_ID} "Omega Strain (native port)"
    ${CMAKE_CURRENT_BINARY_DIR}/livearea_app)

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
