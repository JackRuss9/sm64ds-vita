// Gate-9 host storage and primitives for GCC builds (PS Vita).
//
// The MSVC build gets these from hal/actor_vtables.cpp, hal/cxxname_bridge.cpp
// and hal/actor_ctor_bridge.cpp, mixed in with synthetic vtables and C-name
// forwarders that only make sense under MSVC. Under GCC only the residue is
// needed, collected here:
//
//   * storage for globals whose defining TUs no gate builds yet (the DS
//     keeps them in arm9/overlay .data/.bss; zero is their boot state, or the
//     smoke seeds them). Itanium does not mangle namespace-scope variables, so
//     one plain definition satisfies every C and C++ reference;
//   * the two asm primitives (no C body in src/) and the zero-fill veneer;
//   * the subsystems an arrow sign touches only when it is destroyed or
//     knocked about -- particles, sound, the death table, the camera shake --
//     as no-ops (cosmetic, nothing to host yet) or loud traps (state the gate
//     must not reach).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntr/mmio.h"
extern "C" __attribute__((weak)) void sm64ds_log_flush(void);   /* vita_main.cpp */

typedef unsigned int u32;
struct Vector3;
struct Player { void IncMegaKillCount(); };
struct dClipper { int ClipSphere(Vector3 &, int, unsigned char *); };
namespace Particle { struct System { static void NewSimple(u32, int, int, int); }; }
namespace Sound { void PlayBank3(u32, const Vector3 &); }

[[noreturn]] static void host_trap(const char *who)
{
    fprintf(stderr, "FATAL: %s reached; port/vita/actor_host.cpp only links "
                    "it (the subsystem is not hosted yet)\n", who);
    if (sm64ds_log_flush) sm64ds_log_flush();
    abort();
}

// ---- storage -------------------------------------------------------------
#define HOST_BSS(name, bytes) alignas(8) unsigned char name[bytes]

extern "C" {
/* spawn context fBase_c::fBase_c reads (staged by func_02043180) */
unsigned short data_020a4b54;           /* pending actor ID */
unsigned char data_020a4b48;            /* pending area byte */
u32 data_020a4b60;                      /* pending param1 */
void *data_020a4b64;                    /* pending parent node */
void *data_020a4b68;
u32 data_02099e70;                      /* next unique actor id */
/* actorID -> profile (classInit + two list priorities) */
void *data_020a4bb8_storage[0x200];
void **data_020a4bb8 = data_020a4bb8_storage;
void *data_020a0eac;                    /* the actor heap (smoke: root heap) */

/* ov098's resource columns for the arrow signs: {model, kcl, clps} rows,
   one symbol per column (the DS addresses overlap at +0/+4/+8) */
void *data_ov098_0213c380[6];
void *data_ov098_0213c384[6];
void *data_ov098_0213c388[6];

/* player list: the smoke installs one stand-in */
void *data_0209f394[8];
unsigned char data_0209f21c;

/* actor-world globals, boot state zero */
HOST_BSS(data_020a0e68, 0x40);          /* shared scratch Matrix4x3 */
HOST_BSS(data_020a0c80, 0x80);          /* collision actor registry */
HOST_BSS(data_020a0d60, 0x10);          /* Vector3 */
HOST_BSS(data_020a0d1c, 0x40);          /* dBgPi */
HOST_BSS(data_0209cef4, 0x10);          /* shadow scale globals */
HOST_BSS(data_0209ceec, 0x10);
HOST_BSS(data_0209b458, 0x8);
HOST_BSS(data_0209b450, 0x8);
HOST_BSS(data_0209b44c, 0x4);
HOST_BSS(data_0209b45c, 0x4);           /* spawn default rotation (null) */
HOST_BSS(data_0209b460, 0x4);           /* spawn default position (null) */
HOST_BSS(data_0209b464, 0x4);
HOST_BSS(data_0208e378, 0x4);
HOST_BSS(data_0208e37c, 0x8);
HOST_BSS(data_0208e380, 0x8);
HOST_BSS(data_0209fc68, 0x4);
HOST_BSS(data_0209f43c, 0x100);         /* the dClipper instance */
HOST_BSS(data_0209f318, 0x4);           /* the camera (none) */
HOST_BSS(data_0209f2d8, 0x4);           /* mega-char state: none */
HOST_BSS(data_0209f2c4, 0x4);
HOST_BSS(data_0209f294, 0x4);
HOST_BSS(data_0209f274, 0x4);
HOST_BSS(data_0209f20c, 0x4);
HOST_BSS(data_02099f48, 0x8);
HOST_BSS(data_02099f50, 0x10);
HOST_BSS(data_02099f60, 0x8);
HOST_BSS(data_02099f68, 0x8);
HOST_BSS(data_02099f70, 0x8);
/* dExtShadowModel_c's cuboid template BMD, built at runtime by boot code the
   port does not host yet (the ov000 image holds path strings there) */
HOST_BSS(data_ov000_020ad524, 0x40);
}

// ---- primitives ----------------------------------------------------------
extern "C" {
/* asm primitive (src/engine/mi/MI_Memory.c): 48-byte block copy */
void Copy48Bytes(int *src, int *dst)
{
    int *s = (int *)::ntr::host_ptr((uintptr_t)src);
    int *d = (int *)::ntr::host_ptr((uintptr_t)dst);
    for (int i = 0; i < 12; ++i) d[i] = s[i];
}
/* asm primitive: 4x3 fx32 Y rotation from (sin, cos):
   rows {c,0,-s},{0,1,0},{s,0,c},{0,0,0} */
void func_02052820(int *m, int s, int c)
{
    m[0] = c; m[1] = 0;      m[2] = -s;
    m[3] = 0; m[4] = 0x1000; m[5] = 0;
    m[6] = s; m[7] = 0;      m[8] = c;
    m[9] = 0; m[10] = 0;     m[11] = 0;
}
/* the X and Z siblings (NitroSDK MTX_RotX43_ / MTX_RotZ43_ order, which the
   Y one between them confirms): X rows {1,0,0},{0,c,s},{0,-s,c};
   Z rows {c,s,0},{-s,c,0},{0,0,1} */
void func_02052800(int *m, int s, int c)
{
    m[0] = 0x1000; m[1] = 0;  m[2] = 0;
    m[3] = 0;      m[4] = c;  m[5] = s;
    m[6] = 0;      m[7] = -s; m[8] = c;
    m[9] = 0; m[10] = 0; m[11] = 0;
}
void func_0205283c(int *m, int s, int c)
{
    m[0] = c;  m[1] = s; m[2] = 0;
    m[3] = -s; m[4] = c; m[5] = 0;
    m[6] = 0;  m[7] = 0; m[8] = 0x1000;
    m[9] = 0; m[10] = 0; m[11] = 0;
}
/* fBase_c::operator new's zero fill. Its DS body (src/cstd4.cpp) is a
   register-threaded chain into the block-fill primitives; the semantics are
   memset's. */
void func_0206e2f8(void *p, int v, unsigned n) { memset(p, v, n); }

/* camera shake (dActor_c::Earthquake) */
void func_0200d8c8(void) { host_trap("func_0200d8c8 (camera earthquake)"); }
/* Sound::Play of bank 3 at a position: no audio yet */
int func_02012694(int, void *) { return 0; }
/* area visibility and the per-level death table: only the processing-list
   paths (BeforeInitResources / BeforeBehavior / OnTurnIntoEgg) read them, and
   the smoke drives the lifecycle methods directly */
unsigned char IsAreaShowing(int) { host_trap("IsAreaShowing"); }
int DeathTable_GetBit(int) { host_trap("DeathTable_GetBit"); }
void DeathTable_SetBit(int) { host_trap("DeathTable_SetBit"); }
}

/* particles and sound: cosmetic, not hosted yet */
void Particle::System::NewSimple(u32, int, int, int) {}
void Sound::PlayBank3(u32, const Vector3 &) {}
/* the horizontal angle the mega-char knockback takes (Vec3_HorzAngle); the
   real atan2 runs on ITCM helpers and an arm9 table no gate hosts yet */
namespace cstd { int atan2(int, int); }
int cstd::atan2(int, int) { host_trap("cstd::atan2"); }
/* the Player gates own these */
void Player::IncMegaKillCount() { host_trap("Player::IncMegaKillCount"); }
int dClipper::ClipSphere(Vector3 &, int, unsigned char *) { host_trap("dClipper::ClipSphere"); }

// ---- drop shadows: deferred, as on the MSVC build ------------------------
// dExtShadowModel_c::InitCuboid hands SetFile the cuboid template BMD at
// data_ov000_020ad524, which boot code builds at runtime and the port does not
// host yet (here it is zeroed storage, and the vertex walk would read it as a
// model). The MSVC build replaces InitCuboid with `return 1` and
// DropShadowScaleXYZ with a no-op (hal/cxxname_bridge.cpp). The GCC build
// keeps src/'s definitions and diverts the calls instead: the gate links with
// --wrap for both symbols, so every call lands here.
extern "C" {
int __wrap__ZN17dExtShadowModel_c10InitCuboidEv(void *) { return 1; }
void __wrap__ZN8dActor_c18DropShadowScaleXYZER17dExtShadowModel_cR9Matrix4x3iiih(
    void *, void *, void *, int, int, int, unsigned char) {}
}
