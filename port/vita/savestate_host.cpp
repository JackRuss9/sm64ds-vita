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

/* The frame hook, on the game's thread. */
void sm64ds_savestate_frame(void)
{
    camera_watch();
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
