# Port-only (132ps5c, 132ps5d; research/PS5.md): the PS5 target's own libraries and the folder title. Included at the
# end of CMakeLists.txt when the PS5 toolchain is in use (cmake/ps5/ps5.toolchain.cmake, tools/build_ps5.ps1).
#
#   omega_ps5_platform  the platform layer (src/port/ps5: log, heap, sceAudioOut, scePad, clocks, the disc check, the
#                       crash reporter, the fixed-screen host window) in place of the Windows files of the PC build
#   omega_gfx_gl46      gfx::Device on OpenGL (src/port/gfx/gfx_gl46_ps5.cpp) with the shared Cg sources and the
#                       backend-neutral render thread
#   omega_ps5_app       PPSA99720: the PS5 app (src/port/ps5/ps5_main.cpp) and the PC play host's sources
#   omega_ps5_package   the folder title <build>/PPSA99720 (eboot.bin, sce_module/libc.prx, sce_sys, assets), linked
#                       the way ps5-app-sdk's meson.build does it: its crt, linker script and symbol map, the link
#                       stubs of the payload SDK and of the AGC libraries, ps5-native-tool link + self --sign
#   omega_ps5_imports   (on demand) regenerates src/port/ps5/ps5_imports.cpp from the linked title's imports
foreach(host_target IN LISTS OMEGA_PS5_HOST_ONLY_TARGETS)
    if(TARGET ${host_target})
        set_target_properties(${host_target} PROPERTIES EXCLUDE_FROM_ALL ON)
    endif()
endforeach()
enable_language(C)

add_library(omega_ps5_platform STATIC
    src/port/ps5/ps5_log.cpp
    src/port/ps5/ps5_heap.cpp
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
target_include_directories(omega_gfx_gl46 PRIVATE "${PS5_GL_PREFIX}/include")
target_compile_definitions(omega_gfx_gl46 PRIVATE GL_GLEXT_PROTOTYPES=1 EGL_EGLEXT_PROTOTYPES=1)
target_link_libraries(omega_gfx_gl46 PUBLIC omega_core omega_ps5_platform)

# The PC play host (CMakeLists.txt's omega_play_objects) without its Windows-only files (host_window.cpp,
# audio_device.cpp, gamepad_device.cpp, pacing_probe.cpp, launcher_mod_panel.cpp, the D3D11 backend), which the PS5
# platform layer and the GL backend replace.
add_library(omega_ps5_app STATIC src/port/ps5/ps5_main.cpp
    src/port/app/play_main.cpp src/port/app/launcher.cpp src/port/app/frontend_app.cpp src/port/app/frontend_disc.cpp
    src/port/app/movie_audio_stage.cpp
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

# ---- the native app's runtime (ps5-app-sdk) ----

# crt: _start (process parameters, static constructors, main) and the C++ allocation operators over malloc, built as
# ps5-app-sdk builds them (no exceptions or RTTI in these two files). Its other crt files are the payload loader's
# (syscalls, the kernel r/w helpers): a native title cannot make syscalls (ps5-app-sdk README), so they are not linked.
add_library(omega_ps5_crt OBJECT "${PS5_APP_SDK}/crt/app_crt.cpp" "${PS5_APP_SDK}/crt/app_cpp_runtime.cpp")
target_compile_options(omega_ps5_crt PRIVATE -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)
# The AGC libraries' link stubs (import names only; the system modules libSceAgc.prx / libSceAgcDriver.prx).
add_library(omega_ps5_stub_SceAgc OBJECT "${PS5_APP_SDK}/agc_stubs/src/agc_link_stub.c")
add_library(omega_ps5_stub_SceAgcDriver OBJECT "${PS5_APP_SDK}/agc_stubs/src/agc_driver_link_stub.c")

set(OMEGA_PS5_OUT "${CMAKE_CURRENT_BINARY_DIR}/ps5")
set(OMEGA_PS5_STUB_DIR "${OMEGA_PS5_OUT}/stubs")
set(OMEGA_PS5_APP_DIR "${CMAKE_CURRENT_BINARY_DIR}/PPSA99720")
set(OMEGA_PS5_LLD "${CMAKE_COMMAND}" -E env "LLVM_CONFIG=${OMEGA_PS5_LLVM_CONFIG}" "${PS5_PAYLOAD_SDK}/bin/prospero-lld")
set(OMEGA_PS5_TOOL "${PS5_HOST_TOOLS}/ps5-native-tool")

# The link stubs: the payload SDK's system-module stubs, with the AGC pair built from ps5-app-sdk's sources (as its
# meson.build passes them to ps5-native-tool with --stub).
file(GLOB OMEGA_PS5_SDK_STUBS "${PS5_PAYLOAD_SDK}/target/lib/*.so")
list(FILTER OMEGA_PS5_SDK_STUBS EXCLUDE REGEX "/libSceAgc(Driver)?\\.so$")
set(OMEGA_PS5_STUBS)
foreach(stub IN LISTS OMEGA_PS5_SDK_STUBS)
    get_filename_component(stub_name "${stub}" NAME)
    list(APPEND OMEGA_PS5_STUBS "${OMEGA_PS5_STUB_DIR}/${stub_name}")
endforeach()
add_custom_command(OUTPUT ${OMEGA_PS5_STUBS}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OMEGA_PS5_STUB_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy ${OMEGA_PS5_SDK_STUBS} "${OMEGA_PS5_STUB_DIR}"
    DEPENDS ${OMEGA_PS5_SDK_STUBS}
    VERBATIM)
foreach(lib SceAgc SceAgcDriver)
    add_custom_command(OUTPUT "${OMEGA_PS5_STUB_DIR}/lib${lib}.so"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OMEGA_PS5_STUB_DIR}"
        COMMAND ${OMEGA_PS5_LLD} --shared -soname "lib${lib}.prx" -o "${OMEGA_PS5_STUB_DIR}/lib${lib}.so"
            $<TARGET_OBJECTS:omega_ps5_stub_${lib}>
        DEPENDS omega_ps5_stub_${lib} $<TARGET_OBJECTS:omega_ps5_stub_${lib}>
        COMMAND_EXPAND_LISTS
        VERBATIM)
    list(APPEND OMEGA_PS5_STUBS "${OMEGA_PS5_STUB_DIR}/lib${lib}.so")
endforeach()

# libc++'s locale code needs the *_l functions; the SDK's static libc has a C-locale-only set (no-locale.o) that
# forwards to the plain functions. Only those members: the rest of libc.a would replace system imports.
add_custom_command(OUTPUT "${OMEGA_PS5_OUT}/libc_nolocale.a"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OMEGA_PS5_OUT}/libc_nolocale"
    COMMAND ${CMAKE_COMMAND} -E chdir "${OMEGA_PS5_OUT}/libc_nolocale"
        "${OMEGA_PS5_AR}" x "${PS5_PAYLOAD_SDK}/target/lib/libc.a" no-locale.o mbsnrtowcs.o wcsnrtombs.o
    COMMAND ${CMAKE_COMMAND} -E rm -f "${OMEGA_PS5_OUT}/libc_nolocale.a"
    COMMAND "${OMEGA_PS5_AR}" rcs "${OMEGA_PS5_OUT}/libc_nolocale.a" "${OMEGA_PS5_OUT}/libc_nolocale/no-locale.o"
        "${OMEGA_PS5_OUT}/libc_nolocale/mbsnrtowcs.o" "${OMEGA_PS5_OUT}/libc_nolocale/wcsnrtombs.o"
    DEPENDS "${PS5_PAYLOAD_SDK}/target/lib/libc.a"
    VERBATIM)

execute_process(COMMAND "${OMEGA_PS5_CLANG}" --print-resource-dir OUTPUT_VARIABLE OMEGA_PS5_CLANG_RESOURCES
    OUTPUT_STRIP_TRAILING_WHITESPACE)
set(OMEGA_PS5_BUILTINS "${OMEGA_PS5_CLANG_RESOURCES}/lib/linux/libclang_rt.builtins-x86_64.a")
if(NOT EXISTS "${OMEGA_PS5_BUILTINS}")
    message(FATAL_ERROR "clang 18's compiler-rt builtins are missing (${OMEGA_PS5_BUILTINS}): install libclang-rt-18-dev")
endif()
# The OpenGL runtime (Mesa over AGC), linked statically, in the order of ps5-app-sdk's meson.build.
set(OMEGA_PS5_GL_LIBS)
foreach(lib blake3 compiler gallium glapi_bridge glapi glcpp glsl_util glsl mesa_util_c11 mesa_util_simd
        mesa_util_clflush mesa_util_clflushopt mesa_util mesa mesa_sse41 nir ps5_opengl_core33 psbc.ps5 softpipe vtn)
    list(APPEND OMEGA_PS5_GL_LIBS "${PS5_GL_PREFIX}/lib/lib${lib}.a")
endforeach()

# Every C library call that takes a path is the title's own (src/port/ps5/ps5_cwd.cpp: the sandbox refuses chdir and
# the runtime's getcwd faults), malloc and friends are the title's heap (src/port/ps5/ps5_heap.cpp), and every C++
# throw is logged (ps5_libc_extras.cpp).
set(OMEGA_PS5_WRAPS malloc calloc realloc free posix_memalign malloc_usable_size
    chdir getcwd realpath access fchmodat fopen freopen link lstat mkdir mkstemp open openat opendir fdopendir readdir
    closedir __cxa_throw pathconf readlink remove rename stat statvfs symlink truncate unlink unlinkat utimes)
list(TRANSFORM OMEGA_PS5_WRAPS PREPEND "--wrap=" OUTPUT_VARIABLE OMEGA_PS5_WRAP_FLAGS)

set(OMEGA_PS5_PIE "${OMEGA_PS5_OUT}/llvm-pie.elf")
add_custom_command(OUTPUT "${OMEGA_PS5_PIE}"
    COMMAND ${OMEGA_PS5_LLD} -T "${PS5_APP_SDK}/ps5-elf.ld" --eh-frame-hdr
        --version-script "${PS5_APP_SDK}/ps5-app-symbols.map" -e _start -o "${OMEGA_PS5_PIE}"
        # libc++ / libc++abi / libunwind name pthread as a dependent library: the SDK's (empty) libpthread.a.
        -L "${PS5_PAYLOAD_SDK}/target/lib"
        ${OMEGA_PS5_WRAP_FLAGS} --undefined=ps5_agc_gate2_run
        $<TARGET_OBJECTS:omega_ps5_crt>
        --start-group
        $<TARGET_FILE:omega_ps5_app> $<TARGET_FILE:omega_gfx_gl46> $<TARGET_FILE:omega_ps5_platform>
        $<TARGET_FILE:omega_core> $<TARGET_FILE:omega_online>
        ${OMEGA_PS5_GL_LIBS}
        "${PS5_PAYLOAD_SDK}/target/lib/libunwind.a" "${PS5_PAYLOAD_SDK}/target/lib/libc++abi.a"
        "${PS5_PAYLOAD_SDK}/target/lib/libc++.a" "${OMEGA_PS5_OUT}/libc_nolocale.a" "${OMEGA_PS5_BUILTINS}"
        --end-group
        --as-needed ${OMEGA_PS5_STUBS}
    DEPENDS omega_ps5_crt $<TARGET_OBJECTS:omega_ps5_crt> omega_ps5_app omega_gfx_gl46 omega_ps5_platform omega_core
        omega_online ${OMEGA_PS5_GL_LIBS} ${OMEGA_PS5_STUBS} "${OMEGA_PS5_OUT}/libc_nolocale.a"
        "${PS5_APP_SDK}/ps5-elf.ld" "${PS5_APP_SDK}/ps5-app-symbols.map"
    COMMENT "Linking the PS5 title (llvm-pie.elf)"
    COMMAND_EXPAND_LISTS
    VERBATIM)
# The PS5 module (loader and container constants of ps5-app-sdk's meson.build), signed as the hardware-tested
# eboot.bin of 132ps5c (the alternate fake-signed container magic).
add_custom_command(OUTPUT "${OMEGA_PS5_OUT}/eboot.elf" "${OMEGA_PS5_OUT}/eboot.bin"
    COMMAND "${OMEGA_PS5_TOOL}" link --in "${OMEGA_PS5_PIE}" --out "${OMEGA_PS5_OUT}/eboot.elf"
        --stub-dir "${OMEGA_PS5_STUB_DIR}" --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
    COMMAND "${OMEGA_PS5_TOOL}" self --sign --in "${OMEGA_PS5_OUT}/eboot.elf" --out "${OMEGA_PS5_OUT}/eboot.bin"
        --magic 0x1D3D154F
    DEPENDS "${OMEGA_PS5_PIE}" "${OMEGA_PS5_TOOL}" ${OMEGA_PS5_STUBS}
    VERBATIM)
# libc.prx: the C library module the loader wants beside the title, built from ps5-app-sdk's API surface.
add_custom_command(OUTPUT "${OMEGA_PS5_OUT}/libc.prx"
    COMMAND "${PS5_HOST_TOOLS}/libc-prx-builder" "${PS5_APP_SDK}/tools/libc-prx-builder/api-surface.txt"
        "${PS5_APP_SDK}/tools/libc-prx-builder/imports.txt" "${OMEGA_PS5_OUT}/libc.elf"
    COMMAND "${OMEGA_PS5_TOOL}" self --sign --in "${OMEGA_PS5_OUT}/libc.elf" --out "${OMEGA_PS5_OUT}/libc.prx"
    DEPENDS "${PS5_HOST_TOOLS}/libc-prx-builder" "${OMEGA_PS5_TOOL}"
    VERBATIM)

set(OMEGA_PS5_SHELL_MEDIA "${CMAKE_CURRENT_SOURCE_DIR}/config/ps5/sce_sys")
add_custom_target(omega_ps5_package ALL
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${OMEGA_PS5_APP_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OMEGA_PS5_APP_DIR}/sce_module" "${OMEGA_PS5_APP_DIR}/sce_sys"
        "${OMEGA_PS5_APP_DIR}/assets/config"
    COMMAND ${CMAKE_COMMAND} -E copy "${OMEGA_PS5_OUT}/eboot.bin" "${OMEGA_PS5_APP_DIR}/eboot.bin"
    COMMAND ${CMAKE_COMMAND} -E copy "${OMEGA_PS5_OUT}/libc.prx" "${OMEGA_PS5_APP_DIR}/sce_module/libc.prx"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ps5/param.json"
        "${OMEGA_PS5_APP_DIR}/sce_sys/param.json"
    COMMAND ${CMAKE_COMMAND} -E copy "${OMEGA_PS5_SHELL_MEDIA}/icon0.png"
        "${OMEGA_PS5_APP_DIR}/sce_sys/icon0.png"
    COMMAND ${CMAKE_COMMAND} -E copy "${OMEGA_PS5_SHELL_MEDIA}/pic0.dds"
        "${OMEGA_PS5_APP_DIR}/sce_sys/pic0.dds"
    COMMAND ${CMAKE_COMMAND} -E copy "${OMEGA_PS5_SHELL_MEDIA}/pic1.dds"
        "${OMEGA_PS5_APP_DIR}/sce_sys/pic1.dds"
    COMMAND ${CMAKE_COMMAND} -E copy "${OMEGA_PS5_SHELL_MEDIA}/snd0.at9"
        "${OMEGA_PS5_APP_DIR}/sce_sys/snd0.at9"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/config/disc_us.json"
        "${OMEGA_PS5_APP_DIR}/assets/config/disc_us.json"
    COMMAND ${CMAKE_COMMAND} -E echo "PS5 folder title: ${OMEGA_PS5_APP_DIR}"
    DEPENDS "${OMEGA_PS5_OUT}/eboot.bin" "${OMEGA_PS5_OUT}/libc.prx" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ps5/param.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/config/disc_us.json"
        "${OMEGA_PS5_SHELL_MEDIA}/icon0.png" "${OMEGA_PS5_SHELL_MEDIA}/pic0.dds"
        "${OMEGA_PS5_SHELL_MEDIA}/pic1.dds" "${OMEGA_PS5_SHELL_MEDIA}/snd0.at9"
    COMMENT "Packaging the PS5 folder title PPSA99720"
    VERBATIM)

# Regenerates src/port/ps5/ps5_imports.cpp from the linked title's imports (research/PS5.md "Imports"); build the
# package again afterwards, until a rebuild leaves the file unchanged.
add_custom_target(omega_ps5_imports
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/ps5/genimports.sh" "${OMEGA_PS5_PIE}" "${OMEGA_PS5_OUT}/imports.txt"
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tools/ps5/genimports.py" "${OMEGA_PS5_OUT}/imports.txt"
    DEPENDS "${OMEGA_PS5_PIE}"
    VERBATIM)
