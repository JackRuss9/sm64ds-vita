// Two allocators, one program (full-game GCC build, gate 11 on).
//
// On the DS there is one heap: the global operator new and delete, the
// classes' own operator new/delete and Memory::Allocate all end in the game's
// heaps. On the host the global operator new stays the C library's -- the
// port's own code (renderers, containers) must not eat the DS-sized game heap
// -- so a game object can be allocated by one and freed by the other:
//
//   `new dExtAnmTexPat_c` (global new: malloc) ... `delete mTexSeq` (the
//   class's operator delete: the game heap) -- the end of the intro cutscene
//   freed a malloc block into the game heap and faulted there;
//   and the reverse, a class with its own operator new freed by a plain
//   `delete` (free() of a game-heap block).
//
// Both directions are settled by where the block is: every game heap is
// carved from the SDK's arenas, which the host backs with its own blocks
// (port/vita/arena_host.cpp). A block outside them came from malloc and goes
// to free(); one inside goes to the game's heap.
#include <stdint.h>
#include <stdlib.h>

#include <new>


extern "C" {
int __real__ZN22ExpandingHeapAllocator10DeallocateEPv(void *self, void *ptr);
void _ZN6Memory10DeallocateEPv(void *ptr);          /* Memory::Deallocate(void *) */
}

extern "C" bool sm64ds_in_game_arena(const void *p);   /* port/vita/arena_host.cpp */
static inline bool in_game_ram(const void *p) { return sm64ds_in_game_arena(p); }

/* The game's heaps freeing a block: one that is not theirs is malloc's. */
extern "C" int __wrap__ZN22ExpandingHeapAllocator10DeallocateEPv(void *self, void *ptr)
{
    if (ptr && !in_game_ram(ptr)) {
        free(ptr);
        return 1;
    }
    return __real__ZN22ExpandingHeapAllocator10DeallocateEPv(self, ptr);
}

/* The global operator delete: a game-heap block goes back to the game. */
static void host_delete(void *p) noexcept
{
    if (!p) return;
    if (in_game_ram(p)) _ZN6Memory10DeallocateEPv(p);
    else free(p);
}
void operator delete(void *p) noexcept { host_delete(p); }
void operator delete[](void *p) noexcept { host_delete(p); }
void operator delete(void *p, std::size_t) noexcept { host_delete(p); }
void operator delete[](void *p, std::size_t) noexcept { host_delete(p); }
