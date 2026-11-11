// Native host versions of what the full game build cannot compile from src/
// (GCC / PS Vita).
//
// Each function here is one of the game's hand-written ARM primitives -- the
// asm blocks port/tools/hostexterns.py drops from the host copies -- or a DS
// SDK service whose src/ body is hardware-only. The contract each one keeps is
// taken from its src/ definition (named per entry) or the NitroSDK function it
// is; nothing here reads the ROM.
//
// Three groups:
//   1. memory / matrix / soft-float primitives: exact semantics, natively;
//   2. CPU-state and SDK services (interrupt masks, CP15, locks, spin waits):
//      host equivalents over the ntr runtime's model;
//   3. not here: the thread context switch (ARMSaveContext/
//      ARMRestoreContext/OS_InitContext) is port/vita/os_thread_host.cpp.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntr/mmio.h"
#include "ntr/rt.h"
extern "C" __attribute__((weak)) void sm64ds_log_flush(void);   /* vita_main.cpp */

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;

[[noreturn]] static void host_trap(const char *who)
{
    fprintf(stderr, "FATAL: %s reached; port/vita/game_host.cpp does not "
                    "implement it yet\n", who);
    if (sm64ds_log_flush) sm64ds_log_flush();
    abort();
}

// Game code sometimes hands these primitives DS addresses (VRAM, OAM, the
// palette) and sometimes host pointers; ntr::host_ptr is the identity on the
// latter.
template <class T> static inline T *hp(const void *p)
{
    return reinterpret_cast<T *>(::ntr::host_ptr(reinterpret_cast<uintptr_t>(p)));
}

extern "C" {

// ---- 1. memory primitives (src/engine/mi/MI_Memory.c and friends) --------

/* Copy48Bytes(src, dst): 48 bytes, word by word (ldmia/stmia x4). */
void Copy48Bytes(const u32 *src, u32 *dst)
{
    const u32 *s = hp<const u32>(src);
    u32 *d = hp<u32>(dst);
    for (int i = 0; i < 12; ++i) d[i] = s[i];
}

/* MultiCopy_Int(src, dst, len): words while dst < dst + len (bytes). */
void MultiCopy_Int(const u32 *src, u32 *dst, int len)
{
    const u32 *s = hp<const u32>(src);
    u32 *d = hp<u32>(dst);
    for (int i = 0; i < len / 4; ++i) d[i] = s[i];
}

/* MultiCopy32Bytes(src, dst, len): words, 32-byte blocks then the tail. */
void MultiCopy32Bytes(const u32 *src, u32 *dst, int len)
{
    MultiCopy_Int(src, dst, len);
}

/* MultiCopyHalf(src, dst, size): halfwords at byte offsets 0, 2, .. < size. */
void MultiCopyHalf(const void *src, void *dst, s32 size)
{
    const u16 *s = hp<const u16>(src);
    u16 *d = hp<u16>(dst);
    for (s32 i = 0; i < size / 2 + (size & 1); ++i) d[i] = s[i];
}

/* MultiStore16(val, dst, nbytes): halfword fill at offsets 0, 2, .. < nbytes. */
void MultiStore16(u16 val, void *dst, int nbytes)
{
    u16 *d = hp<u16>(dst);
    for (int i = 0; i < nbytes / 2 + (nbytes & 1); ++i) d[i] = val;
}

/* MultiStore32Bytes(val, dst, len): word fill of len bytes. */
void MultiStore32Bytes(u32 val, u32 *dst, int len)
{
    u32 *d = hp<u32>(dst);
    for (int i = 0; i < len / 4; ++i) d[i] = val;
}

/* func_0205a588 = MI_CpuFill8(dst, value, size): byte fill. */
void func_0205a588(void *dst, int value, u32 size)
{
    memset(hp<u8>(dst), value & 0xff, size);
}

/* CpuCopy8 = MI_CpuCopy8(src, dst, size): byte copy. */
void CpuCopy8(const void *src, void *dst, u32 size)
{
    memmove(hp<u8>(dst), hp<const u8>(src), size);
}

/* MI_SwapWord(value, addr) and func_0206dab4(value, lock): the `swp`
   instruction -- store value, return the old word. One game thread runs at a
   time on the host, so a plain exchange is atomic enough. */
u32 MI_SwapWord(u32 value, volatile u32 *addr)
{
    volatile u32 *a = hp<volatile u32>((const void *)addr);
    u32 old = *a;
    *a = value;
    return old;
}
u32 func_0206dab4(u32 value, void *lock) { return MI_SwapWord(value, (volatile u32 *)lock); }

/* MIi_UncompressBackward(bottom): the BLZ decoder overlays use, in place.
   The host loads no overlay code, so nothing should call it. */
void MIi_UncompressBackward(void *) { host_trap("MIi_UncompressBackward"); }

// ---- matrices (src/unnamed/arm9/0205/func_0205*.c) -------------------------

/* func_020527e8(m, sx, sy, sz): 4x3 fx32 scale matrix. */
void func_020527e8(s32 *m, s32 sx, s32 sy, s32 sz)
{
    memset(m, 0, 48);
    m[0] = sx; m[4] = sy; m[8] = sz;
}
/* func_02052800 / func_02052820 / (func_0205283c): 4x3 rotation about X / Y
   (/ Z) from (sin, cos), NitroSDK MTX_Rot?43_ layout. */
void func_02052800(s32 *m, s32 s, s32 c)
{
    memset(m, 0, 48);
    m[0] = 0x1000; m[4] = c; m[5] = s; m[7] = -s; m[8] = c;
}
void func_02052820(s32 *m, s32 s, s32 c)
{
    memset(m, 0, 48);
    m[0] = c; m[2] = -s; m[4] = 0x1000; m[6] = s; m[8] = c;
}

/* func_0205283c: the Z rotation, {c,s,0},{-s,c,0},{0,0,1} */
void func_0205283c(s32 *m, s32 s, s32 c)
{
    memset(m, 0, 48);
    m[0] = c; m[1] = s; m[3] = -s; m[4] = c; m[8] = 0x1000;
}
/* Matrix4x3_LoadIdentity / Matrix3x3_LoadIdentity / func_02052ec8 (4x4) */
void Matrix4x3_LoadIdentity(s32 *m)
{
    memset(m, 0, 48);
    m[0] = m[4] = m[8] = 0x1000;
}
void Matrix3x3_LoadIdentity(s32 *m)
{
    memset(m, 0, 36);
    m[0] = m[4] = m[8] = 0x1000;
}
void func_02052ec8(s32 *m)
{
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 0x1000;
}
/* Matrix3x3_SetRotationZ(m, s, c): {c,s,0},{-s,c,0},{0,0,1} */
void Matrix3x3_SetRotationZ(s32 *m, s32 s, s32 c)
{
    m[0] = c; m[1] = s; m[2] = 0;
    m[3] = -s; m[4] = c; m[5] = 0;
    m[6] = 0; m[7] = 0; m[8] = 0x1000;
}
/* MTX_RotY33_(m, s, c): {c,0,-s},{0,1,0},{s,0,c} */
void MTX_RotY33_(s32 *m, s32 s, s32 c)
{
    m[0] = c; m[1] = 0; m[2] = -s;
    m[3] = 0; m[4] = 0x1000; m[5] = 0;
    m[6] = s; m[7] = 0; m[8] = c;
}
/* func_02052514(a, b): the 3x3 at `a` widened into a 4x4 at `b` (each row
   gains a 0, then {0,0,0,1}). The asm reads r0 and writes r1, whatever the
   parameter names say. */
void func_02052514(const s32 *a, s32 *b)
{
    for (int r = 0; r < 3; ++r) {
        b[r * 4 + 0] = a[r * 3 + 0];
        b[r * 4 + 1] = a[r * 3 + 1];
        b[r * 4 + 2] = a[r * 3 + 2];
        b[r * 4 + 3] = 0;
    }
    b[12] = 0; b[13] = 0; b[14] = 0; b[15] = 0x1000;
}
/* Copy32Bytes(src, dst): 32 bytes, word by word */
void Copy32Bytes(const u32 *src, u32 *dst)
{
    const u32 *s = hp<const u32>(src);
    u32 *d = hp<u32>(dst);
    for (int i = 0; i < 8; ++i) d[i] = s[i];
}

/* func_02071974 / func_02071988: the IS-NITRO debugger console (semihosting
   SWI 0x123456: write a string, read a char). No debugger on the host. */
void func_02071974(const unsigned char *) {}
unsigned char func_02071988(void) { return 0; }

// ---- the soft-float runtime the game calls by address (ITCM) --------------
// Arguments and results are IEEE bit patterns in integer registers (the
// DS has no FPU); the host computes natively.

static inline float f_of(s32 b) { float f; memcpy(&f, &b, 4); return f; }
static inline s32 b_of(float f) { s32 b; memcpy(&b, &f, 4); return b; }

s32 func_01ff9378(s32 a, s32 b) { return b_of(f_of(a) + f_of(b)); }        /* _fadd */
s32 func_01ffa594(s32 a, s32 b) { return b_of(f_of(a) - f_of(b)); }        /* _fsub */
s32 func_01ffa344(s32 a) { return (s32)f_of(a); }                          /* _ffix */
s32 func_01ffa4bc(s32 a) { return b_of((float)a); }                        /* _fflt */
s32 func_01ff98f4(s32 a, s32 b) { return f_of(a) < f_of(b); }              /* float < */
s32 func_01ff99a4(s32 a, s32 b) { return f_of(a) > f_of(b); }              /* float > */
long long func_01ffaa34(long long a, long long b) { return b ? a / b : 0; } /* _ll_sdiv */
double func_01ff8000(double a, double b) { return a + b; }                 /* _dadd */
double func_01ff8e10(double a, double b) { return a - b; }                 /* _dsub */
double func_01ff8708(double a, double b) { return a * b; }                 /* _dmul */
int func_01ff9d40(double a, double b) { return a == b; }                   /* _deq */
int func_01ff9e2c(double a, double b) { return a < b; }                    /* double < */
unsigned long long func_01ff859c(double a)                                 /* _ll_ufrom_d */
{
    return a <= 0.0 ? 0ull : (unsigned long long)a;
}

// ---- 2. CPU state, interrupts, CP15 ---------------------------------------


/* WaitByLoop / OS_SpinWait: burn time. The host's time is its own. */
void WaitByLoop(s32) {}
void OS_SpinWait(u32) {}

/* func_0206d9cc / func_0206da18: the IS-debugger hooks' IRQ/FIQ-off entries;
   no debugger on the host. */
int func_0206d9cc(void) { return 0; }
int func_0206da18(void) { return 0; }

/* OSi_DoBoot: the reset path (OS_ResetSystem). Leaving the game is the
   host's call. */
void OSi_DoBoot(void) { host_trap("OSi_DoBoot (system reset)"); }

/* OS spin-lock IDs (NitroSDK OS_GetLockID: 0x40..0x7f on the ARM9) and the
   cartridge lock: one CPU, nothing to arbitrate with. */
s32 OS_GetLockID(void)
{
    static s32 next = 0x40;
    return next < 0x80 ? next++ : -3;
}
s32 OS_UnLockCartridge(u16) { return 0; }
void OS_ReleaseLockID(u16) {}

// The thread context switch (ARMSaveContext, ARMRestoreContext and
// OS_InitContext) lives in port/vita/os_thread_host.cpp; interrupt delivery,
// ARMProcessorMode and the BIOS waits in port/vita/irq_host.cpp.

// The OS tick, the timers and the alarm IRQ entry (OSi_AlarmHandler) live
// in port/vita/timers_host.cpp.
/* OSi_ExceptionHandler: the SDK's asm exception vector (OS_Exception.cpp).
   OS_InitException only stores its address for the BIOS; on the host a CPU
   exception is the host's own crash, so it never runs. */
void OSi_ExceptionHandler(void) { host_trap("OSi_ExceptionHandler (CPU exception)"); }



}  // extern "C"

namespace IRQ {
/* Both masks at once: the host has no FIQ, so these are the IRQ pair. */
u32 DisableAll() { return ::ntr::rt_irq_disable(); }
u32 RestoreAll(u32 prev) { return ::ntr::rt_irq_restore(prev); }
void SetIRQState(bool on) { if (on) ::ntr::rt_irq_enable(); else ::ntr::rt_irq_disable(); }
void SetFIQState(bool) {}

}  // namespace IRQ

void Halt(void);    /* port/vita/irq_host.cpp: the frame, then pending interrupts */

namespace CP15 {
void EnableDTCM(void) {}
/* Where the DS maps its data TCM; DS addresses stay DS addresses on the host. */
unsigned int GetDTCMBaseAddress(void) { return 0x023C0000u; }
/* Wait for an interrupt (the idle thread's loop): as the BIOS Halt does --
   posted work and the timers first, the frame loop its turn, then VBlank. On
   the DS it wakes on any pending interrupt, masked or not; the handlers run
   once the thread unmasks. */
void WaitForInterrupt(void) { ::Halt(); }
}  // namespace CP15

/* A message on the screen (port/vita/vita_display.cpp has the real one; the
   gates without a display get this, which does nothing). */
extern "C" __attribute__((weak)) void sm64ds_display_toast(const char *) {}
/* Player.cpp's root-motion trace (gate 11 logs it, savestate_host.cpp). */
extern "C" __attribute__((weak)) void sm64ds_rootmotion_log(int, int, const int *, const int *, const int *,
                                                            const int *, int, const void *, int, int) {}
