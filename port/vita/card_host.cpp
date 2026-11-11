// The game card's ROM for the full-game GCC build (PS Vita).
//
// The NitroSDK FS library (src/engine/fs) keeps working unchanged: it walks the
// ROM's FNT/FAT itself and reads through one function, CARDi_ReadRom
// (src/CARDi_ReadRom.cpp), which on the DS drives the card bus by DMA or on
// the card thread. Only that function is replaced here: the bytes come from the
// user's dump. The request is claimed and released exactly as the SDK does it
// (the busy flag, its wait queue, CARDi_ReadEnd's wake-ups and callback), so
// every caller -- synchronous or asynchronous -- sees the end it expects:
//
//   synchronous   read, then CARDi_ReadEnd on the caller, as the SDK's task
//                 path does;
//   asynchronous  read, then CARDi_ReadEnd posted as the card-DMA interrupt,
//                 run at the next point the DS takes an interrupt -- after the
//                 caller has gone on to wait for it, as it relies on.
//
// The boot also leaves in the shared work area what the DS's loader leaves
// there: the ROM header (0x027ffe00, which FSi_InitRom reads the FNT/FAT
// regions from) and the card ID (0x027ff800, 0x027ffc00).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ntr/mmio.h"

typedef uint32_t u32;
typedef void (*MIDmaCallback)(void *);

extern "C" {
const char *sm64ds_rom_path(void);
void sm64ds_irq_post(void (*fn)(uintptr_t, uintptr_t, uintptr_t), uintptr_t a, uintptr_t b,
                     uintptr_t c);
void CARDi_ReadEnd(void);
void OS_SleepThread(void *queue);
void OS_WakeupThread(void *queue);
void OS_WakeupThreadDirect(void *thread);
void sm64ds_hostlog(const char *what, uint32_t a, uint32_t b);
/* cardi_common (include/nitro/card/card_common.h): the fields used here */
struct CardCommonHost {
    void *cmd;
    int32_t command;
    volatile int lock_owner, lock_ref;
    uint16_t lock_queue, pad12;
    int lock_target;
    u32 src, dst, len, dma;
    MIDmaCallback callback;
    void *callback_arg;
    void *task_func;
    volatile u32 flag;
    int priority;
    unsigned char thread[0x94];
    void *cur_th;
    uint16_t busy_q;
};
extern CardCommonHost cardi_common;
}
namespace IRQ {
u32 Disable(void);
u32 Restore(u32 saved);
}  // namespace IRQ

static_assert(__builtin_offsetof(CardCommonHost, flag) == 0x34, "CARDiCommon.flag");
static_assert(__builtin_offsetof(CardCommonHost, cur_th) == 0xd0, "CARDiCommon.cur_th");
static_assert(__builtin_offsetof(CardCommonHost, busy_q) == 0xd4, "CARDiCommon.busy_q");

namespace {
constexpr u32 CARD_STAT_BUSY = 0x04;
FILE *g_rom;
long g_rom_size;

bool rom_open()
{
    if (g_rom) return true;
    g_rom = fopen(sm64ds_rom_path(), "rb");
    if (!g_rom) {
        fprintf(stderr, "card: cannot open %s\n", sm64ds_rom_path());
        return false;
    }
    fseek(g_rom, 0, SEEK_END);
    g_rom_size = ftell(g_rom);
    return true;
}

/* Time and count of the reads, for the log's slow-frame lines. */
u32 g_reads, g_read_bytes, g_disk_reads;
uint64_t g_read_us;
uint64_t now_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void disk_read(u32 off, void *dst, u32 len, u32 *got)
{
    ++g_disk_reads;
    fseek(g_rom, (long)off, SEEK_SET);
    *got = (u32)fread(dst, 1, len, g_rom);
}

/* The SDK's file system reads the ROM's tables (the FNT when it looks a
   path up, the FAT, archive headers) a few bytes at a time, and each read
   was a seek and a read on the memory card. Small reads now come from a few
   32 KB blocks kept in memory; big ones go straight to the card. */
constexpr u32 BLK = 32 * 1024, NBLK = 16;
struct Block { u32 off; u32 len; u32 used; unsigned char *data; };
Block g_blk[NBLK];
u32 g_tick;

bool cached_read(u32 off, void *dst, u32 len)
{
    if (len > 4096) return false;
    const u32 base = off & ~(BLK - 1);
    if (off + len > base + BLK) return false;        // straddles two blocks
    Block *b = nullptr, *lru = &g_blk[0];
    for (Block &c : g_blk) {
        if (c.data && c.off == base) { b = &c; break; }
        if (!c.data || c.used < lru->used) lru = &c;
    }
    if (!b) {
        b = lru;
        if (!b->data) b->data = (unsigned char *)malloc(BLK);
        if (!b->data) return false;
        b->off = base;
        disk_read(base, b->data, BLK, &b->len);
    }
    b->used = ++g_tick;
    if (off + len > base + b->len) return false;     // the dump's end: as before
    memcpy(dst, b->data + (off - base), len);
    return true;
}

void rom_read(u32 off, void *dst, u32 len)
{
    if (!rom_open()) {
        memset(dst, 0xff, len);
        return;
    }
    const uint64_t t0 = now_us();
    ++g_reads;
    g_read_bytes += len;
    u32 n = 0;
    if (cached_read(off, dst, len)) {
        n = len;
    } else if ((long)off < g_rom_size) {
        n = (long)(off + len) > g_rom_size ? (u32)(g_rom_size - off) : len;
        disk_read(off, dst, n, &n);
    }
    if (n < len) memset((char *)dst + n, 0xff, len - n);   // past the dump: open bus
    g_read_us += now_us() - t0;
}

// CARDi_ReadEnd, which also stores the result through cardi_common.cmd: set
// by CARD_Init, which the game may not have run before its first file read.
void read_end()
{
    CardCommonHost *const c = &cardi_common;
    sm64ds_hostlog("rom-end", (u32)(uintptr_t)c->callback, c->flag);
    if (c->cmd) {
        CARDi_ReadEnd();
        return;
    }
    MIDmaCallback cb = c->callback;
    void *arg = c->callback_arg;
    const u32 mask = IRQ::Disable();
    c->flag &= ~(CARD_STAT_BUSY | 0x08u);
    OS_WakeupThread(&c->busy_q);
    if (c->flag & 0x10u) OS_WakeupThreadDirect(c->thread);
    IRQ::Restore(mask);
    if (cb) cb(arg);
}
void end_async(uintptr_t, uintptr_t, uintptr_t) { read_end(); }
}  // namespace

extern "C" {

void __wrap_CARDi_ReadRom(u32 dma, const void *src, void *dst, u32 len, MIDmaCallback callback,
                          void *arg, int is_async)
{
    CardCommonHost *const c = &cardi_common;
    {
        const u32 mask = IRQ::Disable();
        while (c->flag & CARD_STAT_BUSY) OS_SleepThread(&c->busy_q);
        c->flag |= CARD_STAT_BUSY;
        IRQ::Restore(mask);
    }
    c->dma = dma;
    c->src = (u32)(uintptr_t)src;
    c->dst = (u32)(uintptr_t)dst;
    c->len = len;
    c->callback = callback;
    c->callback_arg = arg;
    sm64ds_hostlog(is_async ? "rom-async" : "rom-sync", (u32)(uintptr_t)src, len);
    // a read straight into palette or VRAM names it by its DS address
    rom_read((u32)(uintptr_t)src, ::ntr::host_ptr((uintptr_t)dst), len);
    if (is_async)
        sm64ds_irq_post(end_async, 0, 0, 0);
    else
        read_end();
}

/* The card reads so far: how many, bytes, how many reached the memory card,
   and the time they took. */
void sm64ds_card_stats(u32 *reads, u32 *bytes, u32 *disk, uint64_t *us)
{
    *reads = g_reads;
    *bytes = g_read_bytes;
    *disk = g_disk_reads;
    *us = g_read_us;
}

/* What the DS's boot loader leaves in the shared area before the ARM9 runs. */
void sm64ds_card_boot_setup(void)
{
    unsigned char header[0x160];
    rom_read(0, header, sizeof header);
    memcpy(::ntr::host_ptr(0x027ffe00), header, sizeof header);
    const u32 chip_id = 0x00000fc2;     // a 128 Mbit mask ROM (Macronix)
    memcpy(::ntr::host_ptr(0x027ff800), &chip_id, 4);
    memcpy(::ntr::host_ptr(0x027ffc00), &chip_id, 4);
}

}  // extern "C"
