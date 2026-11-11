#include <time.h>
#include "ntr/rt.h"

#include "ntr/mmio.h"

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// ---------------------------------------------------------------------------
// The game "fiber" on hosts without Windows fibers (PS Vita; Linux for tests).
//
// The game runs on its own OS thread, and the two threads hand a single turn
// back and forth: exactly one of them runs at any moment, so this has the same
// semantics as the Windows fibers -- no races, game control flow untouched.
//
// Why a real thread and not a hand-rolled stack switch: on the PS Vita a
// switch onto a malloc'd stack ran the game for eight frames and was then
// stopped by the kernel (stop reason 0x10006) with sp outside the thread's
// registered stack. A thread gives the game a stack the kernel itself created.
// VitaSDK's pthread (pthread-embedded) creates native SceKernel threads.
// ---------------------------------------------------------------------------
#if !defined(_WIN32)
#define NTR_THREAD_FIBERS 1
#include <pthread.h>
#endif

namespace ntr {
/* Called first on each thread that runs the game (a host port can pin it to
   a core: port/vita/vita_display.cpp). Optional. */
extern "C" void sm64ds_thread_started(void) __attribute__((weak));

namespace {

constexpr uint32_t REG_IME = 0x04000208;
constexpr uint32_t REG_IE = 0x04000210;
constexpr uint32_t REG_IF = 0x04000214;
constexpr uint32_t REG_VCOUNT = 0x04000006;

inline volatile uint32_t &reg32(uint32_t a) {
    return *static_cast<volatile uint32_t *>(host_ptr(a));
}
inline volatile uint16_t &reg16(uint32_t a) {
    return *static_cast<volatile uint16_t *>(host_ptr(a));
}

struct Runtime {
    void *host_fiber = nullptr;   // Windows: fiber handles
    void *game_fiber = nullptr;
    bool game_running = false;    // threads: the game thread exists
    void (*entry)() = nullptr;
    FrameHook hook = nullptr;
    uint64_t frame = 0;
    uint64_t max_frames = 0;
    bool game_done = false;
    bool stop = false;
    uint32_t cpsr_i = 0;     // 0x80 when interrupts are masked
};

Runtime g;

#if defined(_WIN32)
void CALLBACK game_trampoline(void *) {
    g.entry();
    g.game_done = true;
    // The game returned. Hand control back for good.
    for (;;) SwitchToFiber(g.host_fiber);
}
#elif defined(NTR_THREAD_FIBERS)
// One turn token. `turn` says whose turn it is; each side waits for its own.
enum Turn { HOST, GAME };
// Initialised by function, not PTHREAD_*_INITIALIZER: not every embedded
// pthread provides the static initialisers.
pthread_mutex_t g_lock;
pthread_cond_t g_cv;
bool g_sync_ready = false;
volatile Turn g_turn = HOST;
bool g_abandon = false;           // host stopped early: the game thread must exit


// A wait that re-checks its condition at least every 2 ms. VitaSDK's
// pthreads (pthreads-embedded) lose a condition-variable wake-up now and then
// under heavy hand-offs (seen on hardware: the turn given, the waiter asleep
// for good); the caller's while-loop then picks the change up regardless.
static void cond_wait_2ms(pthread_cond_t *cv, pthread_mutex_t *m)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 2000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(cv, m, &ts);
}

void give_turn(Turn to) {         // caller holds g_lock
    g_turn = to;
    pthread_cond_broadcast(&g_cv);
}
void wait_turn(Turn mine) {       // caller holds g_lock
    while (g_turn != mine) cond_wait_2ms(&g_cv, &g_lock);
}

void *game_thread(void *) {
    if (sm64ds_thread_started) sm64ds_thread_started();
    pthread_mutex_lock(&g_lock);
    wait_turn(GAME);
    pthread_mutex_unlock(&g_lock);
    if (!g_abandon) g.entry();
    pthread_mutex_lock(&g_lock);
    g.game_done = true;
    give_turn(HOST);
    pthread_mutex_unlock(&g_lock);
    return nullptr;
}

// The game thread's stack: generous, the Windows fiber had 256 KB.
constexpr size_t GAME_STACK = 1024 * 1024;
#endif

// Advance the scanline counter across a frame. Anything spinning on VCOUNT --
// the decomp does, in func_02013f4c -- needs this to actually move.
void run_scanlines() {
    for (uint16_t line = 0; line < 263; ++line) reg16(REG_VCOUNT) = line;
}

}  // namespace

void rt_vblank_wait() {
#if defined(_WIN32)
    if (!g.game_fiber) return;   // not running under rt_run; nothing to yield to
    reg32(REG_IF) |= IRQ_VBLANK;
    SwitchToFiber(g.host_fiber);
#elif defined(NTR_THREAD_FIBERS)
    if (!g.game_running) return;   // not running under rt_run; nothing to yield to
    reg32(REG_IF) |= IRQ_VBLANK;
    pthread_mutex_lock(&g_lock);
    give_turn(HOST);
    wait_turn(GAME);
    const bool abandon = g_abandon;
    pthread_mutex_unlock(&g_lock);
    if (abandon) pthread_exit(nullptr);   // host stopped the loop: end this thread
#endif
}

uint64_t rt_frame() { return g.frame; }

uint32_t rt_irq_disable() {
    const uint32_t prev = g.cpsr_i;
    g.cpsr_i = 0x80;
    return prev;
}

void rt_irq_set_masked(bool masked) { g.cpsr_i = masked ? 0x80 : 0; }

static volatile int g_host_stage;
void rt_debug_state(int *turn, int *host_stage) {
#if defined(NTR_THREAD_FIBERS)
    *turn = g_turn == HOST ? 0 : 1;
#else
    *turn = -1;
#endif
    *host_stage = g_host_stage;
}

static void (*g_unmask_hook)();
void rt_set_unmask_hook(void (*hook)()) { g_unmask_hook = hook; }

uint32_t rt_irq_enable() {
    const uint32_t prev = g.cpsr_i;
    g.cpsr_i = 0;
    if (prev && g_unmask_hook) g_unmask_hook();
    return prev;
}

uint32_t rt_irq_restore(uint32_t prev) {
    const uint32_t was = g.cpsr_i;
    g.cpsr_i = prev & 0x80;
    if (was && !g.cpsr_i && g_unmask_hook) g_unmask_hook();
    return was;
}

bool rt_irq_masked() { return g.cpsr_i != 0; }

uint64_t rt_run(void (*game)(), FrameHook hook, uint64_t max_frames) {
#if defined(NTR_THREAD_FIBERS)
    if (!io_init()) {
        std::fprintf(stderr, "rt_run: io_init failed\n");
        return 0;
    }
    g = Runtime{};
    g.entry = game;
    g.hook = hook;
    g.max_frames = max_frames;
    if (!g_sync_ready) {
        pthread_mutex_init(&g_lock, nullptr);
        pthread_cond_init(&g_cv, nullptr);
        g_sync_ready = true;
    }
    g_turn = HOST;
    g_abandon = false;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, GAME_STACK);
    pthread_t th;
    const int rc = pthread_create(&th, &attr, game_thread, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        std::fprintf(stderr, "rt_run: cannot create the game thread (%d)\n", rc);
        return 0;
    }
    g.game_running = true;

    reg32(REG_IME) = 1;
    reg32(REG_IE) = IRQ_VBLANK;

    while (!g.game_done && !g.stop) {
        g_host_stage = 0;
        pthread_mutex_lock(&g_lock);          // run the game until it blocks
        give_turn(GAME);
        wait_turn(HOST);
        pthread_mutex_unlock(&g_lock);
        if (g.game_done) break;

        g_host_stage = 1;
        run_scanlines();
        ++g.frame;
        g_host_stage = 2;
        if (g.hook && !g.hook(g.frame)) g.stop = true;
        if (g.max_frames && g.frame >= g.max_frames) g.stop = true;
        reg32(REG_IF) &= ~IRQ_VBLANK;
    }
    if (!g.game_done) {                       // stopped early: release the game
        pthread_mutex_lock(&g_lock);
        g_abandon = true;
        give_turn(GAME);
        pthread_mutex_unlock(&g_lock);
    }
    pthread_join(th, nullptr);
    g.game_running = false;
    return g.frame;
#else
    if (!io_init()) {
        std::fprintf(stderr, "rt_run: io_init failed\n");
        return 0;
    }

    g = Runtime{};
    g.entry = game;
    g.hook = hook;
    g.max_frames = max_frames;

    g.host_fiber = ConvertThreadToFiber(nullptr);
    if (!g.host_fiber) {
        std::fprintf(stderr, "rt_run: ConvertThreadToFiber failed\n");
        return 0;
    }
    g.game_fiber = CreateFiber(256 * 1024, game_trampoline, nullptr);
    if (!g.game_fiber) {
        std::fprintf(stderr, "rt_run: CreateFiber failed\n");
        return 0;
    }

    // The DS comes up with interrupts enabled and IME set by the CRT0.
    reg32(REG_IME) = 1;
    reg32(REG_IE) = IRQ_VBLANK;

    while (!g.game_done && !g.stop) {
        SwitchToFiber(g.game_fiber);          // run until the game blocks
        if (g.game_done) break;

        run_scanlines();
        ++g.frame;
        if (g.hook && !g.hook(g.frame)) g.stop = true;
        if (g.max_frames && g.frame >= g.max_frames) g.stop = true;

        // The handler is what clears the pending flag on real hardware.
        reg32(REG_IF) &= ~IRQ_VBLANK;
    }

    DeleteFiber(g.game_fiber);
    g.game_fiber = nullptr;
    ConvertFiberToThread();
    return g.frame;
#endif
}

}  // namespace ntr
