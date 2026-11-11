// Gate-9 smoke, GCC/Itanium edition (PS Vita): a real actor lives its
// lifecycle on host.
//
// Same gate as port/tests/smoke_actor.cpp, rewritten for builds where the
// compiler's ABI is the ROM's. That test was written for an MSVC host and
// for an earlier state of the decomp: it calls the factory by its old name
// (ArrowSignRight_Spawn), seeds the ov098 file table as one mixed array, and
// dispatches through a hand-built MSVC vtable with __thiscall casts. At this
// commit the actor is daObjYajirusi_c, its three resource columns are
// separate symbols, and under GCC its vtable is the compiler's own: the
// Itanium layout, which is the ROM's, emitted from the class's key-function
// TU. So nothing here fakes dispatch. Every lifecycle call goes through the
// object's real vptr.
//
// What runs:
//   factory   daObjYajirusi_c_classInit_YAJIRUSI_L: operator new from the
//             game heap and the full ctor chain (dBgActor_c -> dActor_c ->
//             fBase_c, with dExtSimpleModel_c, dBgW_KcMbg and dExtShadowModel_c
//             members), reading the spawn context func_02043180 stages.
//   init      InitResources: the sign's BMD and KCL through the card seam,
//             LoadFile rebasing both, the model's SetFile, the mesh collider's
//             SetFile with the actor's transform.
//   behavior  four Behavior ticks.
//   render    Render, which walks the model into the GX; the triangle count
//             the ntr rasterizer sees is the check.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ntr/gx.h"
#include "ntr/mmio.h"
#include "ntr/ppu.h"

#include "fBase_c.h"

typedef unsigned int u32;

struct ArrowSignFileColumn { void *value; void *pad[2]; };
struct fBaseActorInfoS { fBase_c *(*classInit)(); unsigned short behaviorPriority, renderPriority; };
struct SharedFilePtrC { unsigned short fileID; unsigned char numRefs;
                        unsigned char pad; void *filePtr; };

extern "C" {
int *daObjYajirusi_c_classInit_YAJIRUSI_L(void);
void *_ZN5mHeap6Heap_t13SetupRootHeapEv(void);
SharedFilePtrC *_ZN13SharedFilePtr9ConstructEj(SharedFilePtrC *s, u32 id);
/* stages the spawn context fBase_c's constructor reads (ActorBase.cpp) */
void func_02043180(int actorID, void *parentNode, u32 param1, int unk012);
}
/* Under the Itanium ABI namespace-scope variables are not mangled, so these
   bind to src/'s C++ definitions (or to port/vita/actor_host.cpp's storage
   for the ones that live in TUs no gate builds yet). */
extern int data_0209b3ec[12];                   /* camera matrix */
extern fBaseActorInfoS **data_020a4bb8;         /* actorID -> profile */
extern ArrowSignFileColumn data_ov098_0213c380[];  /* model column */
extern ArrowSignFileColumn data_ov098_0213c384[];  /* KCL column */
extern ArrowSignFileColumn data_ov098_0213c388[];  /* CLPS column */
extern void *data_0209f394[];                   /* the player array */
extern unsigned char data_0209f21c;             /* player count */
extern void *data_020a0eac;                     /* the actor heap */
#include "Memory.h"   /* Memory::defaultHeapPtr */

static int g_failures;
#define CHECK(cond) \
    do { if (!(cond)) { ++g_failures; \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void ident_fx(void *m)
{
    memset(m, 0, 48);
    ((int *)m)[0] = ((int *)m)[4] = ((int *)m)[8] = 0x1000;
}

static void reset_scene()
{
    ntr::gx_reset();
    NTR_MMIO(uint32_t, 0x04000440) = 0;
    NTR_MMIO(uint32_t, 0x04000454) = 0;
    NTR_MMIO(uint32_t, 0x04000440) = 1;
    NTR_MMIO(uint32_t, 0x04000454) = 0;
    NTR_MMIO(uint32_t, 0x04000580) = 0u | (0u << 8) | (255u << 16) | (191u << 24);
    ntr::gx_set_light(0, -0.4f, -0.6f, -0.7f, 0x7FFF);
    ntr::gx_enable_lights(0x1);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!ntr::io_init()) { fprintf(stderr, "io_init failed\n"); return 2; }
    CHECK(_ZN5mHeap6Heap_t13SetupRootHeapEv() != NULL);
    ident_fx(data_0209b3ec);

    /* The spawn context dBase_c::Spawn would have staged: actor 0x12b
       (YAJIRUSI_L, file column 0), its profile's two list priorities, and the
       ov098 resource columns -- on the DS prebuilt overlay data, here seeded
       with handle 1177 (the sign's BMD), 1178 (its KCL) and a zeroed CLPS
       block (the lookup's default path, as in gate 8). */
    static fBaseActorInfoS profile = {
        (fBase_c *(*)())daObjYajirusi_c_classInit_YAJIRUSI_L, 100, 100 };
    data_020a4bb8[0x12b] = &profile;
    data_020a0eac = Memory::defaultHeapPtr;     /* actors allocate from it */
    static SharedFilePtrC sign_model, sign_kcl;
    static char sign_clps[0x100];
    _ZN13SharedFilePtr9ConstructEj(&sign_model, 1177);
    _ZN13SharedFilePtr9ConstructEj(&sign_kcl, 1178);
    data_ov098_0213c380[0].value = &sign_model;
    data_ov098_0213c384[0].value = &sign_kcl;
    data_ov098_0213c388[0].value = sign_clps;

    /* A stand-in player: the actor world assumes at least one exists
       (ClosestPlayer callers dereference without a null check -- on the DS
       Mario always exists). */
    static char fake_player[0x800];
    data_0209f394[0] = fake_player;
    data_0209f21c = 1;

    func_02043180(0x12b, NULL, 0, 0);
    fBase_c *actor = profile.classInit();
    CHECK(actor != NULL);
    if (!actor) return 1;
    printf("  spawned actor 0x%x (unique id %u)\n",
           (unsigned)actor->actorID, (unsigned)actor->uniqueID);
    CHECK(actor->actorID == 0x12b);

    int init_ok = actor->InitResources();
    printf("  InitResources -> %d\n", init_ok);
    CHECK(init_ok == 1);

    reset_scene();
    for (int f = 0; f < 4; ++f) {
        int r = actor->Behavior();
        printf("  tick %d -> %d\n", f, r);
    }
    int render_ok = actor->Render();
    printf("  Render -> %d\n", render_ok);

    size_t tris = 0;
    ntr::gx_polygons(tris);
    printf("  triangles from the actor's render: %u\n", (unsigned)tris);
    CHECK(tris > 0);

    if (g_failures) {
        fprintf(stderr, "smoke_actor: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("smoke_actor: all checks passed (an actor spawned, initialized, "
           "behaved and rendered through its real vtable)\n");
    return 0;
}
