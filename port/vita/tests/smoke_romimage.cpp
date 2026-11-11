// Gate-10 smoke (GCC / PS Vita): the whole game links, and its ROM-resident
// globals load from the user's own dump with every embedded pointer patched.
//
// The binary holds all of src/ (asm dropped, the host's own natives in
// port/vita/game_host.cpp); port/tools/romimage.py generated storage for the
// ~15,700 globals no C source defines and a table of ~13,100 DS pointers
// inside them. This test runs the startup fill and then follows pointer
// chains that cross modules, without running the game:
//
//   ACTOR_SPAWN_TABLE (ARM9 .data) -> a profile in overlay storage ->
//   the profile's classInit -> the actor factory compiled from src/
//
// For the arrow sign (actor 0x12b) the chain must end at
// daObjYajirusi_c_classInit_YAJIRUSI_L, the very function gate 9 ran.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" {
bool sm64ds_romimage_load(const char *rom_path);
extern unsigned sm64ds_romimage_filled, sm64ds_romimage_missing;
int *daObjYajirusi_c_classInit_YAJIRUSI_L(void);
int *daObjYajirusi_c_classInit_YAJIRUSI_R(void);
}
struct Profile { void *classInit; unsigned short behaviorPriority, renderPriority; };
extern Profile *ACTOR_SPAWN_TABLE[];
extern int IDENTITY_MATRIX4X3[12];

#ifndef SM64DS_ROM_PATH
#define SM64DS_ROM_PATH "ux0:data/sm64ds/sm64ds.nds"
#endif

static int g_failures;
#define CHECK(cond) \
    do { if (!(cond)) { ++g_failures; \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool is_ds_address(const void *p)
{
    const uintptr_t a = (uintptr_t)p;
    return a >= 0x01ff8000u && a < 0x02800000u;
}

int main(void)
{
    CHECK(sm64ds_romimage_load(SM64DS_ROM_PATH));
    printf("  globals filled: %u, outside their module: %u\n",
           sm64ds_romimage_filled, sm64ds_romimage_missing);
    CHECK(sm64ds_romimage_filled > 10000);
    CHECK(sm64ds_romimage_missing == 0);
    if (sm64ds_romimage_missing) {
        /* overlays not read: the cross-module chains below would follow
           unfilled storage, so stop here */
        fprintf(stderr, "smoke_romimage: %d FAILURE(S)\n", g_failures);
        return 1;
    }

    /* a plain table: the 4.12 identity */
    CHECK(IDENTITY_MATRIX4X3[0] == 0x1000 && IDENTITY_MATRIX4X3[4] == 0x1000 &&
          IDENTITY_MATRIX4X3[8] == 0x1000 && IDENTITY_MATRIX4X3[1] == 0);

    /* the spawn table: every non-null profile and factory must be a host
       address now, not a DS one */
    int profiles = 0, factories = 0, still_ds = 0;
    /* 391 entries: ACTOR_SPAWN_TABLE runs 0x02090864..0x02090e80 in the ARM9
       map (config/arm9/symbols.txt), one profile pointer per actor ID */
    for (int id = 0; id < 391; ++id) {
        Profile *p = ACTOR_SPAWN_TABLE[id];
        if (!p) continue;
        if (is_ds_address(p)) { ++still_ds; continue; }
        ++profiles;
        if (p->classInit) {
            if (is_ds_address(p->classInit)) ++still_ds;
            else ++factories;
        }
    }
    printf("  spawn table: %d profiles, %d factories, %d DS pointers left\n",
           profiles, factories, still_ds);
    CHECK(profiles > 200);
    CHECK(still_ds == 0);

    Profile *l = ACTOR_SPAWN_TABLE[0x12b], *r = ACTOR_SPAWN_TABLE[0x12c];
    CHECK(l && r);
    if (l && r) {
        printf("  actor 0x12b: profile %p, factory %p (YAJIRUSI_L at %p)\n",
               (void *)l, l->classInit, (void *)&daObjYajirusi_c_classInit_YAJIRUSI_L);
        CHECK(l->classInit == (void *)&daObjYajirusi_c_classInit_YAJIRUSI_L);
        CHECK(r->classInit == (void *)&daObjYajirusi_c_classInit_YAJIRUSI_R);
        printf("  actor 0x12b priorities: behavior %u, render %u\n",
               l->behaviorPriority, l->renderPriority);
    }

    if (g_failures) {
        fprintf(stderr, "smoke_romimage: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("smoke_romimage: all checks passed (the whole game links and its ROM data "
           "loads with every pointer patched)\n");
    return 0;
}
