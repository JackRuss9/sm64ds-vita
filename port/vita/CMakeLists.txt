# PS Vita build of the port's smoke gates (VitaSDK, arm-vita-eabi-gcc).
#
#   cmake -S port/vita -B build/vita
#   cmake --build build/vita
#
# Produces one VPK per gate. Install with VitaShell, launch from the LiveArea,
# then read ux0:data/sm64ds/<gate>.log (and results.txt for the summary).
#
# Gates 1 and 2 need no ROM. Gate 3 onward reads data extracted from your own
# EUR cartridge dump and is not wired up here yet.
#
# Needs the host-ABI patch (types.h size_t, the MSVC-only bridges in port/hal)
# that lets the port build with GCC/Clang; without it the link fails with
# duplicate symbols.

cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "Set VITASDK (see https://vitasdk.org) before configuring.")
    endif()
endif()

project(sm64ds-vita-gates C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS ON)    # gnu++17: the decomp uses GNU extensions
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(PORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(REPO_DIR "${PORT_DIR}/.." ABSOLUTE)

# The decomp's guest idioms (pointer<->u32 casts, type-punned struct access)
# are deliberate. -fno-strict-aliasing keeps GCC from optimizing them away;
# the warnings are silenced so a real error stays visible.
set(SM64DS_FLAGS -fno-strict-aliasing -w -ffunction-sections -fdata-sections)
# mwccarm's ABI where GCC's ARM defaults differ: `char` is signed, an enum is
# an int (arm-vita-eabi, a bare-metal AAPCS target, would otherwise give enums
# the smallest type that fits and shift every struct that holds one), and the
# globals of a file stay in source order, the order the DS laid them out in
# (code that steps from one object into the next keeps working).
list(APPEND SM64DS_FLAGS -fsigned-char -fno-short-enums -fno-toplevel-reorder)
# The decomp checks `this == 0` (and pointers it has just used) for null, as
# the DS code does: Sound::StopLoadedMusic_Layer1 calls into the music
# player, which returns when there is none. GCC takes `this` and dereferenced
# pointers as never null and deletes those checks, so the call faulted on the
# title screen. Keep them.
list(APPEND SM64DS_FLAGS -fno-delete-null-pointer-checks)
# The decomp calls virtual functions through local stand-in classes, e.g.
# `struct VObj { virtual void d00(); ... virtual void m14(void *); }` cast
# over a real model (Player::Render, func_ov002_020e4768). GCC sees a class
# that is never constructed, decides no object of it can exist, and deletes
# the call as unreachable: the players' heads were never placed or drawn
# (Yoshi's sat at his feet, untouched since loading). Keep every such call.
list(APPEND SM64DS_FLAGS -fno-devirtualize -fno-devirtualize-speculatively)
# mwccarm accepts a bare `return;` in a non-void function (the ROM's own
# daObjYajirusi_c::OnAttacked1 has one, slot type int, no value on either
# path); g++ only does with -fpermissive.
list(APPEND SM64DS_FLAGS $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>)
# More of what the ROM's code does that the ARM9 shrugs off and GCC treats as
# "cannot happen", so optimizes into something else (the same class as the
# null checks, devirtualization and missing returns above, found ahead of the
# hardware this time):
#  - signed arithmetic that overflows: the ARM9 wraps (fixed-point products,
#    angle sums); GCC may assume it never does;
#  - loops that index a table past its declared end (the decomp declares many
#    ROM tables with a placeholder size): GCC may shorten or never end them;
#  - a dereference GCC proves is null: it would plant a trap there;
#  - locals read before they are set (the DS reads a stale stack word; 30
#    such reads compile with a warning here): zeroed, where GCC can.
list(APPEND SM64DS_FLAGS -fwrapv -fno-aggressive-loop-optimizations
     -fno-isolate-erroneous-paths-dereference -fno-isolate-erroneous-paths-attribute)
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-ftrivial-auto-var-init=zero SM64DS_HAVE_AUTO_VAR_INIT)
if(SM64DS_HAVE_AUTO_VAR_INIT)
    list(APPEND SM64DS_FLAGS -ftrivial-auto-var-init=zero)
else()
    message(STATUS "This GCC cannot zero uninitialized locals (-ftrivial-auto-var-init needs GCC 12)")
endif()
# vita_create_self needs the relocations kept in the ELF.
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-q")
# Function-level linking, as the PC port gets from MSVC (/Gy + /OPT:REF): each
# function in its own section, unreferenced ones dropped at link time. The
# slices compile whole translation units whose other members call into parts of
# the game no gate links yet; like MSVC, the linker only follows what is used.
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
# vita-elf-create appends its SCE module data (module info, import stubs) right
# after the text segment. The VitaSDK linker script 64 KiB-aligns the data
# segment and, before that, adds __sce_headroom if it is defined: without it, a
# text segment that happens to end just below a 64 KiB boundary leaves too
# little room ("Cannot allocate N bytes for SCE data at end of segment 0;
# segment 1 overlaps" -- hit on gate 3b). 16 KiB covers far more imports than
# any gate has; the cost is at most one extra 64 KiB of address space.
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--defsym,__sce_headroom=0x4000")

# Read a slice manifest (one src path per line, # comments) into a list.
function(sm64ds_read_slice manifest out_var)
    file(STRINGS "${PORT_DIR}/${manifest}" lines)
    set(result "")
    foreach(line IN LISTS lines)
        string(STRIP "${line}" line)
        if(line MATCHES "^#" OR line STREQUAL "")
            continue()
        endif()
        list(APPEND result "${REPO_DIR}/${line}")
    endforeach()
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

# Same rule as port/CMakeLists.txt: a .c file whose first line is exactly
# //cpp is C++ to the matching toolchain, so it is C++ here too.
function(sm64ds_honor_cpp_markers source_var)
    foreach(source IN LISTS ${source_var})
        if(source MATCHES "\\.c$")
            file(STRINGS "${source}" first_line LIMIT_COUNT 1)
            if(first_line STREQUAL "//cpp")
                set_source_files_properties("${source}" PROPERTIES LANGUAGE CXX)
            endif()
        endif()
    endforeach()
endfunction()

# Any slice file that touches a DS address in code (a register deref or a
# pointer cast like `(u16 *)0x06800000`) compiles from its hostgen'd copy: on the
# Vita nothing is mapped at those addresses, so the raw file would fault there.
# hostgen --needs decides at configure time; each copy is generated once.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# TUs whose host copy also goes through port/tools/hostexterns.py.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/hostexterns_files.txt" SM64DS_HOSTEXTERNS_LINES)
set(SM64DS_HOSTEXTERNS "")
foreach(l IN LISTS SM64DS_HOSTEXTERNS_LINES)
    string(STRIP "${l}" l)
    if(NOT l STREQUAL "" AND NOT l MATCHES "^#")
        list(APPEND SM64DS_HOSTEXTERNS "${l}")
    endif()
endforeach()

function(sm64ds_vita_auto_hostgen srcs_var)
    set(rels "")
    foreach(f IN LISTS ${srcs_var})
        file(RELATIVE_PATH rel "${REPO_DIR}" "${f}")
        list(APPEND rels "${rel}")
    endforeach()
    if(NOT rels)
        return()
    endif()
    execute_process(
        COMMAND ${Python3_EXECUTABLE} "${PORT_DIR}/tools/hostgen.py"
                --decomp "${REPO_DIR}" --needs ${rels}
        RESULT_VARIABLE rc OUTPUT_VARIABLE needed ERROR_VARIABLE err
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "hostgen --needs failed: ${err}")
    endif()
    string(REPLACE "\n" ";" needed "${needed}")
    set(hgen "${needed}")
    # hostexterns-only TUs (no DS addresses) get a host copy too
    foreach(rel IN LISTS rels)
        list(FIND SM64DS_HOSTEXTERNS "${rel}" xi)
        list(FIND needed "${rel}" ni)
        if(NOT xi EQUAL -1 AND ni EQUAL -1)
            list(APPEND needed "${rel}")
        endif()
    endforeach()
    set(out_list ${${srcs_var}})
    get_property(done GLOBAL PROPERTY SM64DS_AUTO_HOSTGEN)
    foreach(rel IN LISTS needed)
        if(rel STREQUAL "")
            continue()
        endif()
        string(REGEX REPLACE "\\.(c|cpp)$" ".cpp" out_rel "${rel}")
        set(out "${CMAKE_BINARY_DIR}/host-src/${out_rel}")
        list(FIND done "${rel}" idx)
        if(idx EQUAL -1)
            list(FIND hgen "${rel}" hi)
            list(FIND SM64DS_HOSTEXTERNS "${rel}" xi)
            set(cmds "")
            if(NOT hi EQUAL -1)
                list(APPEND cmds COMMAND ${Python3_EXECUTABLE} "${PORT_DIR}/tools/hostgen.py"
                        --decomp "${REPO_DIR}" --out "${CMAKE_BINARY_DIR}/host-src"
                        --strip-asm --paths "${rel}")
                set(xin "${out}")
            else()
                set(xin "${REPO_DIR}/${rel}")
            endif()
            if(NOT xi EQUAL -1)
                list(APPEND cmds COMMAND ${Python3_EXECUTABLE} "${PORT_DIR}/tools/hostexterns.py"
                        "${xin}" "${out}" --or "${REPO_DIR}/${rel}")
            endif()
            add_custom_command(OUTPUT "${out}" ${cmds}
                DEPENDS "${REPO_DIR}/${rel}" "${PORT_DIR}/tools/hostgen.py"
                        "${PORT_DIR}/tools/hostexterns.py"
                COMMENT "host copy of ${rel}")
            list(APPEND done "${rel}")
        endif()
        list(REMOVE_ITEM out_list "${REPO_DIR}/${rel}")
        list(APPEND out_list "${out}")
        message(STATUS "vita: ${rel} builds from its hostgen copy")
    endforeach()
    set_property(GLOBAL PROPERTY SM64DS_AUTO_HOSTGEN "${done}")
    set(${srcs_var} "${out_list}" PARENT_SCOPE)
endfunction()

# One gate = its smoke test (main renamed), its HAL files, its slice, the
# launcher, packed into a VPK with its own title ID.
function(sm64ds_vita_gate name title_id label test_src)
    cmake_parse_arguments(G "" "" "HAL;SLICES;DEFINES;EXCLUDE" ${ARGN})
    set(srcs "")
    foreach(manifest IN LISTS G_SLICES)
        sm64ds_read_slice(${manifest} slice)
        list(APPEND srcs ${slice})
    endforeach()
    list(REMOVE_DUPLICATES srcs)
    # A translation unit built from its hostgen'd copy must not also compile
    # from src/: same symbols twice.
    if(G_EXCLUDE)
        list(REMOVE_ITEM srcs ${G_EXCLUDE})
    endif()
    sm64ds_vita_auto_hostgen(srcs)
    sm64ds_honor_cpp_markers(srcs)
    set(hal "")
    foreach(h IN LISTS G_HAL)
        list(APPEND hal "${PORT_DIR}/${h}")
    endforeach()
    # HAL diagnostics (e.g. "ROM not found") go to the same log as the test.
    # src/ stays untouched: only port-owned files get the redirect.
    set_source_files_properties(${hal} PROPERTIES
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/vita_log.h")

    set(test "${PORT_DIR}/${test_src}")
    set_source_files_properties("${test}" PROPERTIES
        COMPILE_DEFINITIONS "main=sm64ds_test_main"
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/vita_log.h")

    add_executable(${name} "${CMAKE_CURRENT_SOURCE_DIR}/vita_main.cpp" "${test}" ${hal} ${srcs})
    target_include_directories(${name} PRIVATE "${PORT_DIR}" "${REPO_DIR}/include"
        "${PORT_DIR}/ntr/include")   # hostgen'd copies include ntr/mmio.h
    target_compile_definitions(${name} PRIVATE SM64DS_PLATFORM_PC SM64DS_GATE_NAME="${name}")
    target_compile_options(${name} PRIVATE ${SM64DS_FLAGS})
    if(G_DEFINES)
        target_compile_definitions(${name} PRIVATE ${G_DEFINES})
    endif()

    vita_create_self(${name}.self ${name})
    vita_create_vpk(${name}.vpk ${title_id} ${name}.self
        VERSION 01.00
        NAME "${label}")
endfunction()

# Gate 1: fx math, matrices, Timer, the dFader_c hierarchy.
sm64ds_vita_gate(gate1 SMDS00001 "SM64DS Gate 1" tests/smoke.cpp
    HAL hal/cstd_div.c hal/os_time.cpp hal/shims.cpp hal/mem_delete2.cpp
    SLICES slice_gate1.txt)

# Gate 2: ExpandingHeapAllocator, 5,000-op randomized torture.
sm64ds_vita_gate(gate2 SMDS00002 "SM64DS Gate 2" tests/smoke_heap.cpp
    HAL hal/heap_globals.cpp
    SLICES slice_gate2.txt)

# Gate 3a: SetupRootHeap + the Memory:: layer over the gate-2 allocator.
sm64ds_vita_gate(gate3a SMDS00003 "SM64DS Gate 3a" tests/smoke_roots.cpp
    HAL hal/heap_globals.cpp hal/os_arena.cpp hal/heap_vtable.cpp hal/mem_delete2.cpp
    SLICES slice_gate2.txt slice_gate3a.txt)

# Gate 3b: real game files through SharedFilePtr, read straight from the
# cartridge dump (hal/romfs.cpp) -- copy your own EUR .nds to
# ux0:data/sm64ds/sm64ds.nds. smoke_fs's expected values were computed from
# that ROM, so a PASS cross-checks the reader against the real data.
sm64ds_vita_gate(gate3b SMDS00004 "SM64DS Gate 3b" tests/smoke_fs.cpp
    HAL hal/heap_globals.cpp hal/os_arena.cpp hal/heap_vtable.cpp hal/mem_delete2.cpp
        hal/fs.cpp hal/romfs.cpp
    SLICES slice_gate2.txt slice_gate3a.txt slice_gate3b.txt
    DEFINES SM64DS_FS_ROM)

# ---------------------------------------------------------------------------
# From gate 4a on: the ntr DS platform layer and hostgen'd src/ copies.

# The DS regions (I/O, palettes, VRAM, OAM, main RAM) cannot be mapped at their
# real low addresses on the Vita; ntr allocates them and translates DS
# addresses through host_ptr (see ntr/include/ntr/mmio.h).
add_library(ntr STATIC
    ${PORT_DIR}/ntr/io.cpp ${PORT_DIR}/ntr/gx.cpp ${PORT_DIR}/ntr/texture.cpp
    ${PORT_DIR}/ntr/ppu.cpp ${PORT_DIR}/ntr/ppu_compose.cpp ${PORT_DIR}/ntr/lz77.cpp ${PORT_DIR}/ntr/rt.cpp
    ${PORT_DIR}/ntr/runtime.cpp ${PORT_DIR}/ntr/fs.cpp ${PORT_DIR}/ntr/bmd.cpp
    ${PORT_DIR}/ntr/vram.cpp)
target_include_directories(ntr PUBLIC "${PORT_DIR}/ntr/include")
target_compile_definitions(ntr PUBLIC NTR_RELOCATE_REGIONS)
# rt.cpp runs the game on its own thread (VitaSDK: pthread-embedded, native threads)
target_link_libraries(ntr PUBLIC pthread)
target_compile_options(ntr PRIVATE ${SM64DS_FLAGS})
set_source_files_properties(
    ${PORT_DIR}/ntr/io.cpp ${PORT_DIR}/ntr/gx.cpp ${PORT_DIR}/ntr/texture.cpp
    ${PORT_DIR}/ntr/ppu.cpp ${PORT_DIR}/ntr/ppu_compose.cpp ${PORT_DIR}/ntr/lz77.cpp ${PORT_DIR}/ntr/rt.cpp
    ${PORT_DIR}/ntr/runtime.cpp ${PORT_DIR}/ntr/fs.cpp ${PORT_DIR}/ntr/bmd.cpp
    ${PORT_DIR}/ntr/vram.cpp
    PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/vita_log.h")

# hostgen: MMIO-touching src/ files are built from transformed copies under the
# build tree (src/ is never edited). Same tool and resolution as the PC build;
# python3 explicitly, since WSL/Linux usually has no `python`.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
function(sm64ds_vita_hostgen out_var)
    set(generated "")
    set(originals "")
    set(seen "")
    foreach(sym IN LISTS ARGN)
        execute_process(
            COMMAND ${Python3_EXECUTABLE} "${REPO_DIR}/tools/srcpath.py" "${sym}"
            WORKING_DIRECTORY "${REPO_DIR}"
            RESULT_VARIABLE rc OUTPUT_VARIABLE rel ERROR_VARIABLE err
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT rc EQUAL 0 OR rel MATCHES ": not in src/$")
            message(FATAL_ERROR "hostgen source resolution failed for ${sym}: ${rel}${err}")
        endif()
        list(FIND seen "${rel}" idx)
        if(NOT idx EQUAL -1)
            continue()
        endif()
        list(APPEND seen "${rel}")
        string(REGEX REPLACE "\\.(c|cpp)$" ".cpp" out_rel "${rel}")
        set(out "${CMAKE_BINARY_DIR}/host-src/${out_rel}")
        add_custom_command(OUTPUT "${out}"
            COMMAND ${Python3_EXECUTABLE} "${PORT_DIR}/tools/hostgen.py"
                    --out "${CMAKE_BINARY_DIR}/host-src" ${sym}
            DEPENDS "${REPO_DIR}/${rel}" "${PORT_DIR}/tools/hostgen.py"
            COMMENT "hostgen ${rel}")
        list(APPEND generated "${out}")
        list(APPEND originals "${REPO_DIR}/${rel}")
        # one rule per output: let sm64ds_vita_auto_hostgen reuse this copy
        set_property(GLOBAL APPEND PROPERTY SM64DS_AUTO_HOSTGEN "${rel}")
    endforeach()
    set(${out_var} "${generated}" PARENT_SCOPE)
    set(${out_var}_SRC "${originals}" PARENT_SCOPE)   # for a gate's EXCLUDE
endfunction()

# Gate 4a: the game's display-list pump (MI_SendGXCommandAsync and its DMA/IRQ
# chain) drives the ntr GX with Mario's model, read from the ROM. Also writes
# ux0:data/sm64ds/smoke_gx.bmp -- the fitted render, viewable in VitaShell.
sm64ds_vita_hostgen(GATE4A_GEN
    func_0205532c func_02055388 func_020553a4 func_02055780
    MI_SendGXCommandAsync MIi_FIFOCallback MIi_DMACallback OSi_EnterDmaCallback MI_WaitDma)
sm64ds_vita_gate(gate4a SMDS00005 "SM64DS Gate 4a" tests/smoke_gx.cpp
    DEFINES SM64DS_ROM_PATH="ux0:data/sm64ds/sm64ds.nds"
            SM64DS_OUT_DIR="ux0:data/sm64ds" PORT_REPO_ROOT=".")
target_sources(gate4a PRIVATE ${GATE4A_GEN})
target_link_libraries(gate4a PRIVATE ntr)

# ROM constants (sine table, default matrices): zero-filled storage generated by
# romdata.py --vita-loader, filled at startup from the ARM9 in the user's ROM by
# hal/romdata_rom.cpp (SM64DS_VITA_PREINIT makes the launcher call it).
set(ROMDATA_VITA "${CMAKE_BINARY_DIR}/host-src/romdata_vita.cpp")
add_custom_command(OUTPUT "${ROMDATA_VITA}"
    COMMAND ${Python3_EXECUTABLE} "${PORT_DIR}/tools/romdata.py" --vita-loader "${ROMDATA_VITA}"
    DEPENDS "${PORT_DIR}/tools/romdata.py"
    COMMENT "romdata storage for the Vita loader")
set_source_files_properties("${ROMDATA_VITA}" PROPERTIES
    INCLUDE_DIRECTORIES "${PORT_DIR}/hal")

# Gate 4b: the game's whole model pipeline -- SharedFilePtr loads Mario from the
# ROM, dExtSimpleModel_c rebases it, GX::LoadTex uploads textures into the
# (relocated) VRAM, Render walks bones/materials into the gate-4a pump.
# Writes ux0:data/sm64ds/smoke_model.bmp.
sm64ds_vita_hostgen(GATE4B_GEN
    func_02044b30 Geometry_MatrixMultiply3x3 func_02055998
    func_02044534 func_0204488c NormalizeVec3)
sm64ds_vita_gate(gate4b SMDS00006 "SM64DS Gate 4b" tests/smoke_model.cpp
    HAL hal/heap_globals.cpp hal/os_arena.cpp hal/heap_vtable.cpp hal/fs.cpp
        hal/romfs.cpp hal/romdata_rom.cpp
        hal/model_host.cpp hal/gx_upload_bridge.cpp hal/ctor_bridge.cpp
    SLICES slice_gate2.txt slice_gate3a.txt slice_gate3b.txt slice_gate4b.txt
    EXCLUDE ${GATE4A_GEN_SRC} ${GATE4B_GEN_SRC}
    DEFINES SM64DS_FS_ROM SM64DS_VITA_PREINIT SM64DS_PORT_GATE4B
            SM64DS_ROM_PATH="ux0:data/sm64ds/sm64ds.nds"
            SM64DS_OUT_DIR="ux0:data/sm64ds" PORT_REPO_ROOT=".")
target_sources(gate4b PRIVATE ${GATE4A_GEN} ${GATE4B_GEN} "${ROMDATA_VITA}")
target_link_libraries(gate4b PRIVATE ntr)

# Gate 5: the game's frame shape on the fiber runtime (ntr/rt.cpp: an ARM
# context switch stands in for Windows fibers). The Mad Piano plays its attack
# animation for 24 frames, yielding at VBlank like the real main loop; each
# frame is written to ux0:data/sm64ds/frames/frame_NN.bmp.
sm64ds_vita_gate(gate5 SMDS00007 "SM64DS Gate 5" tests/smoke_frames.cpp
    HAL hal/heap_globals.cpp hal/os_arena.cpp hal/heap_vtable.cpp hal/fs.cpp
        hal/romfs.cpp hal/romdata_rom.cpp
        hal/model_host.cpp hal/gx_upload_bridge.cpp hal/ctor_bridge.cpp
    SLICES slice_gate2.txt slice_gate3a.txt slice_gate3b.txt slice_gate4b.txt
    EXCLUDE ${GATE4A_GEN_SRC} ${GATE4B_GEN_SRC}
    DEFINES SM64DS_FS_ROM SM64DS_VITA_PREINIT SM64DS_PORT_GATE4B
            SM64DS_ROM_PATH="ux0:data/sm64ds/sm64ds.nds"
            SM64DS_OUT_DIR="ux0:data/sm64ds" PORT_REPO_ROOT=".")
target_sources(gate5 PRIVATE ${GATE4A_GEN} ${GATE4B_GEN} "${ROMDATA_VITA}")
target_link_libraries(gate5 PRIVATE ntr)

# Gate 4d: EVERY model in the ROM through the game's pipeline (load, rebase,
# texture upload, render). No SEH on the Vita: each model is logged before it
# runs, so a fault leaves its name as the last line of gate4d.log. Model
# components leak by design in this soak (the PC run needs a 256 MB arena),
# hence the larger heap.
sm64ds_vita_gate(gate4d SMDS00008 "SM64DS Gate 4d" tests/smoke_soak.cpp
    HAL hal/heap_globals.cpp hal/os_arena.cpp hal/heap_vtable.cpp hal/fs.cpp
        hal/romfs.cpp hal/romdata_rom.cpp
        hal/model_host.cpp hal/gx_upload_bridge.cpp hal/ctor_bridge.cpp
    SLICES slice_gate2.txt slice_gate3a.txt slice_gate3b.txt slice_gate4b.txt
    EXCLUDE ${GATE4A_GEN_SRC} ${GATE4B_GEN_SRC}
    DEFINES SM64DS_FS_ROM SM64DS_VITA_PREINIT SM64DS_PORT_GATE4B
            SM64DS_ROM_PATH="ux0:data/sm64ds/sm64ds.nds" PORT_REPO_ROOT="."
            SM64DS_VITA_HEAP_MB=192 SM64DS_HOST_ARENA_MB_DEFAULT=160
            SM64DS_TEST_MAIN_ARGS)
target_sources(gate4d PRIVATE ${GATE4A_GEN} ${GATE4B_GEN} "${ROMDATA_VITA}")
target_link_libraries(gate4d PRIVATE ntr)

# Gate 4c: the piano posed by its BCA (frame 0) through the game's walk.
# Writes ux0:data/sm64ds/smoke_anim.bmp.
sm64ds_vita_gate(gate4c SMDS00009 "SM64DS Gate 4c" tests/smoke_anim.cpp
    HAL hal/heap_globals.cpp hal/os_arena.cpp hal/heap_vtable.cpp hal/fs.cpp
        hal/romfs.cpp hal/romdata_rom.cpp
        hal/model_host.cpp hal/gx_upload_bridge.cpp hal/ctor_bridge.cpp
    SLICES slice_gate2.txt slice_gate3a.txt slice_gate3b.txt slice_gate4b.txt
    EXCLUDE ${GATE4A_GEN_SRC} ${GATE4B_GEN_SRC}
    DEFINES SM64DS_FS_ROM SM64DS_VITA_PREINIT SM64DS_PORT_GATE4B
            SM64DS_ROM_PATH="ux0:data/sm64ds/sm64ds.nds"
            SM64DS_OUT_DIR="ux0:data/sm64ds" PORT_REPO_ROOT=".")
target_sources(gate4c PRIVATE ${GATE4A_GEN} ${GATE4B_GEN} "${ROMDATA_VITA}")
target_link_libraries(gate4c PRIVATE ntr)

# Gate 5b: every animation with a skeleton-compatible sibling model, posed at
# its first/middle/last frame and rendered. Logs each animation before it runs
# and the game heap's free memory every 25 pairs (the PC run needs a 512 MB
# arena because pairs leak by design; this measures what the Vita needs).
sm64ds_vita_gate(gate5b SMDS00010 "SM64DS Gate 5b" tests/smoke_soak_anim.cpp
    HAL hal/heap_globals.cpp hal/os_arena.cpp hal/heap_vtable.cpp hal/fs.cpp
        hal/romfs.cpp hal/romdata_rom.cpp
        hal/model_host.cpp hal/gx_upload_bridge.cpp hal/ctor_bridge.cpp
    SLICES slice_gate2.txt slice_gate3a.txt slice_gate3b.txt slice_gate4b.txt
    EXCLUDE ${GATE4A_GEN_SRC} ${GATE4B_GEN_SRC}
    DEFINES SM64DS_FS_ROM SM64DS_VITA_PREINIT SM64DS_PORT_GATE4B
            SM64DS_ROM_PATH="ux0:data/sm64ds/sm64ds.nds" PORT_REPO_ROOT="."
            SM64DS_VITA_HEAP_MB=192 SM64DS_HOST_ARENA_MB_DEFAULT=160
            SM64DS_TEST_MAIN_ARGS)
target_sources(gate5b PRIVATE ${GATE4A_GEN} ${GATE4B_GEN} "${ROMDATA_VITA}")
target_link_libraries(gate5b PRIVATE ntr)

# Gate 6: the 2D sprite engine (OAM emit, affine params, upload to the
# relocated OAM, and the OBJ scan-out). Writes ux0:data/sm64ds/smoke_oam.bmp.
# Not linked on the Vita: hal/oam_bridge.cpp, hal/anim_bridge.cpp and
# hal/anim_ctor_bridge.cpp are MSVC-only forwarders from C spellings to the
# C++ methods; under GCC those spellings ARE the methods.
sm64ds_vita_hostgen(GATE6_GEN _ZN3OAM4LoadEv)
sm64ds_vita_gate(gate6 SMDS00011 "SM64DS Gate 6" tests/smoke_oam.cpp
    HAL hal/heap_globals.cpp hal/os_arena.cpp hal/heap_vtable.cpp hal/fs.cpp
        hal/romfs.cpp hal/romdata_rom.cpp
        hal/model_host.cpp hal/gx_upload_bridge.cpp hal/ctor_bridge.cpp
    SLICES slice_gate2.txt slice_gate3a.txt slice_gate3b.txt slice_gate4b.txt slice_gate6.txt
    EXCLUDE ${GATE4A_GEN_SRC} ${GATE4B_GEN_SRC} ${GATE6_GEN_SRC}
    DEFINES SM64DS_FS_ROM SM64DS_VITA_PREINIT SM64DS_PORT_GATE4B
            SM64DS_ROM_PATH="ux0:data/sm64ds/sm64ds.nds"
            SM64DS_OUT_DIR="ux0:data/sm64ds" PORT_REPO_ROOT=".")
target_sources(gate6 PRIVATE ${GATE4A_GEN} ${GATE4B_GEN} ${GATE6_GEN} "${ROMDATA_VITA}")
target_link_libraries(gate6 PRIVATE ntr)

# Gate 7: dExtAnmModel_c -- the game itself advances the animation (speed,
# loop wrap) frame by frame on the game thread.
sm64ds_vita_gate(gate7 SMDS00012 "SM64DS Gate 7" tests/smoke_modelanim.cpp
    HAL hal/heap_globals.cpp hal/os_arena.cpp hal/heap_vtable.cpp hal/fs.cpp
        hal/romfs.cpp hal/romdata_rom.cpp
        hal/model_host.cpp hal/gx_upload_bridge.cpp hal/ctor_bridge.cpp
    SLICES slice_gate2.txt slice_gate3a.txt slice_gate3b.txt slice_gate4b.txt slice_gate7.txt
    EXCLUDE ${GATE4A_GEN_SRC} ${GATE4B_GEN_SRC}
    DEFINES SM64DS_FS_ROM SM64DS_VITA_PREINIT SM64DS_PORT_GATE4B
            SM64DS_ROM_PATH="ux0:data/sm64ds/sm64ds.nds"
            SM64DS_OUT_DIR="ux0:data/sm64ds" PORT_REPO_ROOT=".")
target_sources(gate7 PRIVATE ${GATE4A_GEN} ${GATE4B_GEN} "${ROMDATA_VITA}")
target_link_libraries(gate7 PRIVATE ntr)

# Gate 8: collision. The castle grounds KCL (handle 1941) loads through the
# card seam, LoadFile rebases it, and the game's octree walk answers 25
# vertical ground probes. On the Vita the real dBgW_Kc vtable comes out of
# its key-function TU (GCC, Itanium layout = the ROM's), so the synthetic MSVC
# table in hal/clsn_vtable.cpp and the C-name forwarders are MSVC-only.
# The walk is the byte-matched dBgW_Kc::DetectClsn(dBgCh_Lin&) in
# src/engine/collision/d_bg_w_kc_itcm.cpp; the old host draft in port/unmatched
# that slice_gate8.txt still names predates that match, no longer compiles
# (its private dBgCh_Lin lacks SetClsnPos) and would define the method twice.
# Also not linked: hal/bg_ctor_bridge.cpp (MSVC vtable-emission traps and C-name
# ctor forwarders; under GCC src/ defines all of them) and hal/cstd_div.c (the
# host divide; here src/cstd.cpp comes through hostgen and the divide runs on
# the ntr DIV register model, as on the DS).
sm64ds_vita_hostgen(GATE8_GEN _ZN4cstd4sqrtEy _ZN4cstd11fdiv_resultEv
    _ZN4cstd16reciprocal_asyncEi Matrix4x3_ApplyInPlaceToScale)
sm64ds_vita_gate(gate8 SMDS00013 "SM64DS Gate 8" tests/smoke_clsn.cpp
    HAL hal/heap_globals.cpp hal/os_arena.cpp hal/heap_vtable.cpp hal/fs.cpp
        hal/romfs.cpp hal/romdata_rom.cpp
        hal/model_host.cpp hal/gx_upload_bridge.cpp hal/ctor_bridge.cpp
        hal/clsn_vtable.cpp
    SLICES slice_gate2.txt slice_gate3a.txt slice_gate3b.txt slice_gate4b.txt slice_gate8.txt
           vita/slice_gate8_extra.txt
    EXCLUDE ${GATE4A_GEN_SRC} ${GATE4B_GEN_SRC} ${GATE8_GEN_SRC}
            "${REPO_DIR}/port/unmatched/MeshCollider_DetectClsn_RaycastLine.cpp"
    DEFINES SM64DS_FS_ROM SM64DS_VITA_PREINIT SM64DS_PORT_GATE4B SM64DS_PORT_GATE8
            SM64DS_ROM_PATH="ux0:data/sm64ds/sm64ds.nds" PORT_REPO_ROOT=".")
target_sources(gate8 PRIVATE ${GATE4A_GEN} ${GATE4B_GEN} ${GATE8_GEN} "${ROMDATA_VITA}")
target_link_libraries(gate8 PRIVATE ntr)

# Gate 9: the actor framework. A real actor (daObjYajirusi_c, the arrow sign)
# comes out of its factory, loads its model and collider, behaves and
# renders -- every lifecycle call through its real vptr. Uses the GCC edition
# of the test (port/vita/tests/smoke_actor.cpp; see its header for why).
# Under GCC the compiler emits every vtable in this hierarchy with the ROM's
# slot order, so hal/actor_vtables.cpp, hal/actor_ctor_bridge.cpp,
# hal/cxxname_bridge.cpp and hal/bg_ctor_bridge.cpp (synthetic MSVC tables,
# C-name forwarders, a C transcription of fBase_c's ctor that src/ now has as
# real C++) are not linked; port/vita/actor_host.cpp holds the storage and the
# few primitives no sliced TU defines yet.
sm64ds_vita_hostgen(GATE9_GEN _ZN7fBase_c21AfterCleanupResourcesEj)
sm64ds_vita_gate(gate9 SMDS00014 "SM64DS Gate 9" vita/tests/smoke_actor.cpp
    HAL hal/heap_globals.cpp hal/os_arena.cpp hal/heap_vtable.cpp hal/fs.cpp
        hal/romfs.cpp hal/romdata_rom.cpp
        hal/model_host.cpp hal/gx_upload_bridge.cpp hal/ctor_bridge.cpp
        hal/clsn_vtable.cpp vita/actor_host.cpp
    SLICES slice_gate2.txt slice_gate3a.txt slice_gate3b.txt slice_gate4b.txt
           slice_gate7.txt slice_gate8.txt vita/slice_gate8_extra.txt slice_gate9.txt
           vita/slice_gate9_extra.txt
    EXCLUDE ${GATE4A_GEN_SRC} ${GATE4B_GEN_SRC} ${GATE8_GEN_SRC} ${GATE9_GEN_SRC}
            "${REPO_DIR}/port/unmatched/MeshCollider_DetectClsn_RaycastLine.cpp"
    DEFINES SM64DS_FS_ROM SM64DS_VITA_PREINIT SM64DS_PORT_GATE4B SM64DS_PORT_GATE8
            SM64DS_ROM_PATH="ux0:data/sm64ds/sm64ds.nds" PORT_REPO_ROOT=".")
target_sources(gate9 PRIVATE ${GATE4A_GEN} ${GATE4B_GEN} ${GATE8_GEN} ${GATE9_GEN} "${ROMDATA_VITA}")
# Drop shadows stay deferred (see port/vita/actor_host.cpp).
target_link_options(gate9 PRIVATE
    -Wl,--wrap=_ZN17dExtShadowModel_c10InitCuboidEv
    -Wl,--wrap=_ZN8dActor_c18DropShadowScaleXYZER17dExtShadowModel_cR9Matrix4x3iiih)
target_link_libraries(gate9 PRIVATE ntr)

# ---- measurement only (not part of the gate patches) ----
if(SM64DS_FULL_PROBE)
    sm64ds_vita_gate(fullprobe SMDS00099 "SM64DS probe" vita/probe/probe_main.cpp
        HAL hal/heap_globals.cpp hal/fs.cpp hal/romfs.cpp hal/romimage_rt.cpp vita/game_host.cpp
        SLICES vita/slice_full.txt
        DEFINES SM64DS_FS_ROM PORT_REPO_ROOT=".")
    target_link_libraries(fullprobe PRIVATE ntr)
    set_source_files_properties("${REPO_DIR}/src/SharedFilePtr.cpp" "${PORT_DIR}/hal/fs.cpp"
        "${REPO_DIR}/src/engine/heap/Heap.cpp"
        "${CMAKE_BINARY_DIR}/host-src/src/runtime/graphics/fader/d_fader.cpp"
        "${CMAKE_BINARY_DIR}/host-src/src/engine/gx/G3X.cpp"
        "${REPO_DIR}/src/runtime/graphics/fader/d_fader.cpp" "${REPO_DIR}/src/engine/gx/G3X.cpp"
        TARGET_DIRECTORY fullprobe PROPERTIES COMPILE_DEFINITIONS SM64DS_PORT_FULL)
endif()

# ntr for the full game: identical, except that the game's own interrupt
# table, OS_IrqHandler logic and BIOS waits run on the host
# (src/engine/os/OS_Interrupt.cpp under SM64DS_PORT_FULL, port/vita/irq_host.cpp)
# instead of ntr's reduced registry.
get_target_property(NTR_SOURCES ntr SOURCES)
add_library(ntr_full STATIC ${NTR_SOURCES})
target_include_directories(ntr_full PUBLIC "${PORT_DIR}/ntr/include")
target_compile_definitions(ntr_full PUBLIC NTR_RELOCATE_REGIONS PRIVATE NTR_GAME_OWNS_IRQ)
target_link_libraries(ntr_full PUBLIC pthread)
target_compile_options(ntr_full PRIVATE ${SM64DS_FLAGS})

# Gate 10: THE WHOLE GAME. Every src/ TU (port/vita/slice_full.txt) builds into
# one object library; port/tools/romimage.py then reads what those objects
# define and generates storage for the game globals that live only in the ROM
# image, plus the table of DS pointers inside them; the executable links the
# lot. The smoke loads all of it from the user's dump at startup and follows
# pointer chains across modules (spawn table -> overlay profile -> factory);
# the game itself does not run yet.
sm64ds_read_slice(vita/slice_full.txt G10_SRCS)
sm64ds_vita_auto_hostgen(G10_SRCS)
sm64ds_honor_cpp_markers(G10_SRCS)
set(G10_HAL "")
foreach(h hal/heap_globals.cpp hal/fs.cpp hal/romfs.cpp hal/romimage_rt.cpp vita/game_host.cpp
          vita/os_thread_host.cpp vita/irq_host.cpp vita/arena_host.cpp vita/overlay_host.cpp
          vita/timers_host.cpp)
    list(APPEND G10_HAL "${PORT_DIR}/${h}")
endforeach()
set_source_files_properties(${G10_HAL} PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/vita_log.h")
# the game's runtime names that the host libraries also define stay the game's
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/game_private_names.txt" G10_PRIV_LINES)
set(G10_PRIV_DEFS "")
foreach(l IN LISTS G10_PRIV_LINES)
    string(STRIP "${l}" l)
    if(NOT l STREQUAL "" AND NOT l MATCHES "^#")
        list(APPEND G10_PRIV_DEFS "${l}=sm64ds_game_${l}")
    endif()
endforeach()
set_source_files_properties(${G10_SRCS} PROPERTIES COMPILE_DEFINITIONS "${G10_PRIV_DEFS}")
add_library(gate10_game OBJECT ${G10_SRCS} ${G10_HAL})
target_include_directories(gate10_game PRIVATE "${PORT_DIR}" "${REPO_DIR}/include"
    "${PORT_DIR}/ntr/include" "${PORT_DIR}/hal")
target_compile_definitions(gate10_game PRIVATE SM64DS_PLATFORM_PC SM64DS_PORT_FULL
    SM64DS_FS_ROM SM64DS_GATE_NAME="gate10" PORT_REPO_ROOT="."
    SM64DS_ROM_PATH="ux0:data/sm64ds/sm64ds.nds" NTR_RELOCATE_REGIONS)
target_compile_options(gate10_game PRIVATE ${SM64DS_FLAGS})
add_dependencies(gate10_game ntr_full)

execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libstdc++.a
                OUTPUT_VARIABLE G10_LIBSTDCXX OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libc.a
                OUTPUT_VARIABLE G10_LIBC OUTPUT_STRIP_TRAILING_WHITESPACE)
set(G10_GEN "${CMAKE_BINARY_DIR}/romimage/romimage_gen.cpp")
set(G10_LD "${CMAKE_BINARY_DIR}/romimage/romimage_alias.ld")
set(G10_INIT_LD "${CMAKE_BINARY_DIR}/romimage/romimage_init.ld")
add_custom_command(OUTPUT "${G10_GEN}" "${G10_LD}" "${G10_INIT_LD}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/romimage"
    COMMAND ${Python3_EXECUTABLE} "${PORT_DIR}/tools/romimage.py"
            --config "${REPO_DIR}/config/arm9" --manifests "${REPO_DIR}/config/tu_manifest.d"
            --nm "${CMAKE_NM}" --objects $<TARGET_OBJECTS:gate10_game> $<TARGET_FILE:ntr_full>
            "${G10_LIBSTDCXX}" "${G10_LIBC}"
            --out-cpp "${G10_GEN}" --out-ld "${G10_LD}"
            --sources "${CMAKE_CURRENT_SOURCE_DIR}/slice_full.txt" --repo "${REPO_DIR}"
            --out-init-ld "${G10_INIT_LD}"
    DEPENDS gate10_game ntr_full "${PORT_DIR}/tools/romimage.py" "${PORT_DIR}/tools/romimage_raw_data.txt"
    COMMAND_EXPAND_LISTS
    COMMENT "romimage: storage and pointer table for the ROM-resident globals")
set_source_files_properties("${G10_GEN}" PROPERTIES GENERATED TRUE
    COMPILE_OPTIONS "-w;-fdata-sections")
# One target owns the generation: gate10, gate11 (and the full probe) each
# compile romimage_gen.cpp, and with a parallel build two of them could run the
# command at once and interleave their writes (seen: a cut-in-half line in
# romimage_alias.ld). They all wait for this target instead.
add_custom_target(romimage_gen DEPENDS "${G10_GEN}" "${G10_LD}" "${G10_INIT_LD}")
set(G10_TEST "${PORT_DIR}/vita/tests/smoke_romimage.cpp")
set_source_files_properties("${G10_TEST}" PROPERTIES
    COMPILE_DEFINITIONS "main=sm64ds_test_main"
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/vita_log.h")
add_executable(gate10 "${CMAKE_CURRENT_SOURCE_DIR}/vita_main.cpp" "${G10_TEST}" "${G10_GEN}"
    $<TARGET_OBJECTS:gate10_game>)
add_dependencies(gate10 romimage_gen)
target_include_directories(gate10 PRIVATE "${PORT_DIR}" "${REPO_DIR}/include"
    "${PORT_DIR}/ntr/include" "${PORT_DIR}/hal")
target_compile_definitions(gate10 PRIVATE SM64DS_PLATFORM_PC SM64DS_GATE_NAME="gate10"
    SM64DS_ROM_PATH="ux0:data/sm64ds/sm64ds.nds")
target_compile_options(gate10 PRIVATE ${SM64DS_FLAGS})
target_link_libraries(gate10 PRIVATE ntr_full)
target_link_options(gate10 PRIVATE "-Wl,${G10_LD}" "-Wl,-T,${G10_INIT_LD}"
    -Wl,--wrap=__register_global_object)
set_property(TARGET gate10 APPEND PROPERTY LINK_DEPENDS "${G10_LD}")
vita_create_self(gate10.self gate10)
vita_create_vpk(gate10.vpk SMDS00015 gate10.self VERSION 01.00 NAME "SM64DS Gate 10")

# Gate 11: the game boots by itself. Gate 10's objects, run for real: ROM data,
# the ARM9's static initializers, then NitroMain on the frame loop -- DS threads
# on host threads, VBlank through the game's own handler table, overlays loaded
# by port/vita/overlay_host.cpp (the four overlay-loader entry points are
# wrapped). Logs how far it gets and saves boot_<frame>_{top,bottom,3d}.bmp;
# on the Vita it then stays running, playable.
set(G11_TEST "${PORT_DIR}/vita/tests/smoke_boot.cpp")
set_source_files_properties("${G11_TEST}" PROPERTIES
    COMPILE_DEFINITIONS "main=sm64ds_test_main"
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/vita_log.h")
# gate 11's own host files log to the same file
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/arm7_host.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/card_host.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/input_host.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/model_trace_host.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/vita_display.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/snd_host.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/savestate_host.cpp"
    PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/vita_log.h")
enable_language(ASM)
add_executable(gate11 "${CMAKE_CURRENT_SOURCE_DIR}/vita_main.cpp" "${G11_TEST}" "${G10_GEN}"
    "${CMAKE_CURRENT_SOURCE_DIR}/boot_trace.S" "${CMAKE_CURRENT_SOURCE_DIR}/arm7_host.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/card_host.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/vita_display.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/input_host.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/heap_bridge_host.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/model_trace_host.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/snd_host.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/savestate_host.cpp"
    $<TARGET_OBJECTS:gate10_game>)
add_dependencies(gate11 romimage_gen)
target_include_directories(gate11 PRIVATE "${PORT_DIR}" "${REPO_DIR}/include"
    "${PORT_DIR}/ntr/include" "${PORT_DIR}/hal")
target_compile_definitions(gate11 PRIVATE SM64DS_PLATFORM_PC SM64DS_GATE_NAME="gate11"
    SM64DS_ROM_PATH="ux0:data/sm64ds/sm64ds.nds" SM64DS_OUT_DIR="ux0:data/sm64ds"
    SM64DS_VITA_HEAP_MB=192)
target_compile_options(gate11 PRIVATE ${SM64DS_FLAGS})
target_link_libraries(gate11 PRIVATE ntr_full)
# the live view (port/vita/vita_display.cpp) and the buttons and touch
# panels (port/vita/input_host.cpp) -- the real VitaSDK only
foreach(lib SceDisplay_stub SceCtrl_stub SceTouch_stub ScePower_stub SceAudio_stub)
    if(EXISTS "$ENV{VITASDK}/arm-vita-eabi/lib/lib${lib}.a")
        target_link_libraries(gate11 PRIVATE ${lib})
    endif()
endforeach()
# Save states (port/vita/savestate_host.cpp) hold the game's own globals and
# none of the port's. Which addresses those are comes from the link map:
# port/tools/state_ranges.py reads gate11.map after the link and writes every
# data/bss input section of the game's objects (all of gate10_game but the
# port's files there, and the ROM-image storage) as ranges, relative to one
# game global, into state_ranges.bin, which the VPK carries. The game's
# memory layout stays exactly what the link gives it -- a linker script that
# regrouped it once changed the game's behaviour (the castle's particles).
target_link_options(gate11 PRIVATE "-Wl,-Map=${CMAKE_BINARY_DIR}/gate11.map")
add_custom_command(TARGET gate11 POST_BUILD
    COMMAND ${Python3_EXECUTABLE} "${PORT_DIR}/tools/state_ranges.py"
        "${CMAKE_BINARY_DIR}/gate11.map" "${CMAKE_BINARY_DIR}/state_ranges.bin"
    COMMENT "save-state ranges from the link map")
target_link_options(gate11 PRIVATE "-Wl,${G10_LD}" "-Wl,-T,${G10_INIT_LD}"
    -Wl,--wrap=FS_LoadOverlay -Wl,--wrap=func_02018c00
    -Wl,--wrap=func_02018b64 -Wl,--wrap=FS_StartOverlay
    # the ARM7 stand-in at the PXI FIFO (port/vita/arm7_host.cpp)
    -Wl,--wrap=PXI_InitFifo -Wl,--wrap=PXI_SendWordByFifo
    # the card's ROM from the dump (port/vita/card_host.cpp)
    -Wl,--wrap=CARDi_ReadRom
    # the game's Crash() reports its caller and ends the test (tests/smoke_boot.cpp)
    -Wl,--wrap=Crash
    # where an unexpected C++ cleanup ends (tests/smoke_boot.cpp)
    -Wl,--wrap=__cxa_end_cleanup
    # the sprite lists, logged (tests/smoke_boot.cpp)
    -Wl,--wrap=_ZN3OAM5ResetEv -Wl,--wrap=_ZN3OAM4LoadEv
    # an overlay's static objects, registered once (port/vita/overlay_host.cpp)
    -Wl,--wrap=__register_global_object
    -Wl,--wrap=_ZN17dExtSimpleModel_c13LoadTexAndPalER8BMD_File
    # malloc blocks reaching the game's heap (port/vita/heap_bridge_host.cpp)
    -Wl,--wrap=_ZN22ExpandingHeapAllocator10DeallocateEPv
    -Wl,--wrap=_ZN3OAM6RenderEbP7OamAttriiiiiiii -Wl,--wrap=_ZN3OAM6RenderEbP7OamAttriiiiP9Matrix2x2
    # a one-frame model trace after a capture, for the heads (port/vita/model_trace_host.cpp)
    -Wl,--wrap=_ZN15ModelComponents6RenderEP9Matrix4x3P7Vector3
    -Wl,--wrap=_ZN17dExtSimpleModel_c6RenderEPK7Vector3 -Wl,--wrap=MulMat4x3Mat4x3
    -Wl,--wrap=_ZN14dExtAnmModel_c6RenderEPK7Vector3
    # linker-script constants the SDK reads as symbol addresses (OS_InitThread),
    # the values of the game's own lcf: no system-mode stack of its own, 0x600 IRQ
    -Wl,--defsym=SDK_SYS_STACKSIZE=0 -Wl,--defsym=SDK_IRQ_STACKSIZE=0x600)
# the start-up steps port/vita/boot_trace.S logs
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/boot_trace_steps.txt" G11_TRACE)
foreach(t IN LISTS G11_TRACE)
    if(NOT t MATCHES "^#" AND NOT t STREQUAL "")
        target_link_options(gate11 PRIVATE "-Wl,--wrap=${t}")
    endif()
endforeach()
set_property(TARGET gate11 APPEND PROPERTY LINK_DEPENDS "${G10_LD}" "${G10_INIT_LD}")
# The live view on the GPU (port/vita/vita_gl3d.cpp) when port/vita/deps.sh has
# built vitaGL; without it, the CPU draws as before. Its own library: vitaGL's
# header is built with VitaSDK's default flags, not the game's.
set(SM64DS_VITA_DEPS "${REPO_DIR}/build/vita-deps/prefix" CACHE PATH "Where port/vita/deps.sh built vitaGL")
if(EXISTS "${SM64DS_VITA_DEPS}/lib/libvitaGL.a" AND EXISTS "$ENV{VITASDK}/arm-vita-eabi/lib/libSceGxm_stub.a")
    message(STATUS "gate11: the live view draws on the GPU (vitaGL in ${SM64DS_VITA_DEPS})")
    add_library(sm64ds_gl STATIC "${CMAKE_CURRENT_SOURCE_DIR}/vita_gl3d.cpp")
    target_include_directories(sm64ds_gl PRIVATE "${SM64DS_VITA_DEPS}/include" "${PORT_DIR}/ntr/include")
    target_compile_definitions(sm64ds_gl PRIVATE SM64DS_VITAGL)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/vita_gl3d.cpp"
        PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/vita_log.h")
    target_compile_definitions(gate11 PRIVATE SM64DS_VITAGL)
    find_library(SM64DS_MODULEMGR NAMES SceKernelModulemgr_stub SceKernelModuleMgr_stub SceModulemgr_stub
        PATHS "$ENV{VITASDK}/arm-vita-eabi/lib" NO_DEFAULT_PATH)
    if(NOT SM64DS_MODULEMGR)
        set(SM64DS_MODULEMGR "")
    endif()
    target_link_libraries(gate11 PRIVATE sm64ds_gl
        "${SM64DS_VITA_DEPS}/lib/libvitaGL.a" "${SM64DS_VITA_DEPS}/lib/libvitashark.a"
        "${SM64DS_VITA_DEPS}/lib/libSceShaccCgExt.a" "${SM64DS_VITA_DEPS}/lib/libmathneon.a"
        taihen_stub SceShaccCg_stub SceGxm_stub SceDisplay_stub SceCommonDialog_stub SceAppMgr_stub
        SceLibKernel_stub ${SM64DS_MODULEMGR} SceSysmodule_stub SceKernelDmacMgr_stub SceIofilemgr_stub m)
    # more memory: vitaGL takes what the game's heap leaves
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
else()
    message(STATUS "gate11: vitaGL not built (port/vita/deps.sh): the live view draws on the CPU")
endif()
vita_create_self(gate11.self gate11)
# The bubble and the LiveArea: images the user supplies in port/vita/livearea/
# (icon0.png 128x128, bg.png 840x500, startup.png 280x158, optional pic0.png
# 960x544; 8-bit palette PNGs). They are not in the repository -- .gitignore
# keeps them out -- so a build without them gets the system's default bubble.
set(SM64DS_LIVEAREA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/livearea")
set(SM64DS_VPK_FILES "")
if(EXISTS "${SM64DS_LIVEAREA_DIR}/icon0.png")
    list(APPEND SM64DS_VPK_FILES FILE "${SM64DS_LIVEAREA_DIR}/icon0.png" sce_sys/icon0.png)
    message(STATUS "sm64ds-vita.vpk: bubble icon from livearea/icon0.png")
endif()
if(EXISTS "${SM64DS_LIVEAREA_DIR}/pic0.png")
    list(APPEND SM64DS_VPK_FILES FILE "${SM64DS_LIVEAREA_DIR}/pic0.png" sce_sys/pic0.png)
endif()
if(EXISTS "${SM64DS_LIVEAREA_DIR}/bg.png" AND EXISTS "${SM64DS_LIVEAREA_DIR}/startup.png")
    list(APPEND SM64DS_VPK_FILES
        FILE "${SM64DS_LIVEAREA_DIR}/bg.png" sce_sys/livearea/contents/bg.png
        FILE "${SM64DS_LIVEAREA_DIR}/startup.png" sce_sys/livearea/contents/startup.png
        FILE "${SM64DS_LIVEAREA_DIR}/template.xml" sce_sys/livearea/contents/template.xml)
    message(STATUS "sm64ds-vita.vpk: LiveArea background from livearea/bg.png")
endif()
vita_create_vpk(sm64ds-vita.vpk SMDS00016 gate11.self VERSION 01.00 NAME "Super Mario 64 DS"
    FILE "${CMAKE_BINARY_DIR}/state_ranges.bin" state_ranges.bin
    ${SM64DS_VPK_FILES})

# Offline redraw of a gate-11 display snapshot (port/tools/snapshot_render.cpp):
# a desktop/QEMU tool for renderer work, not part of the default build.
add_executable(snapshot_render EXCLUDE_FROM_ALL "${PORT_DIR}/tools/snapshot_render.cpp")
target_link_libraries(snapshot_render PRIVATE ntr)
target_compile_options(snapshot_render PRIVATE ${SM64DS_FLAGS})
