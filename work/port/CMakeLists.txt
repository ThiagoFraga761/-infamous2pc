cmake_minimum_required(VERSION 3.20)
project(MyGameRecomp
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Recompiled PS3 game project (ps3recomp)"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: this project is still sitting inside ps3recomp/templates/project/
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    # The table is scanned from the library sources: a new export needs a
    # regenerated table, or its import falls through to the unresolved stub.
    file(GLOB HLE_LIB_SOURCES CONFIGURE_DEPENDS "${PS3RECOMP_DIR}/libs/*/*.c")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
                ${HLE_LIB_SOURCES}
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)
# MinGW: g_trampoline_fn lives in its own unit, which tools/ppu_cxx.sh hands
# to the lifted code's compiler (see the file for why).
if(MINGW)
    list(APPEND PS3RECOMP_SCAFFOLD "${PS3RECOMP_DIR}/runtime/ppu/ppu_trampoline_tls.cpp")
endif()

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

set(SPU_SOURCES "")
if(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        # spu_cold.c: the target of the lifted code's register-preserving
        # cold calls (SPU_COLD_THUNK, runtime/spu/spu_context.h). It runs the
        # header's float fix-ups, so it is built with the same -march.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c"
             "${PS3RECOMP_DIR}/runtime/spu/spu_cold.c"
             # ec5f_native.c: the render-list job's main, natively. It runs on
             # the SPU's float helpers, so it is built like a lifted TU.
             "${CMAKE_CURRENT_SOURCE_DIR}/ec5f_native.c"
             "${CMAKE_CURRENT_SOURCE_DIR}/job2025_native.c"
             "${CMAKE_CURRENT_SOURCE_DIR}/edge_kernels_native.c"
             "${CMAKE_CURRENT_SOURCE_DIR}/jobbc11_native.c"
             "${CMAKE_CURRENT_SOURCE_DIR}/job207_native.c")
        # The animation kernels' bodies are generated from the lifted job
        # module (game code, so not in the repository): tools/gen_anim_kernels.sh.
        set(_anim_bin "${SPU_DIR}/jobmods/jobmod_2025343b09d0_eFE70.bin")
        if(EXISTS "${_anim_bin}")
            set(ANIM_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/anim_gen")
            add_custom_command(
                OUTPUT "${ANIM_GEN_DIR}/job2025_kernels_gen.h" "${ANIM_GEN_DIR}/job2025_kernels_wide.h"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${ANIM_GEN_DIR}"
                COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_anim_kernels.sh" "${_anim_bin}"
                        "${PS3RECOMP_DIR}" "${ANIM_GEN_DIR}"
                DEPENDS "${_anim_bin}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_anim_kernels.sh"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/kgen.py" "${CMAKE_CURRENT_SOURCE_DIR}/tools/kwide.py"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tools/kabi.py"
                VERBATIM)
            list(APPEND SPU_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/job2025_kernels_native.c")
        else()
            message(STATUS "  (no ${_anim_bin} -- animation kernels left out)")
        endif()
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c spu_elf_workloads.c jobmods_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

# Lifted SPU side of firmware modules (tools/lift_firmware_spu.py): libsre's SPURS
# kernel and the code bodies it streams into local store, so they run lifted
# instead of on the SPU interpreter when the firmware's own libsre is used.
set(FIRMWARE_SPU_DIR "" CACHE PATH
    "Directory of lifted firmware SPU code (<dir>/<image>/spu_recomp.c + *_spu_register.c)")
if(PORT_ENABLE_SPU AND FIRMWARE_SPU_DIR)
    file(GLOB FIRMWARE_SPU CONFIGURE_DEPENDS
         "${FIRMWARE_SPU_DIR}/*/spu_recomp.c" "${FIRMWARE_SPU_DIR}/*_spu_register.c")
    list(LENGTH FIRMWARE_SPU FIRMWARE_SPU_COUNT)
    message(STATUS "Firmware SPU:    ${FIRMWARE_SPU_COUNT} file(s) from ${FIRMWARE_SPU_DIR}")
    list(APPEND SPU_SOURCES ${FIRMWARE_SPU})
    if(NOT "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c" IN_LIST SPU_SOURCES)
        list(APPEND SPU_SOURCES "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
    endif()
endif()

add_executable(${PROJECT_NAME}
    main.cpp
    settings.cpp
    ui.cpp
    stubs.cpp
    port_cpu_check.c
    native_passes.c
    re_trace.c
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
)

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the SPU sources are small and decode-hot; -O2 with
#                          the inlining parameter below. The last -O on the
#                          line wins.
#   -march=PORT_SPU_MARCH -- x86-64 only. runtime/spu/spu_helpers.h has vector
#                          forms of the hot SPU ops for SSE4.1/AVX2 (and AVX-512):
#                          the float ops, the byte permutes, the conversions,
#                          the LS byte swap. On the baseline every one of them
#                          is a per-lane scalar loop. Empty keeps the baseline;
#                          port_cpu_check.c stops an x86-64-vN build with a
#                          message on a CPU below that level.
option(PORT_SPU_LS_DEBUG "Keep the SPU local-store debug hooks in the lifted code" OFF)
set(PORT_SPU_MARCH "x86-64-v3" CACHE STRING
    "-march for the lifted SPU sources on x86-64 (empty: compiler default)")
if(SPU_SOURCES)
    set(_spu_defs "SPU_USE_DISPATCH_MT;SPU_WRCH_INLINE")
    if(NOT PORT_SPU_LS_DEBUG)
        list(APPEND _spu_defs "SPU_LS_NO_DEBUG")
    endif()
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        # SPU_WRCH_INLINE: the MFC parameter channel writes inline
        # (spu_wrch_fast, runtime/spu/spu_helpers.h).
        # SPU_LS_NO_DEBUG: no local-store debug hooks in the lifted code
        # (runtime/spu/spu_context.h); PORT_SPU_LS_DEBUG=ON keeps them.
        COMPILE_DEFINITIONS "${_spu_defs}")
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_spu_opts "-O2")
        if(PORT_SPU_MARCH AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
            list(APPEND _spu_opts "-march=${PORT_SPU_MARCH}")
        endif()
        # A lifted SPU TU is a few huge functions calling hundreds of small
        # helpers; GCC stops inlining once the unit has grown 40%, and the
        # rest become calls that pass every 128-bit register through rdx:rax
        # (inFAMOUS 2's hottest job plugin loop: 144 calls -> 1).
        if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
            list(APPEND _spu_opts "--param=inline-unit-growth=1000")
        endif()
        set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "${_spu_opts}")
    endif()
endif()
if(ANIM_GEN_DIR)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/job2025_kernels_native.c" APPEND PROPERTY
                 INCLUDE_DIRECTORIES "${ANIM_GEN_DIR}")
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/job2025_kernels_native.c" APPEND PROPERTY
                 OBJECT_DEPENDS "${ANIM_GEN_DIR}/job2025_kernels_gen.h;${ANIM_GEN_DIR}/job2025_kernels_wide.h")
endif()
if(PORT_SPU_MARCH MATCHES "^x86-64-v[234]$" AND SPU_SOURCES)
    set_source_files_properties(port_cpu_check.c PROPERTIES
        COMPILE_DEFINITIONS "PORT_SPU_ISA_LEVEL=\"${PORT_SPU_MARCH}\"")
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/prx"             # prx_loader.h, for lifted PRX modules
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)

# The start screen's background is the disc's own PIC1.PNG, read with libpng
# when the system has it (Fedora: libpng-devel); without it, a gradient. The
# library's path only, and the define on ui.cpp alone: an include directory or
# a definition on the target would change every lifted TU's compile command.
find_package(PNG QUIET)
if(PNG_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${PNG_LIBRARIES})
    # A cross sysroot's own include dir (MinGW: <root>/include, where zlib.h
    # and png.h sit) must not be passed again: as a later -isystem it reorders
    # the search and libstdc++'s #include_next <stdlib.h> finds nothing.
    set(_png_inc ${PNG_INCLUDE_DIRS})
    foreach(_root ${CMAKE_FIND_ROOT_PATH})
        list(REMOVE_ITEM _png_inc "${_root}/include")
    endforeach()
    set_source_files_properties(ui.cpp PROPERTIES
        COMPILE_DEFINITIONS PORT_HAVE_PNG=1
        INCLUDE_DIRECTORIES "${_png_inc}")
endif()

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
    )
endif()

# MinGW: libgcc, libstdc++ and winpthread linked in, so the .exe does not need
# libgcc_s_seh-1.dll, libstdc++-6.dll or libwinpthread-1.dll beside it. -static
# only changes how -l names resolve; the libraries CMake found by full path
# (SDL2, libpng, zlib: their .dll.a import libraries) stay DLL imports.
if(MINGW)
    target_link_options(${PROJECT_NAME} PRIVATE -static)
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
        # The lift spells PPC arithmetic as signed C arithmetic, which must wrap;
        # without this, clang exploits overflow UB (e.g. folds "x*x < 0" to false).
        -fwrapv
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
#   - -march=PORT_PPU_MARCH on x86-64. ppu_vmx.h has four-lane forms of the
#     hottest VMX float ops (vmaddfp, vnmsubfp, vaddfp, vsubfp) for FMA and
#     SSE4.1; the baseline runs them a lane at a time through libm's fmaf.
#     Both give the same bits. port_cpu_check.c stops a build on a CPU below
#     the level with a message.
#   - scripts/build.sh sets CMAKE_CXX_COMPILER_LAUNCHER to tools/ppu_cxx.sh,
#     which hands these units to clang++: with the lifter's __restrict context
#     it keeps guest registers in host registers across guest memory accesses
#     (see docs/NOTES.md). PORT_PPU_GCC=1 in the environment keeps GCC.
set(PORT_PPU_MARCH "x86-64-v3" CACHE STRING
    "-march for the lifted PPU sources on x86-64 (empty: compiler default)")
if(MSVC)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/w;/bigobj")
else()
    set(_ppu_opts "-w;-ffp-contract=off")
    if(PORT_PPU_MARCH AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        list(APPEND _ppu_opts "-march=${PORT_PPU_MARCH}")
    endif()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES COMPILE_OPTIONS "${_ppu_opts}"
        COMPILE_DEFINITIONS PPU_INLINE_STORES)
endif()
if(PORT_PPU_MARCH MATCHES "^x86-64-v[234]$")
    set_property(SOURCE port_cpu_check.c APPEND PROPERTY
        COMPILE_DEFINITIONS "PORT_PPU_ISA_LEVEL=\"${PORT_PPU_MARCH}\"")
elseif(PORT_PPU_MARCH)
    message(WARNING "PORT_PPU_MARCH=${PORT_PPU_MARCH} is not an x86-64-vN level: "
                    "port_cpu_check.c cannot test for it, and a CPU without it gets a bare SIGILL")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
