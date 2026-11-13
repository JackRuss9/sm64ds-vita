// Gate-11 smoke (GCC / PS Vita): the game boots by itself.
//
// Everything gate 10 links, run for real: the ROM-resident globals are
// loaded, the ARM9's static initializers run (the DS's crt0 does that before
// NitroMain), and then NitroMain -- the game's own entry point, src/NitroMain.cpp
// -- runs on the frame loop's game thread, after NitroStartUp: OS/heap/FS/display init, the boot
// scene, then the main loop (src/unnamed/arm9/0201/func_020197b8.cpp). DS
// threads run on host threads (port/vita/os_thread_host.cpp), VBlank is
// delivered to the game's own handler table (port/vita/irq_host.cpp), overlays
// load through port/vita/overlay_host.cpp.
//
// The smoke does not judge pixels. It logs how far the game gets -- which
// main-loop step it is in (data_0209d50c, the loop writes it before every
// call), the loop's own frame counter (data_020a0db0), the overlays it loads --
// and saves both screens at a few frames as BMPs, composed as the DS shows
// them, and the 3D engine's picture alone:
// ux0:data/sm64ds/boot_<frame>_{top,bottom,3d}.bmp.
// PASS = the main loop runs for the whole window. On the Vita the game then
// keeps running, playable (port/vita/input_host.cpp), until it is closed; a
// tap on the rear touch panel saves the screens and a display snapshot.
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>

#include <exception>
#include <new>
#include <stdexcept>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#include "ntr/gx.h"
#include "ntr/mmio.h"
#include "ntr/ppu.h"
#include "ntr/rt.h"

extern "C" {
bool sm64ds_romimage_load(const char *rom_path);
void sm64ds_romimage_run_inits(int module);
void NitroStartUp(void);
void sm64ds_card_boot_setup(void);  /* port/vita/card_host.cpp */
void sm64ds_irq_host_init(void);    /* port/vita/irq_host.cpp */
void NitroMain(void);
extern int data_0209d50c;           /* main-loop step marker */
extern int data_020a0db0;           /* main-loop frame counter */
uint32_t sm64ds_thread_entry(void *ctx);
bool sm64ds_display_init(void);     /* port/vita/vita_display.cpp */
void sm64ds_display_present(void);
unsigned sm64ds_display_dropped(void);
void sm64ds_display_tick(void);
void sm64ds_thread_started(void);   /* port/vita/vita_display.cpp */
struct BootThreadInfo { uint16_t resched, max, depth, pad; uint8_t *current, *list; };
extern BootThreadInfo data_020a6134;    /* OSi_ThreadInfo */
void *sm64ds_thread_running(void);
void sm64ds_hostlog_dump(void);     /* port/vita/irq_host.cpp */
void sm64ds_input_init(void);       /* port/vita/input_host.cpp */
void sm64ds_input_frame(void);
bool sm64ds_input_capture_requested(void);
void sm64ds_arm7_frame(void);       /* port/vita/arm7_host.cpp */
extern uint8_t data_0209e660;               /* OAM: single-buffer flag */
extern int data_0209e664, data_0209e670;    /* OAM: main / sub fill counts */
extern uint8_t data_0209e674[], data_0209ea74[];    /* OAM: main / sub staging, 0x400 each */
extern uint8_t data_020a1040[];     /* this frame's input record (func_0203df40) */
extern uint8_t data_020a1154[];     /* the four players' copies (func_0203e0ac) */
extern uint8_t data_020a80cc[];     /* TPWork (include/nitro/spi/tp.h) */
extern uint8_t *data_ov007_0210342c;    /* the title's work block (include/Ov007Work.h) */
}

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#endif
#include <time.h>
extern "C" __attribute__((weak)) void sm64ds_log_flush(void);   /* vita_main.cpp */

/* End the process. On the Vita a plain _exit lets the other threads run on
   into the C library's teardown, where one faulted (a crash report after a
   PASS); sceKernelExitProcess stops them all at once. */
extern "C" void sm64ds_arm7_flush_save(void);   /* port/vita/arm7_host.cpp */
static void quit(int code)
{
    sm64ds_arm7_flush_save();
    fflush(stdout);
    fflush(stderr);
    if (sm64ds_log_flush) sm64ds_log_flush();
#if defined(__vita__)
    sceKernelExitProcess(code);
#endif
    _exit(code);
}

#ifndef SM64DS_ROM_PATH
#define SM64DS_ROM_PATH "ux0:data/sm64ds/sm64ds.nds"
#endif
#ifndef SM64DS_OUT_DIR
#define SM64DS_OUT_DIR "ux0:data/sm64ds"
#endif

static const uint64_t WINDOW = 1800;            /* thirty seconds of DS frames */
static int g_last_step = -1, g_last_count = -1, g_loop_frames;

static void start_watchdog();
/* what the frame loop (the host side) is doing, for the watchdog */
static const char *volatile g_host_phase = "game's turn";
/* Set while the frame hook writes files (snapshots, screenshots): the memory
   card is slow, and a long write is not the game being stuck. */
static volatile int g_host_io;


static void capture(uint64_t frame)
{
    static ntr::Framebuffer a, b, g3d;
    char name[128];
    struct Busy { Busy() { ++g_host_io; } ~Busy() { --g_host_io; } } busy;
    const bool has3d = NTR_MMIO(uint32_t, 0x04000000) & 0x8;
    ntr::gx_render_layer(g3d);
    ntr::ppu_compose(ntr::ENGINE_A, a, has3d ? &g3d : nullptr);
    ntr::ppu_master_brightness(ntr::ENGINE_A, a);
    ntr::ppu_compose(ntr::ENGINE_B, b, nullptr);
    ntr::ppu_master_brightness(ntr::ENGINE_B, b);
    const bool a_top = NTR_MMIO(uint16_t, 0x04000304) & 0x8000;     // POWCNT1 bit 15
    snprintf(name, sizeof name, SM64DS_OUT_DIR "/boot_%04u_top.bmp", (unsigned)frame);
    ntr::ppu_write_bmp(name, a_top ? a : b);
    snprintf(name, sizeof name, SM64DS_OUT_DIR "/boot_%04u_bottom.bmp", (unsigned)frame);
    ntr::ppu_write_bmp(name, a_top ? b : a);
    // the 3D picture alone, over black
    for (int y = 0; y < ntr::SCREEN_H; ++y)
        for (int x = 0; x < ntr::SCREEN_W; ++x) g3d.px[y][x] = 0xFF000000u;
    ntr::gx_render(g3d);
    snprintf(name, sizeof name, SM64DS_OUT_DIR "/boot_%04u_3d.bmp", (unsigned)frame);
    ntr::ppu_write_bmp(name, g3d);
}

/* Every 5 s: what the display hardware is set to, and whether 3D geometry
   reaches the 3D engine -- what the renderer work goes by. */
/* Where a frame's time goes (milliseconds, summed between speed reports):
   drawing the live view on the Vita (both screens composed, the 3D picture
   rendered, scaled to the display), and the rest -- the game's own work and
   the hand-offs between its threads. */
static double g_ms_present, g_ms_frame;
static unsigned g_n_present;
static double now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void display_report(uint64_t frame)
{
    // speed: DS frames per real second since the last report (60 is full speed)
    {
        static double t_last;
        static uint64_t f_last;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const double t = ts.tv_sec + ts.tv_nsec / 1e9;
        if (t_last > 0 && frame > f_last) {
            const unsigned n = (unsigned)(frame - f_last);
            static unsigned dropped_last;
            const unsigned dropped = sm64ds_display_dropped();
            printf("  [speed @%u] %.1f frames/s over the last %u frames; per frame %.1f ms, of which "
                   "live view %.1f ms (%u handed to the drawing core, %.1f ms each; %u skipped, it was busy);"
                   " C heap in use %u MiB\n",
                   (unsigned)frame, n / (t - t_last), n, g_ms_frame / n, g_ms_present / n, g_n_present,
                   g_n_present ? g_ms_present / g_n_present : 0.0, dropped - dropped_last,
                   (unsigned)(mallinfo().uordblks >> 20));
            dropped_last = dropped;
        }
        g_ms_present = g_ms_frame = 0;
        g_n_present = 0;
        t_last = t;
        f_last = frame;
    }
    uint32_t gs[6];
    ntr::gx_debug_stats(gs);
    printf("  [display @%u] 3D: %u commands, %u vertices, %u swaps, last frame %u triangles, "
           "building %u, projection loads %u\n", (unsigned)frame, (unsigned)gs[0], (unsigned)gs[1],
           (unsigned)gs[2], (unsigned)gs[3], (unsigned)gs[4], (unsigned)gs[5]);
    const uint32_t dca = NTR_MMIO(uint32_t, 0x04000000), dcb = NTR_MMIO(uint32_t, 0x04001000);
    printf("    DISPCNT A %08x B %08x  POWCNT1 %04x  MASTER_BRIGHT A %04x B %04x\n",
           (unsigned)dca, (unsigned)dcb, (unsigned)NTR_MMIO(uint16_t, 0x04000304),
           (unsigned)NTR_MMIO(uint16_t, 0x0400006c), (unsigned)NTR_MMIO(uint16_t, 0x0400106c));
    printf("    BGCNT A %04x %04x %04x %04x  B %04x %04x %04x %04x\n",
           (unsigned)NTR_MMIO(uint16_t, 0x04000008), (unsigned)NTR_MMIO(uint16_t, 0x0400000a),
           (unsigned)NTR_MMIO(uint16_t, 0x0400000c), (unsigned)NTR_MMIO(uint16_t, 0x0400000e),
           (unsigned)NTR_MMIO(uint16_t, 0x04001008), (unsigned)NTR_MMIO(uint16_t, 0x0400100a),
           (unsigned)NTR_MMIO(uint16_t, 0x0400100c), (unsigned)NTR_MMIO(uint16_t, 0x0400100e));
    printf("    VRAMCNT A-I %02x %02x %02x %02x %02x %02x %02x %02x %02x  BLDCNT A %04x B %04x\n",
           (unsigned)NTR_MMIO(uint8_t, 0x04000240), (unsigned)NTR_MMIO(uint8_t, 0x04000241),
           (unsigned)NTR_MMIO(uint8_t, 0x04000242), (unsigned)NTR_MMIO(uint8_t, 0x04000243),
           (unsigned)NTR_MMIO(uint8_t, 0x04000244), (unsigned)NTR_MMIO(uint8_t, 0x04000245),
           (unsigned)NTR_MMIO(uint8_t, 0x04000246), (unsigned)NTR_MMIO(uint8_t, 0x04000248),
           (unsigned)NTR_MMIO(uint8_t, 0x04000249), (unsigned)NTR_MMIO(uint16_t, 0x04000050),
           (unsigned)NTR_MMIO(uint16_t, 0x04001050));
    fflush(stdout);
}

static void finish(uint64_t frames);

/* A display snapshot (ux0:data/sm64ds/snapshot_<frame>.bin): the I/O
   registers, palettes, VRAM and OAM as the host holds them, and every 3D
   command since boot -- enough to redraw that frame offline, exactly, when
   working on the renderers (port/tools/snapshot_render.cpp). Sections:
   "SM64SNAP" then { u32 tag, u32 ds_address, u32 bytes, data }. */
static void write_snapshot(uint64_t frame)
{
    char name[128];
    snprintf(name, sizeof name, SM64DS_OUT_DIR "/snapshot_%04u.bin", (unsigned)frame);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    ++g_host_io;
    static char buf[256 * 1024];          /* big writes: the memory card is slow */
    setvbuf(f, buf, _IOFBF, sizeof buf);
    fwrite("SM64SNAP", 1, 8, f);
    auto section = [&](const char *tag, uint32_t ds, const void *p, uint32_t n) {
        uint32_t h[3];
        memcpy(&h[0], tag, 4);
        h[1] = ds;
        h[2] = n;
        fwrite(h, 4, 3, f);
        fwrite(p, 1, n, f);
    };
    section("IO  ", 0x04000000, ntr::host_ptr(0x04000000), 0x2000);
    section("PLTT", 0x05000000, ntr::host_ptr(0x05000000), 0x800);
    section("OAM ", 0x07000000, ntr::host_ptr(0x07000000), 0x800);
    // VRAM: the four 2D views and the banks' own (LCDC) homes, where the
    // texture and extended-palette banks live -- not the empty 9 MB between
    section("VRAM", 0x06000000, ntr::host_ptr(0x06000000), 0x80000);       /* engine A BG */
    section("VRAM", 0x06200000, ntr::host_ptr(0x06200000), 0x20000);       /* engine B BG */
    section("VRAM", 0x06400000, ntr::host_ptr(0x06400000), 0x40000);       /* engine A OBJ */
    section("VRAM", 0x06600000, ntr::host_ptr(0x06600000), 0x20000);       /* engine B OBJ */
    section("VRAM", 0x06800000, ntr::host_ptr(0x06800000), 0xa4000);       /* banks A-I */
    section("MAIN", 0x027ff000, ntr::host_ptr(0x027ff000), 0x1000);        /* the shared area */
    section("OAMS", 0, data_0209e674, 0x800);     /* the game's two OAM staging buffers */
    size_t words = 0;
    uint32_t frames = 0;
    if (ntr::gx_ring_active()) {                 /* the last second or two, to now */
        const uint32_t *part[2];
        size_t n[2];
        words = ntr::gx_ring_view(part, n, frames);
        uint32_t h[3];
        memcpy(&h[0], "GX3D", 4);
        h[1] = 0;
        h[2] = (uint32_t)(words * 4);
        fwrite(h, 4, 3, f);
        fwrite(part[0], 4, n[0], f);
        if (n[1]) fwrite(part[1], 4, n[1], f);
    } else {
        const uint32_t *w = ntr::gx_record_data(words);
        section("GX3D", 0, w, (uint32_t)(words * 4));
    }
    fclose(f);
    --g_host_io;
    printf("  display snapshot: %s (%u 3D command words, %u frames back)\n", name, (unsigned)words,
           (unsigned)frames);
    fflush(stdout);
}

/* --- The game's sprite lists (src/engine/oam/OAM.cpp), being looked into:
   both screens show sprites nothing on the DS would show (OAM entries the
   game should have hidden). Logged: how often the staging buffers are reset
   and loaded, the first sprite-template lists the game hands over, and the
   buffers' state in each display report. */
extern "C" {
void __real__ZN3OAM5ResetEv(void);
void __real__ZN3OAM4LoadEv(void);
void __real__ZN3OAM6RenderEbP7OamAttriiiiiiii(bool, void *, int, int, int, int, int, int, int, int);
void __real__ZN3OAM6RenderEbP7OamAttriiiiP9Matrix2x2(bool, void *, int, int, int, int, void *);
}
static unsigned g_oam_resets, g_oam_loads, g_oam_renders, g_oam_renders7;
extern "C" void __wrap__ZN3OAM5ResetEv(void)
{
    if (++g_oam_resets <= 3) {
        printf("  OAM::Reset #%u (single-buffer %u)\n", g_oam_resets, (unsigned)data_0209e660);
        fflush(stdout);
    }
    __real__ZN3OAM5ResetEv();
}
extern "C" void __wrap__ZN3OAM4LoadEv(void)
{
    ++g_oam_loads;
    __real__ZN3OAM4LoadEv();
}
static void log_template(const char *what, unsigned n, bool sub, const void *attr, int x, int y)
{
    printf("  %s #%u: %s screen, list %p at (%d,%d):", what, n, sub ? "sub" : "main", attr, x, y);
    const uint8_t *b = static_cast<const uint8_t *>(::ntr::host_ptr((uintptr_t)attr));
    for (int i = 0; i < 16; ++i) printf(" %02x", b[i]);
    printf("\n");
    fflush(stdout);
}
extern "C" void __wrap__ZN3OAM6RenderEbP7OamAttriiiiiiii(bool sub, void *attr, int x, int y, int pal,
                                                      int prio, int sx, int sy, int rot, int mode)
{
    if (++g_oam_renders <= 40) log_template("OAM::Render", g_oam_renders, sub, attr, x, y);
    __real__ZN3OAM6RenderEbP7OamAttriiiiiiii(sub, attr, x, y, pal, prio, sx, sy, rot, mode);
}
extern "C" void __wrap__ZN3OAM6RenderEbP7OamAttriiiiP9Matrix2x2(bool sub, void *attr, int x, int y,
                                                             int pal, int prio, void *m)
{
    if (++g_oam_renders7 <= 40) log_template("OAM::Render(1 object)", g_oam_renders7, sub, attr, x, y);
    __real__ZN3OAM6RenderEbP7OamAttriiiiP9Matrix2x2(sub, attr, x, y, pal, prio, m);
}
static void oam_report()
{
    printf("    OAM lists: %u resets, %u loads, %u+%u renders; single %u, counts main %d sub %d\n",
           g_oam_resets, g_oam_loads, g_oam_renders, g_oam_renders7, (unsigned)data_0209e660,
           data_0209e664, data_0209e670);
    for (int k = 0; k < 2; ++k) {
        const uint8_t *b = k ? data_0209ea74 : data_0209e674;
        printf("      %s:", k ? "sub " : "main");
        static const int kShow[] = {0, 1, 2, 8, 9, 64, 127};
        for (int e : kShow) {
            uint16_t a[4];
            memcpy(a, b + e * 8, 8);
            printf(" [%d] %04x %04x %04x", e, a[0], a[1], a[2]);
        }
        printf("\n");
    }
    fflush(stdout);
}

/* --- Input, followed from the Vita to the title screen: what the host
   wrote (KEYINPUT, the ARM7's X/Y word, the touch sample), what the game's
   poll made of it (its input record, player 0's copy, the touch-panel ring
   index), and what the title saw (its pad, its menu states). Logged when the
   Vita's input changes, and every two seconds. */
static void input_trace(uint64_t frame)
{
    static uint32_t last_sig = ~0u;
    static unsigned lines;
    const uint16_t keys = *static_cast<volatile uint16_t *>(ntr::host_ptr(0x04000130));
    const uint16_t xy = *static_cast<volatile uint16_t *>(ntr::host_ptr(0x027fffa8));
    const uint16_t t0 = *static_cast<volatile uint16_t *>(ntr::host_ptr(0x027fffaa));
    const uint16_t t1 = *static_cast<volatile uint16_t *>(ntr::host_ptr(0x027fffac));
    const uint32_t sig = (uint32_t)keys ^ ((uint32_t)xy << 16) ^ ((uint32_t)(t1 >> 8) << 30);
    const bool changed = sig != last_sig;
    last_sig = sig;
    if (!(changed && lines < 400) && frame % 120 != 0) return;
    ++lines;
    auto h16 = [](const uint8_t *b, int o) { uint16_t v; memcpy(&v, b + o, 2); return (unsigned)v; };
    printf("  input @%u: host keys %04x xy %04x touch %04x %04x | record keys %04x touch %u,%u t%u v%u"
           " | player0 keys %04x touch %u,%u t%u | tp index %u flags %04x state %u",
           (unsigned)frame, keys, xy, t0, t1, h16(data_020a1040, 0x0e), h16(data_020a1040, 4),
           h16(data_020a1040, 6), h16(data_020a1040, 8), h16(data_020a1040, 10),
           h16(data_020a1154, 0x0e), h16(data_020a1154, 4), h16(data_020a1154, 6),
           h16(data_020a1154, 8), h16(data_020a80cc, 0x0c), h16(data_020a80cc, 0x36),
           h16(data_020a80cc, 0x32));
    const uint8_t *w = data_ov007_0210342c;
    if (w) {
        int choice, ready;
        memcpy(&choice, w + 0x10, 4);
        memcpy(&ready, w + 0x14, 4);
        uint8_t *pad;
        memcpy(&pad, w + 0x54, 4);
        printf(" | title choice %d ready %d", choice, ready);
        if (pad) printf(" pad %04x lock %u", h16(pad, 0), h16(pad, 0x64));
        printf(" states");
        for (int k = 0; k < 3; ++k) {
            uint8_t *st;
            memcpy(&st, w + 4 + 4 * k, 4);
            if (st) printf(" %d(%u)", (int)(int16_t)h16(st, 0), h16(st, 12) | h16(st, 14) << 16);  // state (ticks)
        }
    }
    printf("\n");
    fflush(stdout);
}

/* --- Model files as they reach dExtSimpleModel_c::LoadTexAndPal (the
   player's spawn faulted inside it): the header's counts and table pointers
   and the first textures and palettes, for the first calls. */
extern "C" void __real__ZN17dExtSimpleModel_c13LoadTexAndPalER8BMD_File(void *file);
extern "C" void __wrap__ZN17dExtSimpleModel_c13LoadTexAndPalER8BMD_File(void *file)
{
    static unsigned n;
    if (n++ < 40) {
        uint32_t h[14];
        memcpy(h, file, sizeof h);
        printf("  model %p: textures %u at %08x, palettes %u at %08x, materials %u at %08x\n", file,
               (unsigned)h[5], (unsigned)h[6], (unsigned)h[7], (unsigned)h[8], (unsigned)h[9],
               (unsigned)h[10]);
        for (unsigned i = 0; i < h[5] && i < 3; ++i) {
            uint32_t t[5];
            memcpy(t, (const char *)(uintptr_t)h[6] + i * 20, sizeof t);
            printf("    tex %u: name %08x data %08x size %u flags %08x\n", i, (unsigned)t[0],
                   (unsigned)t[1], (unsigned)t[2], (unsigned)t[4]);
        }
        for (unsigned i = 0; i < h[7] && i < 2; ++i) {
            uint32_t t[4];
            memcpy(t, (const char *)(uintptr_t)h[8] + i * 16, sizeof t);
            printf("    pal %u: name %08x data %08x size %u\n", i, (unsigned)t[0], (unsigned)t[1],
                   (unsigned)t[2]);
        }
        fflush(stdout);
    }
    __real__ZN17dExtSimpleModel_c13LoadTexAndPalER8BMD_File(file);
}

static uint64_t g_capture_at;      /* a rear-panel capture: its snapshot frame */

/* Hold the game to the DS's 60 frames a second. With the drawing on its
   own core the frame loop can now run faster than that, and nothing else
   paces it: the game ran at 70-80 fps, unevenly. Each frame waits for its
   slot on a 1/60 s clock; a frame that falls more than a few behind resets
   the clock rather than racing to catch up. */
extern "C" int sm64ds_game_speed_pct(void);   /* vita_display.cpp: the settings menu */

static void pace_frame()
{
    static double next;
    const double t = now_ms();
    const int pct = sm64ds_game_speed_pct();
    if (next == 0 || t - next > 100 + 100000.0 / 60.0 / pct) next = t;
    next += 1000.0 / 60.0 * 100.0 / pct;
    if (next > t) usleep((useconds_t)((next - t) * 1000));
}

extern "C" void sm64ds_model_trace_frame(bool arm);   /* model_trace_host.cpp */
extern "C" const char *sm64ds_glitch_capture_requested(void);   /* model_trace_host.cpp */
extern "C" bool sm64ds_rear_capture_enabled(void);             /* vita_display.cpp */
extern "C" void sm64ds_savestate_frame(void);                  /* savestate_host.cpp */
extern "C" void sm64ds_camera_ring_dump(void);                 /* savestate_host.cpp */

/* The rolling 3D record (gx_ring_enable): 12 MB, about one and a half to two
   seconds of play at full speed (much more when slowed down), always kept
   while rear-tap captures are on, so a tap just after a glitch still holds
   the frames that showed it. Each frame is tagged with its number. */
static void ring_frame(uint64_t frame)
{
    const bool want = sm64ds_rear_capture_enabled();
    if (want != ntr::gx_ring_active()) {
        ntr::gx_ring_enable(want ? 3u << 20 : 0);
        printf("  rolling 3D record %s\n", ntr::gx_ring_active() ? "on (12 MB)" : want ? "FAILED (no memory)" : "off");
        fflush(stdout);
    }
    ntr::gx_ring_mark(0x4652u << 16 | (uint32_t)(frame & 0xffff));    /* "FR" + frame */
}

extern "C" void sm64ds_card_stats(uint32_t *reads, uint32_t *bytes, uint32_t *disk, uint64_t *us);
extern "C" void sm64ds_romfs_stats(unsigned *reads, unsigned *bytes, unsigned long long *us);
extern "C" void sm64ds_romimage_stats(unsigned *loads, unsigned long long *us);
extern unsigned long long sm64ds_fs_archived_us;     /* port/hal/fs.cpp */
extern unsigned sm64ds_fs_archived_n;

/* Loading time, for the log: a game frame that took long (a load) says how
   long and how much of it went to reading the game card; the first frame
   says how long the boot took. */
static void load_timing(uint64_t frame)
{
    static double t_prev;
    static uint32_t r0, b0, d0;
    static uint64_t u0;
    const double t = now_ms();
    uint32_t r, b, d;
    uint64_t u;
    sm64ds_card_stats(&r, &b, &d, &u);
#if defined(__vita__)
    const double since_start = sceKernelGetProcessTimeWide() / 1000.0;
#else
    const double since_start = 0;
#endif
    if (frame == 1)
        printf("  [load] boot to the first frame: %.0f ms since the start; card reads %u (%u KB, %u from the memory card), %.0f ms of reading\n",
               since_start, (unsigned)r, (unsigned)(b / 1024), (unsigned)d, u / 1000.0);
    // the game's files (read whole by the port) and the overlays loaded
    static unsigned fr0, fb0, ml0, an0;
    static unsigned long long fu0, mu0, au0;
    unsigned fr, fb, ml;
    unsigned long long fu, mu;
    sm64ds_romfs_stats(&fr, &fb, &fu);
    sm64ds_romimage_stats(&ml, &mu);
    if (frame != 1 && t_prev > 0 && t - t_prev > 250)
        printf("  [load] frame %u took %.0f ms: card reads %u (%u KB, %u from the memory card), %.0f ms of reading;"
               " game files %u (%u KB) %.0f ms; from archives %u, %.0f ms; overlays %u, %.0f ms\n",
               (unsigned)frame, t - t_prev, (unsigned)(r - r0), (unsigned)((b - b0) / 1024),
               (unsigned)(d - d0), (u - u0) / 1000.0, fr - fr0, (fb - fb0) / 1024, (fu - fu0) / 1000.0,
               sm64ds_fs_archived_n - an0, (sm64ds_fs_archived_us - au0) / 1000.0, ml - ml0, (mu - mu0) / 1000.0);
    t_prev = now_ms();
    r0 = r; b0 = b; d0 = d; u0 = u;
    fr0 = fr; fb0 = fb; fu0 = fu; ml0 = ml; mu0 = mu;
    an0 = sm64ds_fs_archived_n; au0 = sm64ds_fs_archived_us;
}

static bool hook(uint64_t frame)
{
    load_timing(frame);
    pace_frame();
    sm64ds_model_trace_frame(false);
    ring_frame(frame);
    sm64ds_savestate_frame();       /* a save state asked for: done here, between frames */
    sm64ds_display_tick();
    sm64ds_input_frame();
    sm64ds_arm7_frame();
    input_trace(frame);
    const int step = data_0209d50c, count = data_020a0db0;
    if (count != g_last_count) ++g_loop_frames;
    if (step != g_last_step || frame % 60 == 0 || frame < 5) {
        printf("  frame %3u: main-loop step %d, loop frame %d\n", (unsigned)frame, step, count);
        fflush(stdout);
    }
    g_last_step = step;
    g_last_count = count;
    if (frame % 300 == 0 || frame == 130) {
        display_report(frame);
        oam_report();
    }
    g_host_phase = "drawing the live view (2D scanout, 3D render)";
    {
        static double t_prev;
        const double t0 = now_ms();
        if (t_prev > 0) g_ms_frame += t0 - t_prev;
        t_prev = t0;
        // the live view: every other frame (30 a second); slowed down,
        // every frame, so none goes by unseen
        if (frame % 2 == 0 || sm64ds_game_speed_pct() < 100) {
            sm64ds_display_present();
            g_ms_present += now_ms() - t0;
            ++g_n_present;
        }
    }
    g_host_phase = "game's turn";

    if (frame == WINDOW) finish(frame);
    // A rear-panel tap: the screens now, and a snapshot three frames later
    // holding the 3D commands of those frames (replayed from a fresh start).
    // Nothing is saved on a timer any more: each save stops the game while
    // the memory card is written.
    const char *auto_why = g_capture_at ? nullptr : sm64ds_glitch_capture_requested();
    if ((sm64ds_input_capture_requested() || auto_why) && !g_capture_at) {
        g_host_phase = "saving the screenshots";
        capture(frame);
        printf("  capture at frame %u (%s)\n", (unsigned)frame, auto_why ? auto_why : "rear panel");
        if (!auto_why) sm64ds_camera_ring_dump();
        fflush(stdout);
        if (!ntr::gx_ring_active()) ntr::gx_record_start(1u << 20);
        g_capture_at = frame + 3;
        sm64ds_model_trace_frame(true);
    }
    if (g_capture_at && frame == g_capture_at) {
        g_host_phase = "writing the display snapshot";
        ntr::gx_record_stop();
        write_snapshot(frame);
        g_capture_at = 0;
    }
    return true;
}

/* crt0's order (src/_start.c), on the frame loop's game thread -- the DS
   regions are mapped by then, and OS_Init's thread setup adopts this thread as
   the launcher: NitroStartUp (OS_Init, the root heap), the ARM9's static
   initializers, then NitroMain. */
static void boot(void)
{
    sm64ds_irq_host_init();
    sm64ds_card_boot_setup();       /* the shared area as the DS's loader leaves it */
    sm64ds_input_init();            /* keys released, lid open, touch calibration */
    printf("  NitroStartUp\n");
    fflush(stdout);
    NitroStartUp();
    printf("  ARM9 static initializers...\n");
    fflush(stdout);
    sm64ds_romimage_run_inits(-1);
    printf("  NitroMain\n");
    fflush(stdout);
    NitroMain();
}

/* An exception nothing catches ends the program through std::terminate,
   which says nothing in the log: say what it was, and how much of the C heap
   was in use, first. */
static void on_terminate()
{
    const char *what = "an unknown exception";
    static char buf[160];
    if (std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::bad_alloc &) {
            what = "std::bad_alloc (out of memory)";
        } catch (const std::exception &x) {
            snprintf(buf, sizeof buf, "std::exception: %s", x.what());
            what = buf;
        } catch (...) {
        }
    } else {
        what = "no exception (terminate called directly)";
    }
    const struct mallinfo mi = mallinfo();
    printf("smoke_boot: std::terminate: %s; C heap in use %u KiB of %u KiB\n", what,
           (unsigned)(mi.uordblks / 1024), (unsigned)(mi.arena / 1024));
    fflush(stdout);
    if (sm64ds_log_flush) sm64ds_log_flush();
    abort();
}

/* A C++ cleanup (destructors run while an exception unwinds) ending: no
   exception should ever unwind in this game, so reaching one means control
   arrived there some other way. Says where, against main's address (both
   are in gate11.syms), before the runtime gives up. */
int main(void);
extern "C" void __real___cxa_end_cleanup(void);
extern "C" void __wrap___cxa_end_cleanup(void)
{
    printf("smoke_boot: a C++ cleanup ended at %p (main at %p)\n", __builtin_return_address(0),
           (void *)&main);
    fflush(stdout);
    __real___cxa_end_cleanup();
}

int main(void)
{
    std::set_terminate(on_terminate);
    if (!sm64ds_romimage_load(SM64DS_ROM_PATH)) {
        fprintf(stderr, "smoke_boot: ROM data did not load\n");
        return 1;
    }
#if defined(__vita__)
    // the Vita's top clocks (CPU 444 MHz, bus 222, GPU 222, crossbar 166),
    // as homebrew ports run at; the default leaves the CPU at 333 MHz
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    printf("  clocks: CPU %d MHz\n", scePowerGetArmClockFrequency());
#endif
    sm64ds_thread_started();        /* the frame loop: the first core */
    if (sm64ds_display_init()) printf("  live view on the Vita's screen\n");
    start_watchdog();
#if defined(__vita__)
    const uint64_t frames = ntr::rt_run(boot, hook, 0);     /* until it is closed */
#else
    const uint64_t frames = ntr::rt_run(boot, hook, WINDOW);
#endif
    printf("  frames run: %u, main-loop frames seen: %d\n", (unsigned)frames, g_loop_frames);
    if (frames < WINDOW || g_loop_frames < 10) {
        fprintf(stderr, "smoke_boot: the game did not keep running (%u frames, %d loop frames)\n",
                (unsigned)frames, g_loop_frames);
        return 1;
    }
    printf("smoke_boot: the game booted and its main loop ran for %u frames\n", (unsigned)frames);
    return 0;
}

/* The window is over: report. Off the Vita the test ends here (the DS
   threads cannot be unwound -- each waits for the CPU on its own host
   thread -- so it ends the process rather than returning through rt_run). On
   the Vita the game keeps running, to be played, until it is closed. */
static void finish(uint64_t frames)
{
    const bool ok = g_loop_frames >= 10;
    printf("  frames run: %u, main-loop frames seen: %d\n", (unsigned)frames, g_loop_frames);
    if (ok)
        printf("smoke_boot: the game booted and its main loop ran for %u frames\n", (unsigned)frames);
    else
        fprintf(stderr, "smoke_boot: the game did not keep running (%d loop frames)\n", g_loop_frames);
    printf("== gate11 finished: %s ==\n", ok ? "PASS" : "FAIL");
    if (FILE *r = fopen(SM64DS_OUT_DIR "/results.txt", "a")) {
        fprintf(r, "gate11: %s (exit %d)\n", ok ? "PASS" : "FAIL", ok ? 0 : 1);
        fclose(r);
    }
#if defined(__vita__)
    printf("  still running: play with the Vita's buttons and the touch screen; "
           "tap the rear panel to save the screens; close it from the PS button\n");
    fflush(stdout);
    fflush(stderr);
#else
    quit(ok ? 0 : 1);
#endif
}

/* port/vita/boot_trace.S: each wrapped start-up step announces itself; the
   last few are kept for the watchdog. A step repeating is logged its first 20
   times (OS_SleepThread, file reads), then counted. */
static const char *g_steps[16];
static unsigned g_nsteps;
extern "C" void sm64ds_boot_trace(const char *step)
{
    // Each step name is logged its first 30 times, then only counted: the log
    // is written straight to the memory card, and the boot's asset table
    // takes thousands of file lookups.
    struct Count { const char *name; unsigned n; };
    static Count counts[96];
    static unsigned ncounts;
    g_steps[g_nsteps++ % 16] = step;
    Count *c = nullptr;
    for (unsigned i = 0; i < ncounts; ++i)
        if (counts[i].name == step) { c = &counts[i]; break; }
    if (!c && ncounts < 96) { c = &counts[ncounts++]; c->name = step; c->n = 0; }
    const unsigned n = c ? ++c->n : 0;
    if (n > 30) return;
    if (n == 30) printf("  -> %s (30th time: no longer logged)\n", step);
    else printf("  -> %s\n", step);
    fflush(stdout);
}

/* --- What the game is doing when it stops: the DS threads (OSThread: state
   0x64, next 0x68, id 0x6c, priority 0x70), the interrupt state, the last
   steps. The addresses are host addresses; NitroMain's own is printed with
   them so a map of the ELF names them. */
static void note_result(const char *what)
{
    if (FILE *r = fopen(SM64DS_OUT_DIR "/results.txt", "a")) {
        fprintf(r, "gate11: FAIL, %s\n", what);
        fclose(r);
    }
}

static void dump_state(const char *why)
{
    printf("== %s ==\n", why);
    printf("  frame %u, main-loop step %d, loop frame %d; NitroMain at %p\n",
           (unsigned)ntr::rt_frame(), data_0209d50c, data_020a0db0, (void *)NitroMain);
    int turn = -1, stage = -1;
    ntr::rt_debug_state(&turn, &stage);
    static const char *const stages[] = {"waiting for its turn", "running scanlines", "in the frame hook"};
    printf("  frame loop: %s; stage: %s; handoff turn: %s\n", g_host_phase,
           stage >= 0 && stage < 3 ? stages[stage] : "?", turn == 0 ? "host" : turn == 1 ? "game" : "?");
    printf("  IME %u IE %08x IF %08x, CPU interrupts %s\n",
           (unsigned)NTR_MMIO(uint16_t, 0x04000208), (unsigned)NTR_MMIO(uint32_t, 0x04000210),
           (unsigned)NTR_MMIO(uint32_t, 0x04000214), ntr::rt_irq_masked() ? "masked" : "on");
    printf("  DS threads (current %p, irqDepth %u, holding the CPU %p):\n",
           (void *)data_020a6134.current, data_020a6134.depth, sm64ds_thread_running());
    static const char *const states[] = {"waiting", "ready", "terminated"};
    int n = 0;
    for (uint8_t *t = data_020a6134.list; t && n < 32; ++n) {
        int32_t state;
        uint32_t id, prio;
        uint8_t *next;
        memcpy(&state, t + 0x64, 4);
        memcpy(&next, t + 0x68, 4);
        memcpy(&id, t + 0x6c, 4);
        memcpy(&prio, t + 0x70, 4);
        printf("    thread %u %p: %s, priority %u, entry 0x%08x\n", (unsigned)id, (void *)t,
               state >= 0 && state < 3 ? states[state] : "?", (unsigned)prio,
               (unsigned)sm64ds_thread_entry(t));
        t = next;
    }
    sm64ds_hostlog_dump();
    printf("  last steps:");
    for (unsigned i = g_nsteps > 16 ? g_nsteps - 16 : 0; i < g_nsteps; ++i) printf(" %s", g_steps[i % 16]);
    printf("\n");
    fflush(stdout);
}

/* The game's own Crash(): it stops the DS for good. Say who called it. */
extern "C" void __wrap_Crash(void)
{
    char why[96];
    snprintf(why, sizeof why, "the game called Crash() from %p", __builtin_return_address(0));
    dump_state(why);
    {
        // who called the caller: the game's return addresses on the stack
        // (Thumb, odd, inside its code), nearest first, for gate11.syms
        // the module's code: from NitroMain's page down 1 MB, up 3 MB
        const uintptr_t at = (uintptr_t)&NitroMain;
        const uintptr_t lo = (at & ~0xfffffu) > 0x100000 ? (at & ~0xfffffu) - 0x100000 : 0, hi = lo + 0x400000;
        uintptr_t *sp = (uintptr_t *)__builtin_frame_address(0);
        printf("  code on the stack (NitroMain %p):", (void *)at);
        int n = 0;
        for (int i = 0; i < 1024 && n < 24; ++i)
            if ((sp[i] & 1) && sp[i] > lo && sp[i] < hi) {
                printf(" %p", (void *)sp[i]);
                ++n;
            }
        printf("\n");
        fflush(stdout);
    }
    fprintf(stderr, "smoke_boot: %s\n", why);
    fflush(stderr);
    note_result("Crash()");
    quit(3);
}

/* No new frame for 15 s: the game is stuck. Log where, and end the test. */
static void *watchdog(void *)
{
    uint64_t last = ~0ull;
    int still = 0;
    // A probe for slow loads: with a folder ux0:data/sm64ds/probe present,
    // the first frame that takes 3 s ends the game the same way, so the
    // crash dump shows what the game's thread is busy with. (Rename or
    // delete the folder to play normally.)
    struct stat st;
    const bool probe = stat("ux0:data/sm64ds/probe", &st) == 0;
    if (probe) printf("  watchdog: probe folder present, a 3 s frame ends the game with a crash dump\n");
    for (;;) {
        usleep(1000000);
        const uint64_t f = ntr::rt_frame();
        still = (f == last && !g_host_io) ? still + 1 : 0;
        last = f;
        if (still == 15 || (probe && still == 3)) {
            dump_state(still == 3 ? "PROBE: a frame has taken 3 s" : "WATCHDOG: no frame for 15 s");
            fprintf(stderr, "smoke_boot: the game stopped (watchdog)\n");
            fflush(stderr);
            note_result("watchdog");
            // End with a deliberate fault rather than a plain exit: the Vita
            // then writes a core dump (ux0:data/*.psp2dmp) holding every
            // thread's registers and stack -- where the stuck one is.
            printf("  (ending with a deliberate fault so the Vita saves a crash dump)\n");
            fflush(stdout);
            if (sm64ds_log_flush) sm64ds_log_flush();
            __builtin_trap();
        }
    }
    return nullptr;
}
static void start_watchdog()
{
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    pthread_create(&th, &attr, watchdog, nullptr);
    pthread_attr_destroy(&attr);
}
