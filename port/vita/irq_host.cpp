// Interrupt delivery for the full-game GCC build (PS Vita).
//
// On the DS the CPU takes an IRQ, the SDK's OS_IrqHandler (hand-written asm,
// src/IRQHandler.cpp) raises OSi_ThreadInfo.irqDepth, calls the handler the
// game registered for the lowest pending source in its table
// (IRQ::SetIRQHandler, src/engine/os/OS_Interrupt.cpp), acknowledges IF and,
// on the way out, performs the thread switch a handler asked for
// (OS_IrqHandler_ThreadSwitch: isNeedRescheduling -> OS_RescheduleThread).
//
// The host has no asynchronous interrupts. The one the game's frame depends on
// is VBlank: IRQ::VBlankHandler counts frames and wakes the threads sleeping
// on the frame queues (the main loop's func_0201a4bc among them). So the BIOS
// waits -- Halt (the idle thread's OS_Halt), VBlankIntrWait, IntrWait -- give
// the frame loop its turn (ntr::rt_vblank_wait) and then deliver VBlank here,
// exactly as OS_IrqHandler would have.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ntr/mmio.h"
#include "ntr/rt.h"

typedef uint16_t u16;
typedef uint32_t u32;

extern "C" {
struct OSThreadInfoHost {
    u16 isNeedRescheduling;     /* 0x00 */
    u16 max_entry;              /* 0x02 */
    u16 irqDepth;               /* 0x04 */
    u16 padding;                /* 0x06 */
};
extern OSThreadInfoHost data_020a6134;  /* OSi_ThreadInfo (include/nitro/os/thread.h) */
void OS_RescheduleThread(void);
void *_ZN3IRQ13GetIRQHandlerEj(u32 mask);   /* IRQ::GetIRQHandler */
void sm64ds_irq_drain(void);
void sm64ds_timers_poll(void);      /* timers_host.cpp */
/* DTCM +0: OSi_IrqThreadQueue, the mask of thread ids sleeping in OS_WaitIrq */
extern uint16_t data_023c0000[];
}

/* The last 48 host-side events (thread switches, interrupts, posted work,
   card reads, ARM7 words), for the watchdog's report. Written only by the
   thread holding the DS CPU, so no lock. */
namespace {
struct HostEvent { const char *what; uint32_t a, b; };
HostEvent g_events[48];
unsigned g_nevents;
}  // namespace
extern "C" void sm64ds_hostlog(const char *what, uint32_t a, uint32_t b)
{
    g_events[g_nevents++ % 48] = {what, a, b};
}
extern "C" int sm64ds_irq_queued(void);
extern "C" void sm64ds_hostlog_dump(void)
{
    printf("  host events, oldest first (posted work queued: %d):\n", sm64ds_irq_queued());
    for (unsigned i = g_nevents > 48 ? g_nevents - 48 : 0; i < g_nevents; ++i) {
        const HostEvent &e = g_events[i % 48];
        printf("    %-10s %08x %08x\n", e.what, (unsigned)e.a, (unsigned)e.b);
    }
}

namespace {
bool g_in_irq;

/* OS_IrqHandler_ThreadSwitch (src/IRQHandler.cpp, func_01ffd9d4): after any
   interrupt, the threads waiting in OS_WaitIrq are made ready, then the
   pending switch is done. (OSThread: state +0x64, next +0x68, id +0x6c;
   OSThreadInfo.list +0x0c.) */
void irq_exit()
{
    const uint16_t q = data_023c0000[0];
    if (q) {
        data_023c0000[0] = 0;
        data_020a6134.isNeedRescheduling = 1;
        uint8_t *t;
        memcpy(&t, reinterpret_cast<uint8_t *>(&data_020a6134) + 0x0c, 4);
        for (int n = 0; t && n < 64; ++n) {
            uint32_t id;
            memcpy(&id, t + 0x6c, 4);
            if (q & (1u << id)) {
                const int32_t ready = 1;
                memcpy(t + 0x64, &ready, 4);
            }
            memcpy(&t, t + 0x68, 4);
        }
    }
    if (data_020a6134.irqDepth == 0 && data_020a6134.isNeedRescheduling) {
        data_020a6134.isNeedRescheduling = 0;
        OS_RescheduleThread();
    }
}
constexpr u32 REG_IME = 0x04000208, REG_IE = 0x04000210, REG_IF = 0x04000214;
constexpr u32 IRQ_VBLANK = 1u << 0;

/* One OS_IrqHandler pass over the sources in `mask`. */
void dispatch(u32 mask)
{
    if (!(NTR_MMIO(u16, REG_IME) & 1) || ::ntr::rt_irq_masked() || data_020a6134.irqDepth) return;
    const u32 pending = mask & NTR_MMIO(u32, REG_IE);
    if (!pending) return;
    sm64ds_hostlog("irq", pending, data_020a6134.irqDepth);
    data_020a6134.irqDepth++;
    g_in_irq = true;
    for (u32 bit = 0; bit < 22; ++bit) {
        if (!(pending & (1u << bit))) continue;
        void (*h)(void) = reinterpret_cast<void (*)(void)>(_ZN3IRQ13GetIRQHandlerEj(1u << bit));
        NTR_MMIO(u32, REG_IF) = NTR_MMIO(u32, REG_IF) & ~(1u << bit);
        if (h) h();
    }
    g_in_irq = false;
    data_020a6134.irqDepth--;
    irq_exit();
}

uint64_t g_last_frame_us;
uint64_t now_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void frame_then_vblank()
{
    sm64ds_timers_poll();
    sm64ds_irq_drain();
    ::ntr::rt_vblank_wait();
    g_last_frame_us = now_us();
    NTR_MMIO(u32, REG_IF) = NTR_MMIO(u32, REG_IF) | IRQ_VBLANK;
    dispatch(IRQ_VBLANK);
}
}  // namespace

/* Run fn(arg) as an interrupt handler would run: inside OS_IrqHandler's
   bracket, so a thread it wakes is switched to on the way out. The ARM7
   stand-in delivers its FIFO replies this way (port/vita/arm7_host.cpp). */
extern "C" void sm64ds_irq_context(void (*fn)(void *), void *arg)
{
    const bool nested = g_in_irq;
    data_020a6134.irqDepth++;
    g_in_irq = true;
    fn(arg);
    g_in_irq = nested;
    data_020a6134.irqDepth--;
    irq_exit();
}

/* Interrupt work posted from outside the game's flow -- an ARM7 reply, the
   end of an asynchronous card read: run as an interrupt handler at the next
   point the DS would take an interrupt (interrupts unmasked, or a BIOS wait).
   As on the DS's IPC and card-DMA interrupts, IME is not consulted: the game
   loads its first overlay before it turns IME on. */
namespace {
struct Work { void (*fn)(uintptr_t, uintptr_t, uintptr_t); uintptr_t a, b, c; };
constexpr int WMAX = 128;
Work g_work[WMAX];
int g_wh, g_wt;
bool g_whook;
void run_work(void *p)
{
    const Work *w = static_cast<const Work *>(p);
    w->fn(w->a, w->b, w->c);
}
}  // namespace

/* A source's IF bit set and OS_IrqHandler run for it (the timers). */
extern "C" void sm64ds_irq_raise(u32 mask)
{
    NTR_MMIO(u32, REG_IF) = NTR_MMIO(u32, REG_IF) | mask;
    dispatch(mask);
}

extern "C" void sm64ds_irq_drain(void)
{
    // Not from inside a handler (irqDepth > 0): the DS takes no interrupt
    // there either. A handler's wake-up may switch threads on its way out
    // (irq_exit): the rest of the queue is then taken by whichever thread
    // next unmasks, as a pending interrupt would be -- no flag is left set
    // for the thread switched to.
    if (::ntr::rt_irq_masked() || data_020a6134.irqDepth) return;
    while (g_wh != g_wt) {
        Work w = g_work[g_wh];
        g_wh = (g_wh + 1) % WMAX;
        sm64ds_hostlog("run-posted", (uint32_t)(uintptr_t)w.fn, (uint32_t)w.a);
        sm64ds_irq_context(run_work, &w);
        if (::ntr::rt_irq_masked()) break;  // resumed in a thread that runs masked
    }
}

/* Interrupts unmasked: posted work runs, and every 32nd time the timers are
   looked at (a timer that came due while the game ran raises its IRQ here). */
/* Interrupts unmasked: posted work runs; every 32nd time the timers are
   looked at, and VBlank comes if its time is due. On the DS VBlank arrives
   every 1/60 s whatever the CPU is doing: a thread that computes for long
   without waiting (the boot's background loader, func_0201a2f8, building the
   asset table) is interrupted, and the main loop gets its frames. The BIOS
   waits still give VBlank at once when a thread does wait. */
static void on_unmask()
{
    static unsigned n;
    if ((++n & 31) == 0) {
        sm64ds_timers_poll();
        if (!data_020a6134.irqDepth && !g_in_irq && now_us() - g_last_frame_us >= 16667) {
            sm64ds_hostlog("vblank-due", (uint32_t)n, 0);
            frame_then_vblank();
            return;
        }
    }
    sm64ds_irq_drain();
}

extern "C" int sm64ds_irq_queued(void) { return (g_wt - g_wh + WMAX) % WMAX; }

/* Before the game runs: interrupt work and the timers are looked at from
   the first unmask on. */
extern "C" void sm64ds_irq_host_init(void)
{
    g_whook = true;
    ::ntr::rt_set_unmask_hook(on_unmask);
}

extern "C" void sm64ds_irq_post(void (*fn)(uintptr_t, uintptr_t, uintptr_t), uintptr_t a,
                                uintptr_t b, uintptr_t c)
{
    if (!g_whook) {
        g_whook = true;
        ::ntr::rt_set_unmask_hook(on_unmask);
    }
    const int next = (g_wt + 1) % WMAX;
    if (next == g_wh) {
        fprintf(stderr, "irq: posted work queue full, dropped\n");
        return;
    }
    g_work[g_wt] = {fn, a, b, c};
    g_wt = next;
    sm64ds_hostlog("post", (uint32_t)(uintptr_t)fn, (uint32_t)a);
}

/* The SDK's processor-mode query: OS_RescheduleThread defers a switch asked
   for from inside an interrupt handler, as on the DS. */
extern "C" int ARMProcessorMode(void) { return g_in_irq ? 0x12 : 0x1f; }

/* BIOS waits (C++ linkage: the decomp's .c spellings, see port/ntr/runtime.cpp) */
void VBlankIntrWait(void) { frame_then_vblank(); }
void IntrWait(void) { frame_then_vblank(); }
void Halt(void) { frame_then_vblank(); }
