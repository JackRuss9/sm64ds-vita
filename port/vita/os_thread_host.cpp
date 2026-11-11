// The DS's threads on host threads (full-game GCC build).
//
// The game keeps its own NitroSDK scheduler (src/engine/os/OS_Thread.cpp):
// thread list, priorities, states, wait queues, OS_SelectThread. Only the three
// hardware steps under it are replaced here, which is all the scheduler asks of
// the CPU:
//
//   OS_InitContext (func_02058568)  record where a new thread starts
//   ARMSaveContext                  "save the running thread": returns 0
//   ARMRestoreContext               switch to another thread
//
// Each OSThread runs on its own host thread (a native SceKernel thread on the
// PS Vita -- the Vita kernel stops a process whose stack pointer leaves its
// thread's registered stack, so the DS's own stacks cannot be switched onto).
// One CPU token makes them take turns: exactly one game thread runs at any
// moment, as on the DS's single ARM9. OS_RescheduleThread's two exits -- the
// one after ARMSaveContext returns nonzero (the thread resumed), and the one
// after ARMRestoreContext -- both simply return, so ARMRestoreContext here
// hands the token over and returns once the token comes back.
//
// The DS thread's stack (stack/stackSize in OS_CreateThread) stays the game's
// bookkeeping (its check words are still written and read); the code runs on
// the host thread's own stack.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
extern "C" __attribute__((weak)) void sm64ds_log_flush(void);   /* vita_main.cpp */

extern "C" void sm64ds_hostlog(const char *what, uint32_t a, uint32_t b);  /* irq_host.cpp */
namespace ntr {
bool rt_irq_masked();
void rt_irq_set_masked(bool masked);
}  // namespace ntr

extern "C" void sm64ds_thread_started(void) __attribute__((weak));   /* vita_display.cpp */

namespace {

// OSContext (NitroSDK): cpsr at +0, r0..r12 at +4, sp +0x38, lr +0x3c, pc+4 +0x40
constexpr unsigned CTX_R0 = 0x04, CTX_LR = 0x3c;

struct HostThread {
    void *ctx;              // the OSThread's OSContext: the identity used here
    uint32_t entry;         // thread function (host address)
    bool started;
    pthread_t th;
};

constexpr int MAX_THREADS = 32;     // NitroSDK OS_THREAD_MAX_NUM is 16
HostThread g_threads[MAX_THREADS];
int g_nthreads;

pthread_mutex_t g_lock;
pthread_cond_t g_cv;
bool g_ready;
void *volatile g_running;           // ctx of the thread holding the CPU token
// Which DS thread a host thread runs, looked up by pthread_self() -- not a
// __thread variable: thread-local storage is not to be relied on with every
// toolchain (on the PS Vita, a hang after the first thread switch pointed at
// it), a table under g_lock is.
struct SelfEntry { pthread_t th; void *ctx; };
SelfEntry g_self[MAX_THREADS + 1];
int g_nself;
void *self_get()                    // g_lock held
{
    const pthread_t me = pthread_self();
    for (int i = 0; i < g_nself; ++i)
        if (pthread_equal(g_self[i].th, me)) return g_self[i].ctx;
    return nullptr;
}
void self_set(void *ctx)            // g_lock held
{
    const pthread_t me = pthread_self();
    for (int i = 0; i < g_nself; ++i)
        if (pthread_equal(g_self[i].th, me)) { g_self[i].ctx = ctx; return; }
    if (g_nself < MAX_THREADS + 1) g_self[g_nself++] = {me, ctx};
}

void init_once()
{
    if (g_ready) return;
    pthread_mutex_init(&g_lock, nullptr);
    pthread_cond_init(&g_cv, nullptr);
    g_ready = true;
}

HostThread *find(void *ctx)
{
    for (int i = 0; i < g_nthreads; ++i)
        if (g_threads[i].ctx == ctx) return &g_threads[i];
    return nullptr;
}


// A wait that re-checks its condition at least every 2 ms. VitaSDK's
// pthreads (pthreads-embedded) lose a condition-variable wake-up now and then
// under heavy hand-offs (seen on hardware: the turn given, the waiter asleep
// for good); the caller's while-loop then picks the change up regardless.
void cond_wait_2ms(pthread_cond_t *cv, pthread_mutex_t *m)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 2000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(cv, m, &ts);
}

void wait_for_token(void *self)     // g_lock held
{
    while (g_running != self) cond_wait_2ms(&g_cv, &g_lock);
}

void *thread_main(void *p)
{
    if (sm64ds_thread_started) sm64ds_thread_started();
    HostThread *ht = static_cast<HostThread *>(p);
    pthread_mutex_lock(&g_lock);
    self_set(ht->ctx);
    wait_for_token(ht->ctx);
    pthread_mutex_unlock(&g_lock);
    // A new thread starts with the CPSR OS_CreateThread gave it (ctx +0):
    // its I bit, not whatever the thread that switched here had.
    uint32_t cpsr;
    memcpy(&cpsr, ht->ctx, 4);
    ::ntr::rt_irq_set_masked(cpsr & 0x80);
    // OS_CreateThread wrote the argument to r0 and OS_ExitThread to lr after
    // OS_InitContext: read them now, at first run, as the CPU would.
    uint32_t arg, lr;
    memcpy(&arg, (char *)ht->ctx + CTX_R0, 4);
    memcpy(&lr, (char *)ht->ctx + CTX_LR, 4);
    ((void (*)(void *))(uintptr_t)ht->entry)((void *)(uintptr_t)arg);
    // Returning from a thread function lands in lr: OS_ExitThread, which marks
    // the thread terminated and switches away for good.
    ((void (*)(void))(uintptr_t)lr)();
    return nullptr;
}

// Host threads need far more stack than the DS gives its threads (0x400 bytes
// for the sound and card threads): the host code is not the DS code.
constexpr size_t THREAD_STACK = 256 * 1024;

}  // namespace

extern "C" {

/* OS_InitContext(ctx, pc, sp): the new thread starts at pc. */
void func_02058568(void *ctx, uint32_t pc, uint32_t sp)
{
    (void)sp;
    init_once();
    pthread_mutex_lock(&g_lock);
    HostThread *ht = find(ctx);
    if (!ht) {
        if (g_nthreads == MAX_THREADS) {
            fprintf(stderr, "FATAL: more than %d DS threads\n", MAX_THREADS);
            if (sm64ds_log_flush) sm64ds_log_flush();
            abort();
        }
        ht = &g_threads[g_nthreads++];
        ht->ctx = ctx;
    } else if (ht->started) {
        fprintf(stderr, "FATAL: OS_InitContext on a running thread\n");
        if (sm64ds_log_flush) sm64ds_log_flush();
        abort();
    }
    ht->entry = pc;
    ht->started = false;
    // the SDK's OS_InitContext: system mode, interrupts on (Thumb if pc is)
    const uint32_t cpsr = 0x1f | ((pc & 1) ? 0x20 : 0);
    memcpy(ctx, &cpsr, 4);
    pthread_mutex_unlock(&g_lock);
}

/* OS_SaveContext: the running thread's state lives on its host thread. The
   first caller is the launcher thread -- the one NitroMain runs on. */
int ARMSaveContext(void *ctx)
{
    init_once();
    pthread_mutex_lock(&g_lock);
    if (!self_get()) {
        self_set(ctx);
        if (!g_running) g_running = ctx;
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* OS_LoadContext(next): give the CPU to `next`; come back when it is ours. */
void ARMRestoreContext(void *next)
{
    init_once();
    // The I bit is part of the thread's state on the DS (its saved CPSR): kept
    // here across the switch, put back when this thread runs again.
    const bool my_mask = ::ntr::rt_irq_masked();
    pthread_mutex_lock(&g_lock);
    void *self = self_get();
    sm64ds_hostlog("switch", (uint32_t)(uintptr_t)self, (uint32_t)(uintptr_t)next);
    g_running = next;
    HostThread *ht = find(next);
    if (ht && !ht->started) {
        ht->started = true;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, THREAD_STACK);
        if (pthread_create(&ht->th, &attr, thread_main, ht) != 0) {
            fprintf(stderr, "FATAL: cannot create a host thread for a DS thread\n");
            if (sm64ds_log_flush) sm64ds_log_flush();
            abort();
        }
        pthread_attr_destroy(&attr);
    }
    pthread_cond_broadcast(&g_cv);
    // A thread that switched away from OS_ExitThread is never scheduled again:
    // its host thread waits here for good (the DS reuses neither).
    wait_for_token(self);
    pthread_mutex_unlock(&g_lock);
    ::ntr::rt_irq_set_masked(my_mask);
}

}  // extern "C"

/* For the watchdog (tests/smoke_boot.cpp): the host address a DS thread was
   started at, 0 if this layer never saw it, and whether it holds the CPU. */
extern "C" uint32_t sm64ds_thread_entry(void *ctx)
{
    HostThread *ht = find(ctx);
    return ht ? ht->entry : 0;
}
extern "C" void *sm64ds_thread_running(void) { return g_running; }
