// Overlay loading for the full-game GCC build (PS Vita).
//
// On the DS, FS_LoadOverlay (src/FS_LoadOverlay.c) reads the overlay's OVT
// record (func_02018c00 = FS_LoadOverlayInfo), streams its code and data into
// the RAM range the record names and decompresses it (func_02018b64 =
// FS_LoadOverlayImage), then runs its static initializers (FS_StartOverlay).
// The game's own bookkeeping around it (LoadOverlay's slot table, overlap
// checks) works on the record's numbers.
//
// On the host every overlay's code is already in the binary and its globals
// have their own storage (port/tools/romimage.py), so:
//   FS_LoadOverlayInfo   the record, read from the user's .nds OVT;
//   FS_LoadOverlayImage  nothing to load;
//   FS_StartOverlay      that overlay's globals back to the ROM's bytes, its
//                        .bss zeroed, its static initializers run
//                        (sm64ds_romimage_load_module);
//   FS_LoadOverlay       the three in a row. Its DS body also range-checks the
//                        record against the address of an ov001 function, a
//                        DS memory-map test with no host meaning.
// The four are linked with --wrap, so the game's own definitions stay in place.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "romfs.h"
#include "romimage_rt.h"

#ifndef SM64DS_ROM_PATH
#define SM64DS_ROM_PATH "ux0:data/sm64ds/sm64ds.nds"
#endif

namespace {
struct OverlayInfo {        /* FSOverlayInfo, include/nitro/fs/overlay.h */
    uint8_t header[0x20];
    int target;
};
}

extern "C" {

int __wrap_func_02018c00(OverlayInfo *info, int target, int id)
{
    if (target != 0 || !romfs::read_ovt_entry(SM64DS_ROM_PATH, (unsigned)id, info->header)) {
        fprintf(stderr, "overlay: no OVT record for overlay %d (target %d)\n", id, target);
        return 0;
    }
    info->target = target;
    return 1;
}

int __wrap_func_02018b64(OverlayInfo *) { return 1; }

/* The overlays' hand-written static initializers. On the DS, FS_StartOverlay
   calls the functions the overlay's own .init table lists; most of those are
   C++ constructors the host runs from its .init_array groups
   (sm64ds_romimage_load_module), but these are plain functions -- they build
   objects that live in the overlay's .bss (the player's model and animation
   files, for one) -- and nothing else calls them. Without them every such
   object stays zero: file 0, and the stage set-up stopped there. */
void __sinit_ov002_02100560(void);
void __sinit_ov002_021019d0(void);
void __sinit_ov006_02130a04(void);
void __sinit_ov006_021322bc(void);
void __sinit_ov022_02113050(void);
void __sinit_ov075_0211bb00(void);
void __sinit_ov075_0211c51c(void);

/* The arrow signs' four file handles (ov098, d_a_obj_yajirusi.cpp): that
   file defines them for the DS link only, so on the host their storage (the
   ROM image's) was never constructed -- file 0, the crash entering Bob-omb
   Battlefield. The same constructors its static initializer calls. */
struct SharedFilePtr;
extern SharedFilePtr gYajirusiLModelFile, gYajirusiRModelFile;
extern SharedFilePtr gYajirusiLCollisionFile, gYajirusiRCollisionFile;
void *func_02017acc(void *, unsigned int);     /* model handle constructor */
void *func_02017b4c(void *, unsigned int);     /* collision handle constructor */

static void sinit_ov098_yajirusi(void)
{
    func_02017acc(&gYajirusiLModelFile, 0x0497);
    func_02017acc(&gYajirusiRModelFile, 0x0499);
    func_02017b4c(&gYajirusiLCollisionFile, 0x0498);
    func_02017b4c(&gYajirusiRCollisionFile, 0x049a);
}

static void run_handwritten_sinits(uint32_t id)
{
    static const struct { uint32_t id; void (*fn)(void); } kSinit[] = {
        {2, __sinit_ov002_02100560},  {2, __sinit_ov002_021019d0},
        {6, __sinit_ov006_02130a04},  {6, __sinit_ov006_021322bc},
        {22, __sinit_ov022_02113050},
        {75, __sinit_ov075_0211bb00}, {75, __sinit_ov075_0211c51c},
        {98, sinit_ov098_yajirusi},
    };
    for (const auto &s : kSinit)
        if (s.id == id) s.fn();         // address order, as the DS's table has them
}

void __wrap_FS_StartOverlay(OverlayInfo *info)
{
    uint32_t id;
    memcpy(&id, info->header, 4);
    printf("overlay: start %u\n", id);
    if (!sm64ds_romimage_load_module((int)id))
        fprintf(stderr, "overlay: could not load overlay %u from the ROM\n", id);
    run_handwritten_sinits(id);
}

int __wrap_FS_LoadOverlay(int target, int id)
{
    OverlayInfo info;
    if (!__wrap_func_02018c00(&info, target, id)) return 0;
    __wrap_FS_StartOverlay(&info);
    return 1;
}

}  // extern "C"

/* __register_global_object (src/__register_global_object.c) pushes a static
   object's 12-byte .bss node onto __global_destructor_chain. On the DS,
   FS_EndOverlay unlinks an overlay's nodes when it is unloaded, so the next
   load pushes them afresh. Here it never does -- it picks the nodes by DS
   address, and the host's code and .bss are not at the overlay's DS
   addresses -- and loading the overlay again zeroes its .bss, node included,
   then pushes the same node on top of itself: the chain loops, and the next
   FS_EndOverlay walks it forever (the hang leaving the first level, after
   the castle grounds had been entered twice). A node already in the chain
   is now refreshed in place instead. */
extern "C" {

struct GlobalDtorNode {
    GlobalDtorNode *next;
    void *dtor;
    void *obj;
};
extern GlobalDtorNode *__global_destructor_chain;
void __real___register_global_object(void *obj, void *dtor, void **regmem);

void __wrap___register_global_object(void *obj, void *dtor, void **regmem)
{
    GlobalDtorNode *node = reinterpret_cast<GlobalDtorNode *>(regmem);
    int guard = 0;
    for (GlobalDtorNode *p = __global_destructor_chain; p && guard < 100000; p = p->next, ++guard) {
        if (p == node) {
            node->dtor = dtor;
            node->obj = obj;
            return;
        }
    }
    __real___register_global_object(obj, dtor, regmem);
}

}  // extern "C"
