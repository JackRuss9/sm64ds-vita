// The DS's tick and timers for the full-game GCC build (PS Vita).
//
// OS tick (replaces src/engine/os/OS_Tick.cpp): on the DS, timer 0 counts at
// 33.513982 MHz / 64 and its overflow IRQ extends it to 64 bits. Here
// OS_GetTick reads the host's monotonic clock in those same units, so every
// delta the game measures (frame timing, alarms, OS_Sleep) is real time.
//
// Timers 1-3: the game (the OS alarms, OSi_SetTimer on timer 1) programs
// them with plain stores to TMxCNT_L/H, which a host store cannot act on. So
// they are looked at -- at the BIOS waits and, now and then, when the game
// unmasks interrupts (port/vita/irq_host.cpp): a timer that is enabled starts
// counting from when it was first seen programmed so; when its period has
// passed it overflows -- reloads, and raises its IRQ if asked to -- as the
// hardware would have, up to one look late. Cascading timers are not used by
// the game and count nothing.
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "ntr/mmio.h"


typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

extern "C" void sm64ds_irq_raise(u32 mask);     /* irq_host.cpp */
namespace IRQ {
void SetIRQHandler(u32 mask, void (*handler)(void));
u32 EnableIRQs(u32 mask);
}  // namespace IRQ

namespace {
constexpr u64 DS_HZ = 33513982;     // ARM7 bus clock: the timers' base

u64 now_us()
{
    struct timespec ts;                 // VitaSDK's newlib: the process clock
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000u + (u64)ts.tv_nsec / 1000u;
}
u64 g_t0;
u64 cycles_now()
{
    if (!g_t0) g_t0 = now_us();
    const u64 us = now_us() - g_t0;
    return us / 1000000u * DS_HZ + us % 1000000u * DS_HZ / 1000000u;
}

struct Timer {
    bool running;
    u16 ctl, reload;
    u64 start;                      // cycles at the last (re)start or overflow
};
Timer g_timer[4];
constexpr u32 PRESCALE[4] = {1, 64, 256, 1024};
}  // namespace

extern "C" {

/* --- OS tick (NitroSDK os_tick.c) --- */
extern u16 data_020a6430;           /* OSi_UseTick */
void OSi_SetTimerReserved(int timerNum);

void OS_InitTick(void)
{
    if (data_020a6430) return;
    data_020a6430 = 1;
    OSi_SetTimerReserved(0);
    cycles_now();
}
long long OS_GetTick(void) { return (long long)(cycles_now() / 64); }
int OS_GetTickLo(void) { return (int)(u16)(cycles_now() / 64); }
void OSi_CountUpTick(void) {}

/* The OS alarm's timer-1 IRQ entry (asm in src/engine/os/OS_Alarm.cpp). */
void OSi_ArrangeTimer(void);
void OSi_AlarmHandler(void) { OSi_ArrangeTimer(); }

/* Look at timers 1-3: start the ones newly programmed, overflow the due ones. */
void sm64ds_timers_poll(void)
{
    u32 raise = 0;
    for (int t = 1; t < 4; ++t) {
        Timer &tm = g_timer[t];
        const u16 ctl = NTR_MMIO(u16, 0x04000102 + 4 * t);
        const u16 reload = NTR_MMIO(u16, 0x04000100 + 4 * t);
        if (!(ctl & 0x80) || (ctl & 0x04)) {    // stopped, or counting up a cascade
            tm.running = false;
            continue;
        }
        const u64 now = cycles_now();
        if (!tm.running || ctl != tm.ctl || reload != tm.reload) {
            tm.running = true;
            tm.ctl = ctl;
            tm.reload = reload;
            tm.start = now;
            continue;
        }
        const u64 period = (u64)(0x10000u - reload) * PRESCALE[ctl & 3];
        if (now - tm.start < period) continue;
        tm.start = now;                         // overflowed: reloaded, counting again
        if (ctl & 0x40) raise |= 1u << (3 + t);
    }
    if (raise) sm64ds_irq_raise(raise);
}

}  // extern "C"
