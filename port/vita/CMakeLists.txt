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
set(SM64DS_FLAGS -fno-strict-aliasing -w)
# vita_create_self needs the relocations kept in the ELF.
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-q")

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

# One gate = its smoke test (main renamed), its HAL files, its slice, the
# launcher, packed into a VPK with its own title ID.
function(sm64ds_vita_gate name title_id label test_src)
    cmake_parse_arguments(G "" "" "HAL;SLICES;DEFINES" ${ARGN})
    set(srcs "")
    foreach(manifest IN LISTS G_SLICES)
        sm64ds_read_slice(${manifest} slice)
        list(APPEND srcs ${slice})
    endforeach()
    list(REMOVE_DUPLICATES srcs)
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
    target_include_directories(${name} PRIVATE "${PORT_DIR}" "${REPO_DIR}/include")
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
# No hal/mem_delete2.cpp here: Heap.cpp (slice_gate3a) defines
# Memory::operator_delete2, and under GCC that IS the _ZN6Memory16operator_delete2EPv
# symbol the class headers reference, so the HAL copy would be a duplicate.
sm64ds_vita_gate(gate3a SMDS00003 "SM64DS Gate 3a" tests/smoke_roots.cpp
    HAL hal/heap_globals.cpp hal/os_arena.cpp hal/heap_vtable.cpp
    SLICES slice_gate2.txt slice_gate3a.txt)

# Gate 3b: real game files through SharedFilePtr, read straight from the
# cartridge dump (hal/romfs.cpp) -- copy your own EUR .nds to
# ux0:data/sm64ds/sm64ds.nds. smoke_fs's expected values were computed from
# that ROM, so a PASS cross-checks the reader against the real data.
sm64ds_vita_gate(gate3b SMDS00004 "SM64DS Gate 3b" tests/smoke_fs.cpp
    HAL hal/heap_globals.cpp hal/os_arena.cpp hal/heap_vtable.cpp
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
    ${PORT_DIR}/ntr/ppu.cpp ${PORT_DIR}/ntr/lz77.cpp ${PORT_DIR}/ntr/rt.cpp
    ${PORT_DIR}/ntr/runtime.cpp ${PORT_DIR}/ntr/fs.cpp ${PORT_DIR}/ntr/bmd.cpp)
target_include_directories(ntr PUBLIC "${PORT_DIR}/ntr/include")
target_compile_definitions(ntr PUBLIC NTR_RELOCATE_REGIONS)
target_compile_options(ntr PRIVATE ${SM64DS_FLAGS})
set_source_files_properties(
    ${PORT_DIR}/ntr/io.cpp ${PORT_DIR}/ntr/gx.cpp ${PORT_DIR}/ntr/texture.cpp
    ${PORT_DIR}/ntr/ppu.cpp ${PORT_DIR}/ntr/lz77.cpp ${PORT_DIR}/ntr/rt.cpp
    ${PORT_DIR}/ntr/runtime.cpp ${PORT_DIR}/ntr/fs.cpp ${PORT_DIR}/ntr/bmd.cpp
    PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/vita_log.h")

# hostgen: MMIO-touching src/ files are built from transformed copies under the
# build tree (src/ is never edited). Same tool and resolution as the PC build;
# python3 explicitly, since WSL/Linux usually has no `python`.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
function(sm64ds_vita_hostgen out_var)
    set(generated "")
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
    endforeach()
    set(${out_var} "${generated}" PARENT_SCOPE)
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
