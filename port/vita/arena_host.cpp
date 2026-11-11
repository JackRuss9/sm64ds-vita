// The NitroSDK memory arenas for the full-game GCC build (PS Vita).
//
// src/engine/os/OS_Arena.cpp derives each arena's bounds from the DS memory
// map (main RAM after the static image and the largest overlay, the TCMs,
// shared and work RAM). Game code then carves its heaps out of those ranges
// with plain pointer arithmetic, which on the host must land in host memory.
// This file is that TU for the host: the same API and the same table (kept at
// its DS address, 0x027ffda0/0x027ffdc4, through ntr::host_ptr, so a direct
// reader still agrees), with every arena backed by a host block.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ntr/mmio.h"

namespace {
enum { MAIN = 0, MAINEX = 2, ITCM = 3, DTCM = 4, SHARED = 5, WRAM_MAIN = 6, COUNT = 9 };

// The DS main arena is ~2.7 MB (0x020xxxxx .. 0x023e0000); the host gives the
// game more room, which only ever makes allocation succeed where the DS's would.
#ifndef SM64DS_HOST_MAIN_ARENA_MB
#define SM64DS_HOST_MAIN_ARENA_MB 24
#endif
constexpr size_t SIZES[COUNT] = {
    (size_t)SM64DS_HOST_MAIN_ARENA_MB << 20,   // main
    0,                                          // (sub processor main)
    0,                                          // main ex: an 8 MB debug unit only
    0x2000,                                     // ITCM (the DS leaves ~8 KB)
    0x3000,                                     // DTCM (below the stacks)
    0x1000,                                     // shared
    0x8000,                                     // ARM9 work RAM
    0, 0,
};
unsigned *g_lo, *g_hi;
unsigned g_init_lo[COUNT], g_init_hi[COUNT];
bool g_blocks;

void blocks()
{
    if (g_blocks) return;
    g_blocks = true;
    g_lo = static_cast<unsigned *>(::ntr::host_ptr(0x027ffda0u));
    g_hi = static_cast<unsigned *>(::ntr::host_ptr(0x027ffdc4u));
    for (int i = 0; i < COUNT; ++i) {
        if (!SIZES[i]) continue;
        // 32-byte aligned, like the DS arenas
        char *raw = static_cast<char *>(malloc(SIZES[i] + 32));
        memset(raw, 0, SIZES[i] + 32);
        char *lo = reinterpret_cast<char *>((reinterpret_cast<uintptr_t>(raw) + 31) & ~uintptr_t(31));
        g_init_lo[i] = (unsigned)(uintptr_t)lo;
        g_init_hi[i] = (unsigned)(uintptr_t)(lo + SIZES[i]);
    }
}
}  // namespace

extern "C" {
/* Is `p` inside one of the arenas (the game's heaps are carved from them)?
   port/vita/heap_bridge_host.cpp tells game-heap blocks from malloc's by it. */
bool sm64ds_in_game_arena(const void *p)
{
    blocks();
    const unsigned a = (unsigned)(uintptr_t)p;
    for (int i = 0; i < COUNT; ++i)
        if (g_init_hi[i] && a >= g_init_lo[i] && a < g_init_hi[i]) return true;
    return false;
}

unsigned OS_GetInitArenaLo(int id) { blocks(); return (id >= 0 && id < COUNT) ? g_init_lo[id] : 0; }
unsigned OS_GetInitArenaHi(int id) { blocks(); return (id >= 0 && id < COUNT) ? g_init_hi[id] : 0; }
unsigned OS_GetArenaLo(int id) { blocks(); return g_lo[id]; }
unsigned OS_GetArenaHi(int id) { blocks(); return g_hi[id]; }
void OS_SetArenaLo(int id, unsigned v) { blocks(); g_lo[id] = v; }
void OS_SetArenaHi(int id, unsigned v) { blocks(); g_hi[id] = v; }

void OS_InitArena(void)
{
    static bool done;
    if (done) return;
    done = true;
    blocks();
    static const int ids[] = {MAIN, ITCM, DTCM, SHARED, WRAM_MAIN};
    for (int id : ids) {
        g_hi[id] = g_init_hi[id];
        g_lo[id] = g_init_lo[id];
    }
    g_lo[MAINEX] = g_hi[MAINEX] = 0;
}

/* only an 8 MB development unit has the extended main arena */
void OS_InitArenaEx(void) {}

unsigned OS_AllocFromArenaLo(int id, unsigned size, unsigned align)
{
    unsigned ptr = OS_GetArenaLo(id);
    if (ptr == 0) return 0;
    unsigned lo = (ptr + align - 1) & ~(align - 1);
    unsigned hi = (lo + size + align - 1) & ~(align - 1);
    if (hi > OS_GetArenaHi(id)) return 0;
    OS_SetArenaLo(id, hi);
    return lo;
}
}  // extern "C"
