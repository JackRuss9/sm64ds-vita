// Save states (D-pad Up + R saves, D-pad Down + L loads, as in the Super Mario
// Sunshine Vita port).
//
// The game runs as native code, so its state is not one emulated memory
// image: it is the game's own globals (.data and .bss of the game's object
// files and the ROM-image storage, listed from the link map into
// state_ranges.bin by port/tools/state_ranges.py), the DS memory the port keeps
// (main RAM, I/O registers, palettes, VRAM, OAM), the arenas the game's heaps
// are carved from, and three pieces of the port that describe that memory --
// the VRAM banks' mapping, the sound driver (its channels point into the
// game's samples) and which overlays loaded in what order. The port's own
// files, threads, GPU and caches are left as they are.
//
// Both happen on the game's thread, at the frame boundary (the frame hook),
// and only during play (main-loop step 7): while a level loads, the file
// system's thread is mid-way through its work and its stack is not saved.
// A state belongs to the build that wrote it (the code's addresses are in
// it): another build's is refused. One slot, ux0:data/sm64ds/state.bin.
//
// File: "SM64DSST", version, build tag, range count; then per range its id,
// its size, a byte per 4 KB page (0 all zero, 1 stored) and the stored pages.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "ntr/mmio.h"
#include "ntr/rt.h"

extern "C" {
extern int data_0209d50c;               /* the main loop's step: 7 while playing; the ranges' anchor */
bool sm64ds_arena_span(int i, void **p, unsigned *n);          /* arena_host.cpp */
unsigned sm64ds_snd_state_size(void);                          /* snd_host.cpp */
void sm64ds_snd_state_save(void *out);
void sm64ds_snd_state_load(const void *in);
unsigned sm64ds_romimage_state_size(void);                     /* hal/romimage_rt.cpp */
void sm64ds_romimage_state_save(void *out);
void sm64ds_romimage_state_load(const void *in);
void sm64ds_display_toast(const char *msg);                    /* vita_display.cpp */
void sm64ds_savestate_frame(void);
}

namespace {
const char *const kStateFile = "ux0:data/sm64ds/state.bin";
constexpr uint32_t PAGE = 4096, VERSION = 1;

struct Range {
    uint32_t id;
    unsigned char *p;
    uint32_t size;
};

/* What a state holds. The small pieces of the port go through buffers
   (`blob`), filled before a save and applied after a load. */
std::vector<unsigned char> g_vram_map, g_snd, g_ovl;
/* The game's globals: state_ranges.bin (port/tools/state_ranges.py, from
   the link map), link-time addresses relative to data_0209d50c, read once. */
std::vector<std::pair<uint32_t, uint32_t>> g_game;
uint32_t g_game_tag;
bool game_ranges()
{
    static int state;               // 0 not read, 1 read, -1 missing
    if (state) return state > 0;
    state = -1;
    FILE *f = fopen("app0:state_ranges.bin", "rb");
    if (!f) {
        printf("  save state: app0:state_ranges.bin missing, save states off\n");
        return false;
    }
    char magic[8];
    uint32_t h[2];
    if (fread(magic, 1, 8, f) == 8 && !memcmp(magic, "SMDSRNG1", 8) && fread(h, 4, 2, f) == 2) {
        const intptr_t delta = (intptr_t)(uintptr_t)&data_0209d50c - (intptr_t)h[1];
        uint32_t ab[2];
        for (uint32_t i = 0; i < h[0] && fread(ab, 4, 2, f) == 2; ++i) {
            g_game.push_back({(uint32_t)((intptr_t)ab[0] + delta), ab[1]});
            g_game_tag = g_game_tag * 31u + ab[0] * 7u + ab[1];
        }
        if (g_game.size() == h[0]) state = 1;
    }
    fclose(f);
    printf("  save state: %u ranges of game globals\n", (unsigned)g_game.size());
    return state > 0;
}

std::vector<Range> ranges()
{
    std::vector<Range> r;
    for (size_t i = 0; i < g_game.size(); ++i)
        r.push_back({1000u + (uint32_t)i, (unsigned char *)(uintptr_t)g_game[i].first, g_game[i].second});
    const struct { uint32_t id; uintptr_t base, size; } ds[] = {
        {2, ntr::MAIN_BASE, ntr::MAIN_SIZE}, {3, ntr::IO_BASE, ntr::IO_SIZE},
        {4, ntr::PLTT_BASE, ntr::PLTT_SIZE}, {5, ntr::VRAM_BASE, ntr::VRAM_SIZE},
        {6, ntr::OAM_BASE, ntr::OAM_SIZE},
    };
    for (const auto &d : ds)
        if (void *p = ntr::host_ptr(d.base)) r.push_back({d.id, (unsigned char *)p, (uint32_t)d.size});
    void *p;
    unsigned n;
    for (int i = 0; sm64ds_arena_span(i, &p, &n); ++i) r.push_back({10u + i, (unsigned char *)p, n});
    g_vram_map.resize(ntr::vram_state_size());
    g_snd.resize(sm64ds_snd_state_size());
    g_ovl.resize(sm64ds_romimage_state_size());
    r.push_back({30, g_vram_map.data(), (uint32_t)g_vram_map.size()});
    r.push_back({31, g_snd.data(), (uint32_t)g_snd.size()});
    r.push_back({32, g_ovl.data(), (uint32_t)g_ovl.size()});
    return r;
}

uint32_t build_tag() { return g_game_tag ^ (uint32_t)g_game.size(); }

/* A range can start at any byte (a game global of byte alignment): compared,
   not read a word at a time -- a word load the compiler pairs up faults on
   an odd address (the first save on hardware did). */
bool page_zero(const unsigned char *p, uint32_t n)
{
    static const unsigned char zero[PAGE] = {};
    return memcmp(p, zero, n) == 0;
}

bool save()
{
    std::vector<Range> rs = ranges();
    ntr::vram_state_save(g_vram_map.data());
    sm64ds_snd_state_save(g_snd.data());
    sm64ds_romimage_state_save(g_ovl.data());
    FILE *f = fopen(kStateFile, "wb");
    if (!f) return false;
    static char buf[1 << 20];
    setvbuf(f, buf, _IOFBF, sizeof buf);
    const uint32_t head[4] = {VERSION, build_tag(), (uint32_t)rs.size(), 0};
    bool ok = fwrite("SM64DSST", 1, 8, f) == 8 && fwrite(head, 4, 4, f) == 4;
    uint64_t stored = 0;
    std::vector<unsigned char> map;
    for (const Range &r : rs) {
        if (!ok) break;
        const uint32_t np = (r.size + PAGE - 1) / PAGE;
        map.assign(np, 0);
        for (uint32_t i = 0; i < np; ++i) {
            const uint32_t n = i + 1 < np ? PAGE : r.size - i * PAGE;
            map[i] = page_zero(r.p + i * PAGE, n) ? 0 : 1;
        }
        const uint32_t rh[2] = {r.id, r.size};
        ok = fwrite(rh, 4, 2, f) == 2 && fwrite(map.data(), 1, np, f) == np;
        for (uint32_t i = 0; ok && i < np; ++i) {
            if (!map[i]) continue;
            const uint32_t n = i + 1 < np ? PAGE : r.size - i * PAGE;
            ok = fwrite(r.p + i * PAGE, 1, n, f) == n;
            stored += n;
        }
    }
    ok = fclose(f) == 0 && ok;
    printf("  save state: %s, %u ranges, %u KB stored\n", ok ? "written" : "FAILED", (unsigned)rs.size(),
           (unsigned)(stored / 1024));
    if (!ok) remove(kStateFile);
    return ok;
}

/* -1: no state; 0: not this build's or damaged; 1: loaded. The file is read
   whole into memory with one read (a few MB), checked whole, and only then
   copied in place. (Seeking through the file instead refilled the 1 MB
   buffer at every range: hundreds of MB read, and the watchdog fired.) */
int load()
{
    FILE *f = fopen(kStateFile, "rb");
    if (!f) return -1;
    std::vector<unsigned char> file;
    bool ok = fseek(f, 0, SEEK_END) == 0;
    const long len = ok ? ftell(f) : -1;
    ok = ok && len >= 24 && fseek(f, 0, SEEK_SET) == 0;
    if (ok) {
        file.resize((size_t)len);
        ok = fread(file.data(), 1, (size_t)len, f) == (size_t)len;
    }
    fclose(f);
    std::vector<Range> rs = ranges();
    const unsigned char *const end = file.data() + file.size();
    const unsigned char *q = file.data();
    uint32_t head[4];
    if (ok) {
        memcpy(head, q + 8, 16);
        ok = !memcmp(q, "SM64DSST", 8) && head[0] == VERSION && head[1] == build_tag() && head[2] == rs.size();
        q += 24;
    }
    // first pass: every range where and as big as this build's, every page there
    const unsigned char *const body = q;
    for (const Range &r : rs) {
        if (!ok) break;
        uint32_t rh[2];
        const uint32_t np = (r.size + PAGE - 1) / PAGE;
        ok = end - q >= 8 + (long)np;
        if (!ok) break;
        memcpy(rh, q, 8);
        ok = rh[0] == r.id && rh[1] == r.size;
        const unsigned char *map = q + 8;
        q += 8 + np;
        size_t data = 0;
        for (uint32_t i = 0; ok && i < np; ++i)
            if (map[i]) data += i + 1 < np ? PAGE : r.size - i * PAGE;
        ok = ok && (size_t)(end - q) >= data;
        q += ok ? data : 0;
    }
    ok = ok && q == end;
    if (!ok) {
        printf("  save state: %s is not this build's (or is damaged), not loaded\n", kStateFile);
        return 0;
    }
    // second pass: in place
    q = body;
    for (const Range &r : rs) {
        const uint32_t np = (r.size + PAGE - 1) / PAGE;
        const unsigned char *map = q + 8;
        q += 8 + np;
        for (uint32_t i = 0; i < np; ++i) {
            const uint32_t n = i + 1 < np ? PAGE : r.size - i * PAGE;
            if (map[i]) {
                memcpy(r.p + i * PAGE, q, n);
                q += n;
            } else {
                memset(r.p + i * PAGE, 0, n);
            }
        }
    }
    ntr::vram_state_load(g_vram_map.data());
    sm64ds_snd_state_load(g_snd.data());
    sm64ds_romimage_state_load(g_ovl.data());
    printf("  save state: loaded\n");
    return 1;
}

volatile int g_request;     // 1 save, 2 load
}  // namespace

extern "C" {

/* From the input (port/vita/input_host.cpp): done at the next frame. */
void sm64ds_savestate_request(int action) { g_request = action; }

/* Camera watch (a bug report aid, logged to gate11.log): while playing, a
   frame where the camera or a player moves more than 1500 units at once is
   logged with the camera's state and every player's, the first 300 times.
   For the "camera goes crazy after freeing Mario" report. */
extern void *data_0209f318;             /* the camera (dCamera_c) */
extern void *data_0209f394[];           /* the players, by slot */
extern unsigned char gActivePlayerSlot;
bool sm64ds_in_game_arena(const void *p);   /* arena_host.cpp */
extern int data_02086fcc;               /* the camera's mode table, 0x28 a mode */
/* The camera's mode, area mode and state, logged as they change while
   playing (the first 300 changes), with how far it sits from what it looks
   at: for "the camera got stuck in the tower wall" reports. */
static void camera_modes(const char *c)
{
    static int left = 300, frame;
    static const void *last[3];
    ++frame;
    if (left <= 0) return;
    const void *now[3] = {*(void *const *)(c + 0x13c), *(void *const *)(c + 0x140), *(void *const *)(c + 0x138)};
    if (!memcmp(now, last, sizeof now)) return;
    memcpy(last, now, sizeof now);
    --left;
    const int32_t *pos = (const int32_t *)(c + 0x8c), *look = (const int32_t *)(c + 0x80);
    double d = 0;
    for (int k = 0; k < 3; ++k) d += ((double)pos[k] - look[k]) * ((double)pos[k] - look[k]);
    const auto mode = [](const void *m) {
        return m ? (int)(((const char *)m - (const char *)&data_02086fcc) / 0x28) : -1;
    };
    printf("  [camera %d] mode %d, area mode %d, state %p; %d units from what it looks at; pos %d %d %d look %d %d %d\n",
           frame, mode(now[0]), mode(now[1]), now[2], (int)(__builtin_sqrt(d) / 4096), pos[0] >> 12, pos[1] >> 12,
           pos[2] >> 12, look[0] >> 12, look[1] >> 12, look[2] >> 12);
}
/* The camera under what it looks at by more than 600 units (it "falls"
   at a spot on Whomp's tower): logged every 4th frame, 250 lines at most,
   with what it aims for and the wall it last hit. */
static void camera_low(const char *c)
{
    static int left = 250, frame;
    ++frame;
    if (left <= 0 || (frame & 3)) return;
    const int32_t *v = (const int32_t *)c;   // in words: 0x80 lookAt, 0x8c pos, 0x98 target, 0xe0 wall, 0xec normal
    if ((v[0x8c / 4 + 1] >> 12) >= (v[0x80 / 4 + 1] >> 12) - 600) return;
    --left;
    const auto u = [&](int off, int k) { return v[off / 4 + k] >> 12; };
    printf("  [camera low %d] pos %d %d %d look %d %d %d target %d %d %d wall %d %d %d normal %d %d %d"
           " flags %08x angles x %d y %d base %d pitch %d yaw %d timer %u view %p\n",
           frame, u(0x8c, 0), u(0x8c, 1), u(0x8c, 2), u(0x80, 0), u(0x80, 1), u(0x80, 2), u(0x98, 0), u(0x98, 1),
           u(0x98, 2), u(0xe0, 0), u(0xe0, 1), u(0xe0, 2), v[0xec / 4], v[0xec / 4 + 1], v[0xec / 4 + 2],
           *(const uint32_t *)(c + 0x154), *(const int16_t *)(c + 0x17e), *(const int16_t *)(c + 0x17c),
           *(const int16_t *)(c + 0x182), *(const int16_t *)(c + 0x1a2), *(const int16_t *)(c + 0x1a4),
           *(const uint16_t *)(c + 0x196), *(void *const *)(c + 0x144));
}
static void camera_watch(void)
{
    static int left = 300, frame;
    static int32_t prev[5][3];
    static bool have[5];
    ++frame;
    if (left <= 0) return;
    if (data_0209d50c != 7) {
        for (bool &h : have) h = false;
        return;
    }
    if (data_0209f318 && sm64ds_in_game_arena(data_0209f318)) {
        camera_modes((const char *)data_0209f318);
        camera_low((const char *)data_0209f318);
    }
    char *obj[5] = {(char *)data_0209f318};
    for (int i = 0; i < 4; ++i) obj[i + 1] = (char *)data_0209f394[i];
    int32_t now[5][3];
    bool ok[5], jump = false;
    for (int i = 0; i < 5; ++i) {
        ok[i] = obj[i] && sm64ds_in_game_arena(obj[i]);
        if (!ok[i]) { have[i] = false; continue; }
        memcpy(now[i], obj[i] + (i ? 0x5c : 0x8c), 12);   // dActor_c::mPos, dCamera_c::pos
        if (have[i]) {
            int64_t d = 0;
            for (int k = 0; k < 3; ++k) d += llabs((int64_t)now[i][k] - prev[i][k]);
            if (d > (int64_t)1500 << 12) jump = true;
        }
    }
    if (jump) {
        --left;
        char *c = obj[0];
        printf("  [watch %d] slot %u, &data_0209d50c %p; camera", frame, gActivePlayerSlot, (void *)&data_0209d50c);
        if (ok[0])
            printf(" pos %d %d %d (was %d %d %d) look %d %d %d target %p state %p mode %p area %p view %p fixed %p",
                   now[0][0] >> 12, now[0][1] >> 12, now[0][2] >> 12, prev[0][0] >> 12, prev[0][1] >> 12,
                   prev[0][2] >> 12, *(int32_t *)(c + 0x80) >> 12, *(int32_t *)(c + 0x84) >> 12,
                   *(int32_t *)(c + 0x88) >> 12, *(void **)(c + 0x110), *(void **)(c + 0x138),
                   *(void **)(c + 0x13c), *(void **)(c + 0x140), *(void **)(c + 0x144), *(void **)(c + 0x148));
        printf("\n");
        for (int i = 1; i < 5; ++i) {
            if (!ok[i]) continue;
            char *p = obj[i];
            printf("    player %d %p vt %p pos %d %d %d (was %d %d %d) state %p prev %p step %u timer %u no %u\n", i - 1,
                   (void *)p, *(void **)p, now[i][0] >> 12, now[i][1] >> 12, now[i][2] >> 12, prev[i][0] >> 12,
                   prev[i][1] >> 12, prev[i][2] >> 12, *(void **)(p + 0x370), *(void **)(p + 0x374),
                   *(uint8_t *)(p + 0x6e3), *(uint16_t *)(p + 0x6a4), *(uint8_t *)(p + 0x6d8));
        }
        fflush(stdout);
    }
    for (int i = 0; i < 5; ++i)
        if (ok[i]) { memcpy(prev[i], now[i], 12); have[i] = true; }
}

/* Player.cpp's root motion (func_ov002_020e640c, under the PC guard): a
   scripted animation's bone-1 translation moving the player. Logged when it
   is latched (kind 0) and when a frame moves the player more than 200 units
   (kind 1), the first 200 times: the "Mario jumps about after his rescue"
   report showed him snapping between the door and points far off. */
void sm64ds_rootmotion_log(int kind, int mode, const int *bone, const int *target, const int *before,
                           const int *after, int angle, const void *model, int file_base, int character)
{
    static int left = 200, frame;
    if (left <= 0) return;
    if (kind == 1) {
        int64_t d = 0;
        for (int k = 0; k < 3; ++k) d += llabs((int64_t)after[k] - before[k]);
        if (d <= (int64_t)200 << 12) return;
    }
    --left;
    (void)frame;
    printf("  [root %s] mode %d angle %04x bone1 %d %d %d target %d %d %d pos %d %d %d -> %d %d %d (x4096)\n",
           kind ? "move" : "latch", mode, angle & 0xffff, bone[0], bone[1], bone[2], target[0], target[1],
           target[2], before[0], before[1], before[2], after[0], after[1], after[2]);
    // the animation playing: dExtAnmModel_c's frame (+0x54 count and flags,
    // +0x58 frame) and BCA file (+0x60), and that file's header
    // (the model and file are checked for a plausible address only: the
    // game's objects are not all in the arenas)
    auto plausible = [](const void *p) {
        const uintptr_t a = (uintptr_t)p;
        return a && !(a & 3) && a >= 0x81000000u && a < 0xc0000000u;
    };
    const unsigned char *m = (const unsigned char *)model;
    // bone 1's own animation, if it has one (its record's +0x2c file, +0x30 frame)
    printf("    bone 1 own anim %08x frame %u;", (unsigned)bone[3], (unsigned)(uint16_t)bone[4]);
    if (plausible(m)) {
        const uint32_t *bca = *(const uint32_t *const *)(m + 0x60);
        printf(" anim %d (character %d): model %p frames/flags %08x frame %d file %p", file_base >> 2, character & 3,
               (const void *)m, *(const uint32_t *)(m + 0x54), *(const int32_t *)(m + 0x58) >> 12, (const void *)bca);
        if (plausible(bca))
            printf(" header %08x %08x %08x %08x %08x %08x (in heaps %d)", bca[0], bca[1], bca[2], bca[3], bca[4],
                   bca[5], (int)sm64ds_in_game_arena(bca));
    }
    printf("\n");
}

/* d_camera.cpp's follow camera (func_02009e70, under the PC guard), for
   the camera that drops under Whomp's tower stairs. Each frame it probes
   three times -- a line up from the camera (0), a line up from halfway to
   what it looks at (1), the ground under the first line's end (2) -- and
   clamps the height it wants between a floor and the ceilings found. The
   probes and the mode record are kept here; sm64ds_camera_height_log prints
   them all on a frame where a limit changes the height, the ground is 300
   units or more under the target, or the camera drops 40 units or more while
   the target does not: 600 lines at most. Each mode record in use is dumped
   once, raw. */
namespace {
struct CamProbe {
    int hit;
    int32_t from[3], to[3];
    int32_t clsn[3];            // the line's hit point, or the ground's height in [1]
    uint32_t clps[2];
    int32_t normal[3];
    unsigned tri, collider;
    unsigned owner_id;
    const void *owner;
};
CamProbe g_probe[3];
const void *g_cam_mode;
int g_cam_mode_vals[6];
}

void sm64ds_camera_probe(int kind, const void *query, int hit, const void *from, const void *to)
{
    if (kind < 0 || kind > 2) return;
    CamProbe &p = g_probe[kind];
    const unsigned char *q = (const unsigned char *)query;
    p.hit = hit;
    memcpy(p.from, from, 12);
    memcpy(p.to, to, 12);
    memset(p.clsn, 0, sizeof p.clsn);
    if (kind == 2) p.clsn[1] = *(const int32_t *)(q + 0x44);            // dBgCh_Gnd::clsnY
    else memcpy(p.clsn, q + 0x54, 12);                                   // dBgCh_Lin's hit point
    // the hit record, dBgPi at +0x10: surface (clps, normal) at +0x14,
    // triangle +0x28, collider slot +0x2a, owner +0x30
    memcpy(p.clps, q + 0x14, 8);
    memcpy(p.normal, q + 0x1c, 12);
    p.tri = *(const uint16_t *)(q + 0x28);
    p.collider = *(const uint16_t *)(q + 0x2a);
    p.owner = *(const void *const *)(q + 0x30);
    p.owner_id = p.owner && sm64ds_in_game_arena(p.owner) ? *(const uint16_t *)((const char *)p.owner + 0xc) : 0xffff;
}

void sm64ds_camera_mode_log(const void *mode, int dist, int min_slope, int max_slope, int r6, int sp04, int flags)
{
    g_cam_mode = mode;
    const int v[6] = {dist, min_slope, max_slope, r6, sp04, flags};
    memcpy(g_cam_mode_vals, v, sizeof v);
    static const void *dumped[64];
    static int ndumped;
    for (int i = 0; i < ndumped; ++i)
        if (dumped[i] == mode) return;
    if (ndumped >= 64 || !mode) return;
    dumped[ndumped++] = mode;
    const int32_t *w = (const int32_t *)mode;
    printf("  [camera mode record %d]", (int)(((const char *)mode - (const char *)&data_02086fcc) / 0x28));
    for (int i = 0; i < 10; ++i) printf(" %08x", (unsigned)w[i]);
    printf("\n");
}

static void camera_ring_height(int ground, int wanted, int floor_, int ceiling, int probe_y);
void sm64ds_camera_height_log(const void *cam, int ground, int wanted, int floor_, int ceiling, int probe_y)
{
    camera_ring_height(ground, wanted, floor_, ceiling, probe_y);
    static int left = 600;
    static int32_t last_pos_y, last_target_y;
    const int32_t *v = (const int32_t *)cam;
    const int32_t pos_y = v[0x8c / 4 + 1], target_y = v[0x98 / 4 + 1];
    const bool dropped = pos_y < last_pos_y - (40 << 12) && target_y >= last_target_y - (8 << 12);
    last_pos_y = pos_y;
    last_target_y = target_y;
    if (left <= 0) return;
    const bool clamped = wanted > ceiling || wanted < floor_;
    if (!clamped && !dropped && ground > target_y - (300 << 12)) return;
    --left;
    const int *m = g_cam_mode_vals;
    printf("  [camera height %u] wants %d, ground %d (probe from %d), floor %d, ceiling %d%s%s; pos %d %d %d"
           " look %d %d %d target %d %d %d; mode %d dist %d slopes %d..%d (min rise %d) sp04 %d flags %x\n",
           (unsigned)ntr::rt_frame(), wanted >> 12, ground >> 12, probe_y >> 12, floor_ >> 12, ceiling >> 12,
           wanted > ceiling ? " CEILING" : wanted < floor_ ? " FLOOR" : "", dropped ? " DROPPED" : "",
           v[0x8c / 4] >> 12, pos_y >> 12, v[0x8c / 4 + 2] >> 12, v[0x80 / 4] >> 12, v[0x80 / 4 + 1] >> 12,
           v[0x80 / 4 + 2] >> 12, v[0x98 / 4] >> 12, target_y >> 12, v[0x98 / 4 + 2] >> 12,
           g_cam_mode ? (int)(((const char *)g_cam_mode - (const char *)&data_02086fcc) / 0x28) : -1, m[0] >> 12,
           m[1], m[2], m[3] >> 12, m[4] >> 12, (unsigned)m[5]);
    static const char *const kind[3] = {"up from camera", "up from midpoint", "ground"};
    for (int i = 0; i < 3; ++i) {
        const CamProbe &p = g_probe[i];
        printf("    %s: %s %d %d %d -> %d %d %d", kind[i], p.hit ? "HIT" : "miss", p.from[0] >> 12, p.from[1] >> 12,
               p.from[2] >> 12, p.to[0] >> 12, p.to[1] >> 12, p.to[2] >> 12);
        if (p.hit)
            printf(" at %d %d %d, triangle %u collider %u owner %p (actor %04x), clps %08x %08x normal %d %d %d",
                   p.clsn[0] >> 12, p.clsn[1] >> 12, p.clsn[2] >> 12, p.tri, p.collider, p.owner, p.owner_id,
                   p.clps[0], p.clps[1], p.normal[0], p.normal[1], p.normal[2]);
        printf("\n");
    }
}

/* The camera's last 2 seconds, dumped to the log on a rear-panel tap
   (sm64ds_camera_ring_dump, from smoke_boot.cpp's capture). The other
   camera logs stop after a fixed count, and the Boo's Carousel glitch came
   after they had all run out; this one costs nothing until a tap. Each
   frame: the camera's position, what it looks at and aims for, its mode,
   area mode and state, flags and angles, the active player's position,
   and, when the follow camera ran, its height limits and probes. */
namespace {
struct CamRec {
    uint32_t frame;
    int32_t pos[3], look[3], target[3], player[3];
    int16_t mode, area;
    const void *state, *view, *fixed;
    uint32_t flags;
    int16_t ax, ay, base;
    bool height;                 // the follow camera's limits below are this frame's
    int32_t ground, wanted, floor_, ceiling, probe_y;
    int8_t hit[3];
    int32_t hit_y[3];
    uint16_t hit_tri[3], hit_owner[3];
};
CamRec g_cam_ring[120];
unsigned g_cam_ring_n;
CamRec g_cam_cur;
}

static void camera_ring_frame(void)
{
    const char *c = (const char *)data_0209f318;
    if (data_0209d50c != 7 || !c || !sm64ds_in_game_arena(c)) {
        g_cam_cur.height = false;
        return;
    }
    CamRec &r = g_cam_cur;
    r.frame = (uint32_t)ntr::rt_frame();
    memcpy(r.pos, c + 0x8c, 12);
    memcpy(r.look, c + 0x80, 12);
    memcpy(r.target, c + 0x98, 12);
    const auto mode = [](const void *m) {
        return (int16_t)(m ? ((const char *)m - (const char *)&data_02086fcc) / 0x28 : -1);
    };
    r.mode = mode(*(void *const *)(c + 0x13c));
    r.area = mode(*(void *const *)(c + 0x140));
    r.state = *(void *const *)(c + 0x138);
    r.view = *(void *const *)(c + 0x144);
    r.fixed = *(void *const *)(c + 0x148);
    r.flags = *(const uint32_t *)(c + 0x154);
    r.ax = *(const int16_t *)(c + 0x17e);
    r.ay = *(const int16_t *)(c + 0x17c);
    r.base = *(const int16_t *)(c + 0x182);
    const char *p = gActivePlayerSlot < 4 ? (const char *)data_0209f394[gActivePlayerSlot] : nullptr;
    if (p && sm64ds_in_game_arena(p)) memcpy(r.player, p + 0x5c, 12);
    else memset(r.player, 0, 12);
    g_cam_ring[g_cam_ring_n++ % 120] = r;
    r.height = false;
}

static void camera_ring_height(int ground, int wanted, int floor_, int ceiling, int probe_y)
{
    CamRec &r = g_cam_cur;
    r.height = true;
    r.ground = ground;
    r.wanted = wanted;
    r.floor_ = floor_;
    r.ceiling = ceiling;
    r.probe_y = probe_y;
    for (int i = 0; i < 3; ++i) {
        const CamProbe &p = g_probe[i];
        r.hit[i] = (int8_t)(p.hit != 0);
        r.hit_y[i] = p.clsn[1];
        r.hit_tri[i] = (uint16_t)p.tri;
        r.hit_owner[i] = (uint16_t)p.owner_id;
    }
}

extern "C" void sm64ds_camera_ring_dump(void)
{
    const unsigned n = g_cam_ring_n < 120 ? g_cam_ring_n : 120;
    printf("  [camera trace] the last %u frames (pos / look / target / player in units; modes index the table at 02086fcc)\n", n);
    for (unsigned i = g_cam_ring_n - n; i != g_cam_ring_n; ++i) {
        const CamRec &r = g_cam_ring[i % 120];
        printf("   %u: pos %d %d %d look %d %d %d target %d %d %d player %d %d %d mode %d area %d state %p view %p"
               " fixed %p flags %x angles %d %d base %d",
               r.frame, r.pos[0] >> 12, r.pos[1] >> 12, r.pos[2] >> 12, r.look[0] >> 12, r.look[1] >> 12,
               r.look[2] >> 12, r.target[0] >> 12, r.target[1] >> 12, r.target[2] >> 12, r.player[0] >> 12,
               r.player[1] >> 12, r.player[2] >> 12, r.mode, r.area, r.state, r.view, r.fixed, (unsigned)r.flags,
               r.ax, r.ay, r.base);
        if (r.height)
            printf("; follow: ground %d wants %d floor %d ceiling %d probe %d; up %s%d/%u/%04x mid %s%d/%u/%04x gnd %s%d/%u/%04x",
                   r.ground >> 12, r.wanted >> 12, r.floor_ >> 12, r.ceiling >> 12, r.probe_y >> 12,
                   r.hit[0] ? "HIT " : "miss ", r.hit_y[0] >> 12, r.hit_tri[0], r.hit_owner[0],
                   r.hit[1] ? "HIT " : "miss ", r.hit_y[1] >> 12, r.hit_tri[1], r.hit_owner[1],
                   r.hit[2] ? "HIT " : "miss ", r.hit_y[2] >> 12, r.hit_tri[2], r.hit_owner[2]);
        printf("\n");
    }
    // the camera object now, raw, and its mode records
    const char *c = (const char *)data_0209f318;
    if (c && sm64ds_in_game_arena(c)) {
        printf("  [camera trace] camera %p:", (const void *)c);
        for (int i = 0; i < 0x1c0 / 4; ++i) printf("%s%08x", i % 8 ? " " : "\n    +", (unsigned)((const uint32_t *)c)[i]);
        printf("\n");
        for (int k = 0; k < 2; ++k) {
            const uint32_t *m = *(const uint32_t *const *)(c + (k ? 0x140 : 0x13c));
            if (!m) continue;
            printf("  [camera trace] %s record:", k ? "area mode" : "mode");
            for (int i = 0; i < 10; ++i) printf(" %08x", (unsigned)m[i]);
            printf("\n");
        }
    }
    fflush(stdout);
}

extern void (*sm64ds_camera_probe_hook)(int, const void *, int, const void *, const void *);
extern void (*sm64ds_camera_height_hook)(const void *, int, int, int, int, int);
extern void (*sm64ds_camera_mode_hook)(const void *, int, int, int, int, int, int);
/* d_camera.cpp's trace hooks, set before the game starts */
static struct CameraHooks {
    CameraHooks()
    {
        sm64ds_camera_probe_hook = sm64ds_camera_probe;
        sm64ds_camera_height_hook = sm64ds_camera_height_log;
        sm64ds_camera_mode_hook = sm64ds_camera_mode_log;
    }
} g_camera_hooks;

/* The frame hook, on the game's thread. */
void sm64ds_savestate_frame(void)
{
    camera_watch();
    camera_ring_frame();
    const int r = g_request;
    if (!r) return;
    g_request = 0;
    if (!game_ranges()) {
        sm64ds_display_toast("SAVE STATES UNAVAILABLE");
        return;
    }
    if (data_0209d50c != 7) {
        sm64ds_display_toast(r == 1 ? "CANNOT SAVE STATE NOW" : "CANNOT LOAD STATE NOW");
        printf("  save state: not now (main-loop step %d)\n", data_0209d50c);
        return;
    }
    if (r == 1) {
        sm64ds_display_toast(save() ? "STATE SAVED" : "STATE NOT SAVED");
    } else {
        const int l = load();
        sm64ds_display_toast(l > 0 ? "STATE LOADED" : l < 0 ? "NO SAVED STATE" : "STATE FROM ANOTHER VERSION");
    }
    fflush(stdout);
}
}
