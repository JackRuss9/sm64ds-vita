// A live view of both DS screens on the PS Vita's display (gate 11 on).
//
// Two ways to draw it. With vitaGL built (port/vita/deps.sh) the 3D picture
// is drawn on the GPU and both screens are shown through it
// (port/vita/vita_gl3d.cpp); otherwise -- or with ux0:data/sm64ds/cpu3d.txt
// present -- the CPU draws everything into a plain framebuffer, as before.
//
// Layout: the two screens side by side, 480x360 each (1.875x): the top
// screen on the left, the touch screen on the right (which engine shows on
// which is POWCNT1's choice).
//
// The drawing runs on its own core. At a present the frame loop captures the
// 3D frame (its triangle list and the decoded textures it points at) and
// copies what the 2D engines read -- I/O registers, palettes, OAM and the
// VRAM the 2D views and extended palettes use, about 1.6 MB -- then hands
// the copy to a drawing thread on the Vita's second core, which composes both
// screens, scales them into the back framebuffer and shows it. The game runs
// on meanwhile. If that thread is still busy with the last frame, this frame
// is not drawn (the game is not held up for the live view).
// Off the Vita (QEMU tests) every call is a no-op.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <new>

#include "ntr/gx.h"
#include "ntr/mmio.h"
#include "ntr/ppu.h"

#if defined(__vita__)
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#endif

#if defined(__vita__) && defined(SM64DS_VITAGL)
#include "vita_gl3d.h"       // the 3D and the presentation on the GPU
#endif

/* The cheats (the cheats menu below; applied by port/vita/input_host.cpp). */
extern "C" {
/* The 3D is drawn 16:9 (the WIDESCREEN layout): the game culls for that
   width (src/engine/collision/d_clipper.cpp, input_host.cpp). */
volatile int sm64ds_wide_view;
volatile int sm64ds_cheat_infinite_lives, sm64ds_cheat_moon_jump, sm64ds_cheat_infinite_power,
    sm64ds_cheat_infinite_breath, sm64ds_cheat_99_coins, sm64ds_cheat_god_mode;
}

namespace {
constexpr int VW = 960, VH = 544, SW = 480, SH = 360, Y0 = (VH - SH) / 2;
constexpr uint32_t FB_BYTES = 2u * 1024 * 1024;     // one 960x544 A8B8G8R8 buffer
uint32_t *g_fbs[2];                     // shown / being drawn
uint32_t *g_fb;                         // the one being drawn
int g_back;
int g_sx[SW], g_sy[SH];                 // display pixel -> DS pixel

// The copy of the display state the drawing thread works from: each DS
// region at its own offsets (only the parts the 2D engines read are copied).
struct Copy {
    uint8_t *region[8];
    ntr::GxFrame frame3d;               // the 3D frame to draw: triangles, textures, registers
    ntr::Framebuffer g3d;               // drawn from it on the drawing thread
    bool has3d;
};
Copy g_copy;
// Screen layouts (the GPU view; the CPU one stays side by side): where the
// top and the touch screen go on the 960x544 display, {x, y, w, h}; a zero
// width hides one. Chosen in the settings menu (below).
struct Layout { const char *name; int top[4], bottom[4]; bool wide; };
constexpr Layout kLayouts[] = {
    {"SIDE BY SIDE", {0, 92, 480, 360}, {480, 92, 480, 360}},
    {"BIG TOP SCREEN", {0, 0, 725, 544}, {725, 0, 235, 176}},
    {"BIG TOUCH SCREEN", {235, 0, 725, 544}, {0, 0, 235, 176}},
    {"TOP SCREEN ONLY", {117, 0, 725, 544}, {0, 0, 0, 0}},
    // the top screen 16:9: the 3D picture wider (vita_gl3d.cpp), the 2D in
    // the middle at its own proportions
    {"WIDESCREEN 16:9", {0, 0, 960, 544}, {765, 398, 195, 146}, true},
};
constexpr int NLAYOUTS = sizeof kLayouts / sizeof kLayouts[0];
volatile int g_layout;
// The 3D picture's resolution on the GPU view, 1..4 times the DS's.
volatile int g_scale = 2;
volatile int g_show_fps = 1;            // the frame-rate line
volatile int g_rear_capture = 1;        // a rear-panel tap takes a capture
// The game's speed, to watch what goes by too fast (a glitch for a frame or
// two): 100, 50, 25, 10 or 5 percent of the DS's 60 frames a second.
volatile int g_speed_idx;
constexpr int kSpeeds[] = {100, 50, 25, 10, 5};
volatile int g_right_stick_camera = 1;  // the right stick turns the camera
unsigned g_dropped;                     // presents skipped: the thread was busy
bool g_ready;                           // the live view is up
bool g_gl;                              // drawn on the GPU (vitaGL), not the CPU

// --- the frame-rate line, top left: the game's frames per second (DS
// frames: 60 is full speed) and how many of them reached the screen ---
volatile float g_game_fps, g_shown_fps;
unsigned g_shown;                       // frames shown, counted by the drawing thread

// a 5x7 font: digits, capitals and a little punctuation
struct Glyph { char c; uint8_t rows[7]; };
constexpr Glyph kFont[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}}, {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}}, {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}}, {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}}, {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}}, {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}}, {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}}, {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    // the rest, for the settings menu
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}}, {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}}, {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}}, {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}}, {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}}, {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}}, {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'<', {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}}, {'>', {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}},
    {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
};

/* Text at (x0, y0) of a buffer `stride` pixels wide, each font pixel 2x2,
   white on black. */
void draw_text(uint32_t *fb, int x0, int y0, const char *text, int stride = VW)
{
    constexpr int SCALE = 2, ADV = 6 * SCALE;
    int x = x0;
    for (const char *p = text; *p; ++p, x += ADV) {
        for (int yy = 0; yy < 7 * SCALE; ++yy)
            for (int xx = 0; xx < ADV; ++xx) fb[(y0 + yy) * stride + x + xx] = 0xff000000u;
        const Glyph *g = nullptr;
        for (const Glyph &k : kFont)
            if (k.c == *p) g = &k;
        if (!g) continue;
        for (int r = 0; r < 7; ++r)
            for (int c = 0; c < 5; ++c)
                if (g->rows[r] & (0x10 >> c))
                    for (int yy = 0; yy < SCALE; ++yy)
                        for (int xx = 0; xx < SCALE; ++xx)
                            fb[(y0 + r * SCALE + yy) * stride + x + c * SCALE + xx] = 0xffffffffu;
    }
}

void fps_line(uint32_t *fb, int x0 = 12, int y0 = 12, int stride = VW)
{
    char line[64];
    const int g10 = (int)(g_game_fps * 10 + 0.5f), s10 = (int)(g_shown_fps * 10 + 0.5f);
    // one decimal, without printf's float formatting
    snprintf(line, sizeof line, "GAME %d.%d FPS   SCREEN %d.%d FPS", g10 / 10, g10 % 10, s10 / 10, s10 % 10);
    if (kSpeeds[g_speed_idx] != 100) {      // slowed down on purpose: say so
        const size_t n = strlen(line);
        snprintf(line + n, sizeof line - n, "   SPEED %d PERCENT", kSpeeds[g_speed_idx]);
    }
    draw_text(fb, x0, y0, line, stride);
}

double seconds();
/* A short message on the text line ("STATE SAVED"), for two seconds. */
char g_toast[48];
double g_toast_until;
void toast_line(uint32_t *fb, int x0, int y0, int stride, bool below_fps)
{
    if (!g_toast[0] || seconds() > g_toast_until) return;
    draw_text(fb, x0 + (below_fps ? 560 : 0), y0, g_toast, stride);
}

double seconds()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

inline uint32_t to_abgr(uint32_t argb)
{
    return 0xff000000u | (argb & 0x0000ff00u) | ((argb >> 16) & 0xffu) | ((argb & 0xffu) << 16);
}

void blit(const ntr::Framebuffer &src, int x0)
{
    for (int y = 0; y < SH; ++y) {
        uint32_t *dst = g_fb + (Y0 + y) * VW + x0;
        const uint32_t *row = src.px[g_sy[y]];
        for (int x = 0; x < SW; ++x) dst[x] = to_abgr(row[g_sx[x]]);
    }
}

#if defined(__vita__)
SceUID g_go, g_done;                    // semaphores: copy ready / drawing finished

void show(uint32_t *buf)
{
    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof fb);
    fb.size = sizeof fb;
    fb.base = buf;
    fb.pitch = VW;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = VW;
    fb.height = VH;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

/* The drawing thread: compose both screens from the copy, scale them into
   the back buffer, show it. */
// --- two menus, laid out and opened as in the Super Mario Sunshine Vita
// port: settings (D-pad Right + Square) and cheats (D-pad Left + Circle).
// Circle closes either; Up/Down choose, Left/Right or Cross change. While
// one is open the game gets no input. The choices of both are kept in
// ux0:data/sm64ds/settings.txt. ---
const char *const kSettingsFile = "ux0:data/sm64ds/settings.txt";
struct MenuItem {
    const char *label, *key;        // what the menu shows, the settings file's name
    volatile int *value;
    int lo, hi;
    int kind;                       // 0 off/on, 1 layout, 2 resolution, 3 shown/hidden
};
const MenuItem kSettingsItems[] = {
    {"SCREEN LAYOUT", "layout", &g_layout, 0, NLAYOUTS - 1, 1},
    {"3D RESOLUTION", "resolution", &g_scale, 0, 4, 2},
    {"FRAME RATE", "show_fps", &g_show_fps, 0, 1, 3},
    {"REAR TAP CAPTURES", "rear_capture", &g_rear_capture, 0, 1, 0},
    {"GAME SPEED", "game_speed", &g_speed_idx, 0, 4, 4},
    {"RIGHT STICK CAMERA", "right_stick_camera", &g_right_stick_camera, 0, 1, 0},
};
const MenuItem kCheatItems[] = {
    {"INFINITE LIVES", "cheat_lives", &sm64ds_cheat_infinite_lives, 0, 1, 0},
    {"MOON JUMP (HOLD JUMP)", "cheat_moon_jump", &sm64ds_cheat_moon_jump, 0, 1, 0},
    {"INFINITE POWER", "cheat_power", &sm64ds_cheat_infinite_power, 0, 1, 0},
    {"INFINITE BREATH UNDERWATER", "cheat_breath", &sm64ds_cheat_infinite_breath, 0, 1, 0},
    {"99 COINS", "cheat_99_coins", &sm64ds_cheat_99_coins, 0, 1, 0},
    {"GOD MODE (NO DAMAGE)", "cheat_god_mode", &sm64ds_cheat_god_mode, 0, 1, 0},
};
struct Menu {
    const char *title;
    const MenuItem *items;
    int count;
    int sel;
};
Menu g_menus[] = {
    {"SETTINGS", kSettingsItems, int(sizeof kSettingsItems / sizeof kSettingsItems[0]), 0},
    {"CHEATS", kCheatItems, int(sizeof kCheatItems / sizeof kCheatItems[0]), 0},
};
volatile int g_menu_open = -1;          // the menu shown, -1 none
volatile unsigned g_menu_gen = 1;       // bumped whenever what a menu shows changes

const char *menu_value(const MenuItem &m, char *buf, size_t n)
{
    const int v = *m.value;
    switch (m.kind) {
    case 1: return kLayouts[v].name;
    case 2:
        // 0: the size the top screen has on the display in this layout
        if (v == 0) snprintf(buf, n, "SCREEN SIZE (%dX%d)", kLayouts[g_layout].top[2], kLayouts[g_layout].top[3]);
        else snprintf(buf, n, "%dX (%dX%d)", v, 256 * v, 192 * v);
        return buf;
    case 3: return v ? "SHOWN" : "HIDDEN";
    case 4: snprintf(buf, n, "%d PERCENT", kSpeeds[v]); return buf;
    default: return v ? "ON" : "OFF";
    }
}

void settings_save()
{
    if (FILE *f = fopen(kSettingsFile, "w")) {
        for (const Menu &mn : g_menus)
            for (int i = 0; i < mn.count; ++i) fprintf(f, "%s=%d\n", mn.items[i].key, *mn.items[i].value);
        fclose(f);
    }
}

void settings_load()
{
    FILE *f = fopen(kSettingsFile, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const int v = atoi(eq + 1);
        for (const Menu &mn : g_menus)
            for (int i = 0; i < mn.count; ++i) {
                const MenuItem &m = mn.items[i];
                if (!strcmp(line, m.key) && v >= m.lo && v <= m.hi) *m.value = v;
            }
    }
    fclose(f);
    printf("  settings: layout %d (%s), 3D %d, game speed %d%%, cheats: lives %d moon jump %d power %d breath %d"
           " coins %d god mode %d\n", g_layout, kLayouts[g_layout].name, g_scale, kSpeeds[g_speed_idx],
           sm64ds_cheat_infinite_lives, sm64ds_cheat_moon_jump, sm64ds_cheat_infinite_power,
           sm64ds_cheat_infinite_breath, sm64ds_cheat_99_coins, sm64ds_cheat_god_mode);
}

// the open menu's picture: drawn again only when what it shows changes
constexpr int MENU_W = 576, MENU_H = 288;   // 46 characters a line, 9 rows
uint32_t g_menu_px[MENU_W * MENU_H];
unsigned g_menu_drawn;              // the g_menu_gen drawn into g_menu_px

bool render_menu()
{
    const int open = g_menu_open;
    if (open < 0 || g_menu_drawn == g_menu_gen) return false;
    g_menu_drawn = g_menu_gen;
    const Menu &mn = g_menus[open];
    for (uint32_t &p : g_menu_px) p = 0xe0181818u;      // dark, a little see-through
    int y = 12;
    draw_text(g_menu_px, 16, y, mn.title, MENU_W);
    y += 30;
    for (int i = 0; i < mn.count; ++i, y += 22) {
        char line[64], buf[24];
        snprintf(line, sizeof line, "%s %-21s < %s >", i == mn.sel ? ">" : " ", mn.items[i].label,
                 menu_value(mn.items[i], buf, sizeof buf));
        draw_text(g_menu_px, 16, y, line, MENU_W);
    }
    y += 14;
    if (!g_gl && open == 0) {
        draw_text(g_menu_px, 16, y, "LAYOUT, RESOLUTION: GPU VIEW ONLY", MENU_W);
        y += 20;
    }
    draw_text(g_menu_px, 16, y, "UP/DOWN: CHOOSE  LEFT/RIGHT: CHANGE", MENU_W);
    draw_text(g_menu_px, 16, y + 20, "CIRCLE: CLOSE", MENU_W);
    return true;
}

bool alloc_framebuffers();

/* Whether anything engine B (the touch screen) reads differs from the last
   frame drawn: its registers, the VRAM bank setup, its palettes and OAM, its
   BG and OBJ VRAM and the extended-palette banks (H, I). Comparing is far
   cheaper than composing. */
bool engine_b_changed(const uint8_t *const *regions)
{
    static const struct { int region; uint32_t off, len; } kParts[] = {
        {4, 0x1000, 0x70}, {4, 0x240, 0x10}, {4, 0x304, 4},
        {5, 0x400, 0x400}, {7, 0x400, 0x400},
        {6, 0x200000, 0x20000}, {6, 0x600000, 0x20000},
        {6, 0x898000, 0x8000}, {6, 0x8A0000, 0x4000},
    };
    static uint8_t *saved;
    static size_t total;
    static bool have;
    if (!saved) {
        for (const auto &p : kParts) total += p.len;
        saved = new (std::nothrow) uint8_t[total];
        if (!saved) return true;
    }
    bool changed = !have;
    size_t o = 0;
    for (const auto &p : kParts) {
        const uint8_t *src = regions[p.region] + p.off;
        if (changed || memcmp(saved + o, src, p.len) != 0) {
            changed = true;
            memcpy(saved + o, src, p.len);
        }
        o += p.len;
    }
    have = true;
    return changed;
}

/* Where the drawing thread's time goes, logged every 300 frames drawn: the
   3D picture, the 2D composition, the presentation. */
void timing(double t0, double t3d, double t2d, double t_end)
{
    static double s3d, s2d, sp;
    static unsigned n;
    s3d += t3d - t0;
    s2d += t2d - t3d;
    sp += t_end - t2d;
    if (++n == 300) {
        printf("  [draw] per frame drawn: 3D %.1f ms, 2D %.1f ms, present %.1f ms (%s)\n",
               s3d * 1000 / n, s2d * 1000 / n, sp * 1000 / n, g_gl ? "GPU" : "CPU");
        s3d = s2d = sp = 0;
        n = 0;
    }
}

// The helper on the third core: while the drawing thread draws the 3D
// picture it composes the touch screen (when that is due), then the lower
// half of the top screen while the drawing thread does the upper half.
SceUID g_help_b, g_help_a, g_help_done;
struct HelpJob {
    ntr::Framebuffer *a, *b;
    const ntr::Framebuffer *layer3d;
    ntr::Framebuffer *a_ctl;           // the GPU mixes the 3D picture in (ppu.h)
    bool do_b;
    uint16_t mb_b;
} g_job;
constexpr int SPLIT = 96;      // engine A lines [0, SPLIT) here, the rest on the helper

int help_thread(SceSize, void *)
{
    for (;;) {
        sceKernelWaitSema(g_help_b, 1, nullptr);
        if (g_job.do_b) {
            ntr::ppu_compose_lines(ntr::ENGINE_B, *g_job.b, nullptr, 0, ntr::SCREEN_H, 2);
            ntr::ppu_apply_master_brightness(g_job.mb_b, *g_job.b);
        }
        sceKernelWaitSema(g_help_a, 1, nullptr);
        ntr::ppu_compose_lines(ntr::ENGINE_A, *g_job.a, g_job.layer3d, SPLIT, ntr::SCREEN_H, 1, g_job.a_ctl);
        sceKernelSignalSema(g_help_done, 1);
    }
    return 0;
}

int draw_thread(SceSize, void *)
{
    static ntr::Framebuffer a, b;
    g_help_b = sceKernelCreateSema("sm64ds_help_b", 0, 0, 1, nullptr);
    g_help_a = sceKernelCreateSema("sm64ds_help_a", 0, 0, 1, nullptr);
    g_help_done = sceKernelCreateSema("sm64ds_help_done", 0, 0, 1, nullptr);
    const SceUID helper = sceKernelCreateThread("sm64ds_compose", help_thread, 0x10000100, 64 * 1024, 0,
                                                0x40000 /* CPU_MASK_USER_2 */, nullptr);
    const bool have_helper = helper >= 0 && g_help_b >= 0 && g_help_a >= 0 && g_help_done >= 0;
    if (have_helper) sceKernelStartThread(helper, 0, nullptr);
    else printf("  live view: no helper thread (%#x): composing on one core\n", (unsigned)helper);
#if defined(SM64DS_VITAGL)
    if (g_gl && !sm64ds_gl_init()) {
        g_gl = false;
        if (!alloc_framebuffers()) return 0;
        show(g_fbs[0]);
    }
#endif
    for (;;) {
        sceKernelWaitSema(g_go, 1, nullptr);
        const uint8_t *const *regions = g_copy.region;
        const double t_start = seconds();
        uint16_t mb_a, mb_b, powcnt;
        memcpy(&mb_a, regions[4] + 0x6c, 2);
        memcpy(&mb_b, regions[4] + 0x106c, 2);
        memcpy(&powcnt, regions[4] + 0x304, 2);
        ntr::ppu_compose_set_regions(regions);
        // the touch screen rarely changes in play: drawn again only when
        // something engine B reads did (and then at most every other frame
        // drawn: its map marker moving would otherwise cost a full
        // composition each time). The helper starts on it at once.
        static unsigned b_parity;
        static ntr::Framebuffer a_ctl;
        g_job.a = &a;
        g_job.b = &b;
        // on the GPU, the 3D picture is mixed in there, at its resolution
        g_job.a_ctl = g_gl ? &a_ctl : nullptr;
        g_job.layer3d = (g_copy.has3d && !g_gl) ? &g_copy.g3d : nullptr;
        g_job.do_b = (++b_parity & 1) && engine_b_changed(regions);
        g_job.mb_b = mb_b;
        if (g_job.do_b) ntr::ppu_compose_prepare(ntr::ENGINE_B);
        ntr::ppu_compose_prepare(ntr::ENGINE_A);
        if (have_helper) sceKernelSignalSema(g_help_b, 1);
#if defined(SM64DS_VITAGL)
        // widescreen: the layout's, while engine A (the 3D) is the top screen
        uint16_t powcnt_now;
        memcpy(&powcnt_now, regions[4] + 0x304, 2);
        const bool wide = g_gl && kLayouts[g_layout].wide && (powcnt_now & 0x8000);
        sm64ds_wide_view = wide;     /* the game's view culling follows (d_clipper.cpp) */
        // "screen size": the 3D drawn at exactly the size its screen has on
        // the display -- no more pixels than the Vita shows, none scaled up
        int nw = 0, nh = 0;
        if (g_scale == 0) {
            const int *r = (powcnt_now & 0x8000) ? kLayouts[g_layout].top : kLayouts[g_layout].bottom;
            nw = r[2];
            nh = r[3];
        }
        if (g_copy.has3d && g_gl) sm64ds_gl_render_3d(g_copy.frame3d, nullptr, g_scale, wide, nw, nh);
        else
#endif
        if (g_copy.has3d) ntr::gx_render_frame(g_copy.frame3d, g_copy.g3d);
        const double t_3d = seconds();
        // the top screen in two halves, this core and the helper's
        if (have_helper) {
            sceKernelSignalSema(g_help_a, 1);
            ntr::ppu_compose_lines(ntr::ENGINE_A, a, g_job.layer3d, 0, SPLIT, 0, g_job.a_ctl);
            sceKernelWaitSema(g_help_done, 1, nullptr);
        } else {
            ntr::ppu_compose_lines(ntr::ENGINE_A, a, g_job.layer3d, 0, ntr::SCREEN_H, 0, g_job.a_ctl);
            if (g_job.do_b) {
                ntr::ppu_compose_lines(ntr::ENGINE_B, b, nullptr, 0, ntr::SCREEN_H, 2);
                ntr::ppu_apply_master_brightness(mb_b, b);
            }
        }
        if (!g_gl) ntr::ppu_apply_master_brightness(mb_a, a);    // the GPU's, else
        const double t_2d = seconds();
        // POWCNT1 bit 15: engine A on the top screen (the usual case)
        const bool a_top = powcnt & 0x8000;
        if (!g_gl) {
            g_fb = g_fbs[g_back];
            blit(a_top ? a : b, 0);         // left: the top screen
            blit(a_top ? b : a, SW);        // right: the touch screen
        }
        {
            // frames shown per second, over about a second
            static double t0;
            static unsigned n0;
            ++g_shown;
            const double t = seconds();
            if (t0 == 0) t0 = t;
            if (t - t0 >= 1.0) {
                g_shown_fps = (float)((g_shown - n0) / (t - t0));
                t0 = t;
                n0 = g_shown;
            }
        }
#if defined(SM64DS_VITAGL)
        if (g_gl) {
            static uint32_t text[960 * 16];
            memset(text, 0, sizeof text);       // transparent but for the line
            if (g_show_fps) fps_line(text, 0, 1, 960);
            toast_line(text, 0, 1, 960, g_show_fps);
            const Layout &l = kLayouts[g_layout];
            GlScreens sc;
            sc.a = &a;
            sc.a_ctl = &a_ctl;
            sc.b = &b;
            sc.a_top = a_top;
            sc.top_rect = l.top;
            sc.bottom_rect = l.bottom;
            // engine A's BLDCNT, BLDALPHA, BLDY, BG0HOFS: how the 3D mixes in
            uint16_t bldcnt, bldalpha, bldy, hofs;
            memcpy(&bldcnt, regions[4] + 0x50, 2);
            memcpy(&bldalpha, regions[4] + 0x52, 2);
            memcpy(&bldy, regions[4] + 0x54, 2);
            memcpy(&hofs, regions[4] + 0x10, 2);
            auto clamp16 = [](unsigned v) { return (v > 16 ? 16 : v) / 16.0f; };
            sc.mix_b[0] = clamp16(bldalpha & 31);
            sc.mix_b[1] = clamp16((bldalpha >> 8) & 31);
            sc.mix_b[2] = clamp16(bldy & 31);
            sc.mix_b[3] = (float)((bldcnt >> 6) & 3);
            const int h = hofs & 0x1ff;
            sc.mix_m[0] = (h >= 256 ? h - 512 : h) / 256.0f;
            const unsigned mbm = (mb_a >> 14) & 3, mbf = mb_a & 31;
            sc.mix_m[1] = (mbm == 1 || mbm == 2) ? (float)mbm : 0.0f;
            sc.mix_m[2] = clamp16(mbf);
            sc.mix_m[3] = wide ? 0.75f : 1.0f;
            sc.text = reinterpret_cast<const uint8_t *>(text);
            sc.text_w = 960;
            sc.text_h = 16;
            sc.menu = g_menu_open >= 0 ? reinterpret_cast<const uint8_t *>(g_menu_px) : nullptr;
            sc.menu_changed = g_menu_open >= 0 && render_menu();
            sc.menu_w = MENU_W;
            sc.menu_h = MENU_H;
            sm64ds_gl_present(sc);
            timing(t_start, t_3d, t_2d, seconds());
            sceKernelSignalSema(g_done, 1);
            continue;
        }
#endif
        if (g_show_fps) fps_line(g_fb);
        toast_line(g_fb, 12, 12, VW, g_show_fps);
        if (g_menu_open >= 0) {             // the menu, in the middle
            render_menu();
            for (int y = 0; y < MENU_H; ++y)
                memcpy(g_fb + (VH - MENU_H) / 2 * VW + y * VW + (VW - MENU_W) / 2, g_menu_px + y * MENU_W,
                       MENU_W * 4);
        }
        show(g_fb);
        g_back ^= 1;
        timing(t_start, t_3d, t_2d, seconds());
        sceKernelSignalSema(g_done, 1);
    }
    return 0;
}

/* The CPU path's two display buffers. */
bool alloc_framebuffers()
{
    // the display reads CDRAM; a block there is sized in 256 KiB steps
    const SceUID uid = sceKernelAllocMemBlock("sm64ds_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                              2 * FB_BYTES, nullptr);
    if (uid < 0) return false;
    void *base = nullptr;
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) return false;
    g_fbs[0] = static_cast<uint32_t *>(base);
    g_fbs[1] = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(base) + FB_BYTES);
    memset(base, 0, 2 * FB_BYTES);
    g_fb = g_fbs[0];
    g_back = 1;
    return true;
}

void copy_range(uint32_t ds, uint32_t n)
{
    memcpy(g_copy.region[ds >> 24] + (ds & 0xFFFFFFu), ::ntr::host_ptr(ds), n);
}
#endif
}  // namespace

extern "C" bool sm64ds_display_init(void)
{
#if defined(__vita__)
    if (g_ready) return true;
#if defined(SM64DS_VITAGL)
    // The GPU unless ux0:data/sm64ds/cpu3d.txt exists (then the CPU draws
    // everything, as before). vitaGL owns the display: it starts on the
    // drawing thread, which falls back to the CPU if it cannot.
    {
        SceIoStat st;
        g_gl = sceIoGetstat("ux0:data/sm64ds/cpu3d.txt", &st) < 0;
        printf("  live view: %s\n", g_gl ? "the GPU (vitaGL)" : "the CPU (ux0:data/sm64ds/cpu3d.txt)");
    }
#endif
    if (!g_gl && !alloc_framebuffers()) return false;
    settings_load();
    for (int x = 0; x < SW; ++x) g_sx[x] = x * ntr::SCREEN_W / SW;
    for (int y = 0; y < SH; ++y) g_sy[y] = y * ntr::SCREEN_H / SH;
    // the copy: I/O, palettes, VRAM (views at 0x06000000.., the banks'
    // homes up to 0x068A4000), OAM
    static const uint32_t sizes[8] = {0, 0, 0, 0, 0x2000, 0x800, 0x8A4000, 0x800};
    for (int r = 4; r < 8; ++r) {
        g_copy.region[r] = new (std::nothrow) uint8_t[sizes[r]]();
        if (!g_copy.region[r]) return false;
    }
    g_go = sceKernelCreateSema("sm64ds_draw_go", 0, 0, 1, nullptr);
    g_done = sceKernelCreateSema("sm64ds_draw_done", 0, 1, 1, nullptr);
    // the second core (SCE_KERNEL_CPU_MASK_USER_1), the default user priority
    const SceUID th = sceKernelCreateThread("sm64ds_draw", draw_thread, 0x10000100, 256 * 1024, 0,
                                            0x20000, nullptr);
    if (g_go < 0 || g_done < 0 || th < 0) return false;
    sceKernelStartThread(th, 0, nullptr);
    if (!g_gl) show(g_fbs[0]);
    g_ready = true;
    return true;
#else
    return false;
#endif
}

/* A frame for the live view: the 3D picture rendered here, the 2D state
   copied, the rest drawn on the drawing thread -- unless it is still busy
   with the last one, then this frame is skipped. */
extern "C" void sm64ds_display_present(void)
{
    if (!g_ready) return;
#if defined(__vita__)
    if (sceKernelPollSema(g_done, 1) < 0) {
        ++g_dropped;
        return;
    }
    g_copy.has3d = NTR_MMIO(uint32_t, 0x04000000) & 0x8;     // DISPCNT A: BG0 is 3D
    // the 3D frame is captured here (its triangles and the textures they
    // use) and rendered on the drawing thread with the rest
    if (g_copy.has3d) ntr::gx_capture_frame(g_copy.frame3d);
    copy_range(0x04000000, 0x2000);         // I/O
    copy_range(0x05000000, 0x800);          // palettes
    copy_range(0x07000000, 0x800);          // OAM
    copy_range(0x06000000, 0x80000);        // engine A BG
    copy_range(0x06200000, 0x20000);        // engine B BG
    copy_range(0x06400000, 0x40000);        // engine A OBJ
    copy_range(0x06600000, 0x20000);        // engine B OBJ
    copy_range(0x06800000, 0xA4000);        // the banks' homes (extended palettes, LCDC)
    sceKernelSignalSema(g_go, 1);
#endif
}

/* Once per DS frame: counts the game's frames per second for the line. */
extern "C" void sm64ds_display_tick(void)
{
    static double t0;
    static unsigned frames;
    const double t = seconds();
    if (t0 == 0) t0 = t;
    ++frames;
    if (t - t0 >= 1.0) {
        g_game_fps = (float)(frames / (t - t0));
        t0 = t;
        frames = 0;
    }
}

/* The game's threads (the frame loop, the game thread, each DS thread) stay
   on the first core, the drawing thread has the second to itself. Left to
   the scheduler, a game thread waking from the frame pacing's sleep could
   land on the drawing core and the two would take turns there. */
extern "C" void sm64ds_thread_started(void)
{
#if defined(__vita__)
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x10000);  // CPU_MASK_USER_0
#endif
}

/* Presents the drawing thread had to skip (it was still busy), since boot. */
extern "C" unsigned sm64ds_display_dropped(void) { return g_dropped; }

/* A point on the Vita's display (960x544) -> a DS dot on the touch screen
   (shown on the right half); false when it is outside that screen. */
/* Called on each pad read (the game's turn) with the Vita's buttons, the
   left stick folded into the D-pad's, and the buttons alone: true while a
   menu has them, the game then sees none. A menu opens with the D-pad
   itself, not the stick. */
extern "C" bool sm64ds_menu_pad(uint32_t buttons, uint32_t dpad_buttons)
{
#if !defined(__vita__)
    (void)buttons;
    (void)dpad_buttons;
    return false;
#else
    static uint32_t prev;
    static bool wait_release;
    const uint32_t pressed = buttons & ~prev;
    prev = buttons;
    static uint32_t prev_dpad;
    const uint32_t pressed_dpad = dpad_buttons & ~prev_dpad;
    prev_dpad = dpad_buttons;
    const uint32_t kOpenKeys = SCE_CTRL_CIRCLE | SCE_CTRL_SQUARE | SCE_CTRL_LEFT | SCE_CTRL_RIGHT;
    if (g_menu_open < 0) {
        if (wait_release) {         // the button that closed it, not the game's yet
            if (buttons & kOpenKeys) return true;
            wait_release = false;
        }
        int open = -1;
        if ((dpad_buttons & SCE_CTRL_RIGHT) && (dpad_buttons & SCE_CTRL_SQUARE)
            && (pressed_dpad & (SCE_CTRL_RIGHT | SCE_CTRL_SQUARE)))
            open = 0;
        else if ((dpad_buttons & SCE_CTRL_LEFT) && (dpad_buttons & SCE_CTRL_CIRCLE)
                 && (pressed_dpad & (SCE_CTRL_LEFT | SCE_CTRL_CIRCLE)))
            open = 1;
        if (open < 0) return false;
        g_menu_open = open;
        ++g_menu_gen;
        printf("  %s menu open\n", open ? "cheats" : "settings");
        // the buttons that opened it are still down: nothing changes until
        // they are let go and pressed again
        return true;
    }
    Menu &mn = g_menus[g_menu_open];
    if (pressed & SCE_CTRL_CIRCLE) {
        g_menu_open = -1;
        wait_release = true;
        settings_save();
        printf("  menu closed: layout %d, 3D %dx, frame rate %s, rear captures %s, speed %d%%, right stick camera %s;"
               " infinite lives %s, moon jump %s, infinite power %s, infinite breath %s, 99 coins %s, god mode %s\n",
               g_layout, g_scale, g_show_fps ? "shown" : "hidden", g_rear_capture ? "on" : "off",
               kSpeeds[g_speed_idx], g_right_stick_camera ? "on" : "off",
               sm64ds_cheat_infinite_lives ? "on" : "off", sm64ds_cheat_moon_jump ? "on" : "off",
               sm64ds_cheat_infinite_power ? "on" : "off", sm64ds_cheat_infinite_breath ? "on" : "off",
               sm64ds_cheat_99_coins ? "on" : "off", sm64ds_cheat_god_mode ? "on" : "off");
        return true;
    }
    if (pressed & SCE_CTRL_UP) mn.sel = (mn.sel + mn.count - 1) % mn.count;
    if (pressed & SCE_CTRL_DOWN) mn.sel = (mn.sel + 1) % mn.count;
    int step = 0;
    if (pressed & (SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) step = 1;
    if (pressed & SCE_CTRL_LEFT) step = -1;
    if (step) {
        const MenuItem &m = mn.items[mn.sel];
        int v = *m.value + step;
        if (v > m.hi) v = m.lo;
        if (v < m.lo) v = m.hi;
        *m.value = v;
    }
    if (pressed) ++g_menu_gen;
    return true;
#endif
}

/* The game's speed in percent (the settings menu), and whether the right
   stick turns the camera. */
extern "C" int sm64ds_game_speed_pct(void)
{
#if defined(__vita__)
    return kSpeeds[g_speed_idx];
#else
    return 100;
#endif
}

extern "C" bool sm64ds_right_stick_camera(void)
{
#if defined(__vita__)
    return g_right_stick_camera != 0;
#else
    return false;
#endif
}

/* Whether a rear-panel tap should take a capture (the settings menu). */
extern "C" bool sm64ds_rear_capture_enabled(void)
{
#if defined(__vita__)
    return g_rear_capture != 0;
#else
    return true;
#endif
}

extern "C" bool sm64ds_display_to_touch(int vx, int vy, int *x, int *y)
{
    // where the touch screen is: the layout's (GPU view) or the right half
    int r[4] = {SW, Y0, SW, SH};
    if (g_gl) {
        // the touch screen is the bottom one of the layout: the drawing thread
        // already put engine B there unless POWCNT1 bit 15 swapped them, and
        // the panel is the DS's lower screen either way
        const int *t = kLayouts[g_layout].bottom;
        for (int i = 0; i < 4; ++i) r[i] = t[i];
    }
    vx -= r[0];
    vy -= r[1];
    if (r[2] <= 0 || vx < 0 || vx >= r[2] || vy < 0 || vy >= r[3]) return false;
    *x = vx * ntr::SCREEN_W / r[2];
    *y = vy * ntr::SCREEN_H / r[3];
    return true;
}

/* A message on the text line for two seconds (save states, port/vita/savestate_host.cpp). */
extern "C" void sm64ds_display_toast(const char *msg)
{
    snprintf(g_toast, sizeof g_toast, "%s", msg);
    g_toast_until = seconds() + 2.0;
}

