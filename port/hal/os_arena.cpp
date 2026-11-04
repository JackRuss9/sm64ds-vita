// Host OS arena: the memory region SetupRootHeap carves the root heap from.
//
// On the DS these are the Nitro SDK's arena accessors (OS_GetArenaLo,
// OS_GetArenaHi, OS_SetArenaLo, OS_AllocFromArenaLo) and OS_InitAlloc,
// which lays the OS heap descriptors out at the bottom of the arena. On host the arena is one big malloc'd block, initialized on
// first use so the smoke needs no setup call.
#include <stdlib.h>

typedef unsigned int u32;

/* 8 MB: larger than the DS main-RAM arena. Builds without an environment
   (the PS Vita) set the soak gates' larger arena at compile time. */
#ifndef SM64DS_HOST_ARENA_MB_DEFAULT
#define SM64DS_HOST_ARENA_MB_DEFAULT 8
#endif
enum { HOST_ARENA = SM64DS_HOST_ARENA_MB_DEFAULT << 20 };

static char *g_lo, *g_hi;
static void arena_init(void)
{
    if (!g_lo) {
        /* soaks and tools can ask for more than the DS ever had */
        const char *env = getenv("SM64DS_HOST_ARENA_MB");
        size_t mb = env ? (size_t)atoi(env) : 0;
        size_t size = mb ? mb << 20 : (size_t)HOST_ARENA;
        g_lo = (char *)malloc(size);
        g_hi = g_lo + size;
    }
}

extern "C" {
// The arena id Heap::SetupRootHeap selects from. The accessors below ignore it,
// but the storage has to exist to link.
//
// Was spelled `char data_020a0ea4[4]'. Same word at 0x020a0ea4, which
// config/arm9/symbols.txt has always also named ROOT_HEAP_ARENA_ID; src/ now
// uses that name throughout and the address spelling appears nowhere, so this
// reference was left dangling and port_refcheck caught it.
//
// It is a u32 id, not a pointer: Heap::InitializeRootHeap stores 0 to select
// OS_ARENA_MAIN, and it is passed BY VALUE. The accessors took `void *' when
// the caller was a .c file spelling its own mangled name; the migrated caller
// declares them `u32', so they say u32 here too rather than relying on a
// 32-bit value and a 64-bit parameter agreeing on a host they do not.
u32 ROOT_HEAP_ARENA_ID;

int OS_GetArenaLo(u32) { arena_init(); return (int)(size_t)g_lo; }   /* arena lo */
int OS_GetArenaHi(u32) { arena_init(); return (int)(size_t)g_hi; }   /* arena hi */

/* On the DS, OS_InitAlloc puts the arena's OSHeapInfo and its last argument's
 * worth of heap descriptors at lo and returns the 32-aligned memory after them.
 * The host keeps no OS heaps, so it only rounds lo up by that argument. */
int OS_InitAlloc(u32, int lo, int hi, int align)
{
    (void)hi;
    return (lo + align - 1) & ~(align - 1);
}

void OS_SetArenaLo(u32, int newLo) { g_lo = (char *)(size_t)newLo; }  /* set lo */

/* carve `size` bytes aligned `align` from the low side */
void *OS_AllocFromArenaLo(u32, int size, int align)
{
    arena_init();
    char *p = (char *)(((size_t)g_lo + align - 1) & ~(size_t)(align - 1));
    if (p + size > g_hi)
        return 0;
    g_lo = p + size;
    return p;
}
}
