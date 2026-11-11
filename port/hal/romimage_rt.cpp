// Startup fill of the game's ROM-resident globals. See romimage_rt.h.
#include "romimage_rt.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "romfs.h"

namespace {

struct Image {
    uint8_t *bytes = nullptr;
    uint32_t base = 0;
    size_t len = 0;
};

inline uint32_t rd32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

// NitroSDK module params: the "21 06 C0 DE DE C0 06 21" magic closes them, and
// the first three words are the autoload list, its end and the autoload data
// start (all ARM9 RAM addresses). Each list entry is {address, size, bss}; the
// blocks' bytes follow one another from the data start. ITCM and DTCM come in
// this way.
bool find_autoload(const Image &arm9, uint32_t want_lo, uint32_t want_hi, Image *out) {
    static const uint8_t magic[8] = {0x21, 0x06, 0xC0, 0xDE, 0xDE, 0xC0, 0x06, 0x21};
    const size_t scan = arm9.len < 0x8000 ? arm9.len : 0x8000;
    for (size_t off = 0x1C; off + 8 <= scan; off += 4) {
        if (memcmp(arm9.bytes + off, magic, 8) != 0) continue;
        const uint8_t *params = arm9.bytes + off - 0x1C;
        const uint32_t list = rd32(params), list_end = rd32(params + 4);
        uint32_t data = rd32(params + 8);
        if (list < arm9.base || list_end < list || list_end - arm9.base > arm9.len ||
            (list_end - list) % 12)
            return false;
        for (uint32_t e = list; e < list_end; e += 12) {
            const uint8_t *ent = arm9.bytes + (e - arm9.base);
            const uint32_t addr = rd32(ent), size = rd32(ent + 4);
            if (data < arm9.base || data - arm9.base + size > arm9.len) return false;
            if (addr >= want_lo && addr < want_hi) {
                out->bytes = arm9.bytes + (data - arm9.base);   // borrowed
                out->base = addr;
                out->len = size;
                return true;
            }
            data += size;
        }
        return false;
    }
    return false;
}

}  // namespace

extern "C" unsigned sm64ds_romimage_filled = 0, sm64ds_romimage_missing = 0;

namespace {
char g_rom_path[256];

// Which fill each patch writes into, computed once: a module reload re-applies
// the patches of that module's fills only.
int *g_patch_fill;

/* The fill holding host address `at`. Asked once for each of ~13,000
   patches and the retargets: a scan of all ~13,000 fills each time was 170
   million comparisons, four seconds of the boot. The fills sorted by
   address once, then a binary search (the first fill holding it, as the
   scan found). */
unsigned *g_by_dst;
int fill_of(const unsigned char *at)
{
    if (!g_by_dst) {
        g_by_dst = (unsigned *)malloc(sizeof(unsigned) * (rim_fill_count ? rim_fill_count : 1));
        for (unsigned i = 0; i < rim_fill_count; ++i) g_by_dst[i] = i;
        std::stable_sort(g_by_dst, g_by_dst + rim_fill_count,
                         [](unsigned a, unsigned b) { return rim_fills[a].dst < rim_fills[b].dst; });
    }
    // the last fill starting at or before `at`, then back over any that
    // start earlier and still reach it (fills do not overlap in practice)
    unsigned lo = 0, hi = rim_fill_count;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (rim_fills[g_by_dst[mid]].dst <= at) lo = mid + 1;
        else hi = mid;
    }
    int best = -1;
    for (unsigned k = lo; k-- > 0 && lo - k <= 64;) {
        const RimFill &f = rim_fills[g_by_dst[k]];
        if (at >= f.dst && at < f.dst + f.size && (best < 0 || g_by_dst[k] < (unsigned)best)) best = (int)g_by_dst[k];
    }
    return best;
}
}  // namespace

namespace {
/* Load order of the overlays (0 = never loaded): the most recent of a set of
   overlays sharing an address is the resident one. */
unsigned g_load_seq[256], g_seq;
int *g_retarget_mod;     /* the module each retarget slot lives in */

bool resident(int m) { return m < 0 || (m < 256 && g_load_seq[m] != 0); }

void put(unsigned char *at, const void *p)
{
    const uint32_t v = (uint32_t)(uintptr_t)p;
    memcpy(at, &v, 4);
}

/* Overlay `module` has just been (re)loaded: pointers elsewhere that may mean
   it now do, and its own such pointers take the newest resident candidate. */
void retarget(int module)
{
    if (module < 0 || module >= 256 || !rim_retarget_count) return;
    if (!g_retarget_mod) {
        g_retarget_mod = (int *)malloc(sizeof(int) * rim_retarget_count);
        for (unsigned i = 0; i < rim_retarget_count; ++i) {
            const int fi = fill_of(rim_retargets[i].at);
            g_retarget_mod[i] = fi < 0 ? -99 : rim_fills[fi].module;
        }
    }
    g_load_seq[module] = ++g_seq;
    for (unsigned i = 0; i < rim_retarget_count;) {
        unsigned j = i;     /* entries of one slot are consecutive */
        while (j < rim_retarget_count && rim_retargets[j].at == rim_retargets[i].at) ++j;
        const int sm = g_retarget_mod[i];
        if (sm != -99 && resident(sm)) {
            unsigned best = j, best_seq = 0;
            for (unsigned k = i; k < j; ++k) {
                const int ov = rim_retargets[k].overlay;
                if (ov >= 0 && ov < 256 && g_load_seq[ov] > best_seq) { best = k; best_seq = g_load_seq[ov]; }
            }
            if (best < j && (sm == module || rim_retargets[best].overlay == module))
                put(rim_retargets[best].at, rim_retargets[best].target);
        }
        i = j;
    }
}
}  // namespace

namespace {
/* Overlay `module` has just been loaded at [base, base + len): every other
   overlay's storage for DS addresses in that range now holds this one's
   bytes, as the shared memory does on the DS. Overlays that load at the same
   place (the levels, at 0x0211xxxx) each have their own storage here, and
   code compiled against one of them -- the decomp names the address after
   the overlay it matched against -- reads that one: Big Goomboss's material
   animation is data_ov010_021123f4, whichever level is loaded, and in his
   own arena (another overlay) it read level 10's storage, never filled, and
   crashed. The copies carry this overlay's patched pointers, so they point
   into its storage. Only storage the DS range is covered by both is copied;
   the other overlay refills its own when it loads. */
struct Span { uint32_t ds; uint32_t size; unsigned char *dst; };
void mirror(int module, uint32_t base, uint32_t len)
{
    static Span *spans;
    static unsigned cap;
    unsigned n = 0;
    for (unsigned i = 0; i < rim_fill_count; ++i) {
        const RimFill &f = rim_fills[i];
        if (f.module != module) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            spans = (Span *)realloc(spans, cap * sizeof(Span));
        }
        spans[n++] = {f.ds_addr, f.size, f.dst};
    }
    if (!n) return;
    std::sort(spans, spans + n, [](const Span &a, const Span &b) { return a.ds < b.ds; });
    const uint32_t end = base + len;
    unsigned copies = 0, bytes = 0;
    for (unsigned i = 0; i < rim_fill_count; ++i) {
        const RimFill &f = rim_fills[i];
        if (f.module < 0 || f.module == module) continue;
        if (f.ds_addr >= end || f.ds_addr + f.size <= base) continue;
        // this overlay's spans that overlap f: from the last one starting at
        // or before f's start
        unsigned lo = 0, hi = n;
        while (lo < hi) {
            const unsigned mid = (lo + hi) / 2;
            if (spans[mid].ds <= f.ds_addr) lo = mid + 1;
            else hi = mid;
        }
        for (unsigned k = lo ? lo - 1 : 0; k < n && spans[k].ds < f.ds_addr + f.size; ++k) {
            const uint32_t a = spans[k].ds > f.ds_addr ? spans[k].ds : f.ds_addr;
            const uint32_t b0 = spans[k].ds + spans[k].size, b1 = f.ds_addr + f.size;
            const uint32_t b = b0 < b1 ? b0 : b1;
            if (a >= b) continue;
            memcpy(f.dst + (a - f.ds_addr), spans[k].dst + (a - spans[k].ds), b - a);
            ++copies;
            bytes += b - a;
        }
    }
    if (copies) printf("romimage: overlay %d mirrored into the overlays sharing its place (%u copies, %u KB)\n",
                       module, copies, bytes / 1024);
}
}  // namespace

extern "C" void sm64ds_romimage_run_inits(int module)
{
    for (unsigned i = 0; i < rim_init_count; ++i) {
        if (rim_inits[i].module != module) continue;
        for (void (*const *f)(void) = rim_inits[i].start; f < rim_inits[i].end; ++f)
            if (*f) (*f)();
    }
}

/* Module loads (overlays, the ARM9), for the log's slow-frame lines. */
static unsigned g_mod_loads;
static unsigned long long g_mod_us;
static unsigned long long mod_now_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000u + (unsigned long long)ts.tv_nsec / 1000u;
}
extern "C" void sm64ds_romimage_stats(unsigned *loads, unsigned long long *us)
{
    *loads = g_mod_loads;
    *us = g_mod_us;
}
static bool load_module_timed(int module);
extern "C" bool sm64ds_romimage_load_module(int module)
{
    const unsigned long long t0 = mod_now_us();
    const bool ok = load_module_timed(module);
    ++g_mod_loads;
    g_mod_us += mod_now_us() - t0;
    return ok;
}
static bool load_module_timed(int module)
{
    Image img;
    if (module == -1) img.bytes = romfs::read_arm9(g_rom_path, &img.base, &img.len);
    else img.bytes = romfs::read_overlay(g_rom_path, (unsigned)module, &img.base, &img.len);
    if (!img.bytes) return false;
    if (!g_patch_fill) {
        g_patch_fill = (int *)malloc(sizeof(int) * (rim_patch_count ? rim_patch_count : 1));
        for (unsigned i = 0; i < rim_patch_count; ++i) g_patch_fill[i] = fill_of(rim_patches[i].at);
    }
    for (unsigned i = 0; i < rim_fill_count; ++i) {
        const RimFill &f = rim_fills[i];
        if (f.module != module) continue;
        if (f.ds_addr >= img.base && f.ds_addr - img.base + f.size <= img.len)
            memcpy(f.dst, img.bytes + (f.ds_addr - img.base), f.size);
    }
    for (unsigned i = 0; i < rim_patch_count; ++i) {
        const int fi = g_patch_fill[i];
        if (fi < 0 || rim_fills[fi].module != module) continue;
        const uint32_t v = (uint32_t)(uintptr_t)rim_patches[i].target;
        memcpy(rim_patches[i].at, &v, 4);
    }
    for (unsigned i = 0; i < rim_bss_count; ++i)
        if (rim_bss[i].module == module) memset(rim_bss[i].dst, 0, rim_bss[i].size);
    if (module >= 0) mirror(module, img.base, img.len);
    free(img.bytes);
    retarget(module);
    sm64ds_romimage_run_inits(module);
    return true;
}

extern "C" bool sm64ds_romimage_load(const char *rom_path) {
    strncpy(g_rom_path, rom_path, sizeof g_rom_path - 1);
    Image arm9;
    arm9.bytes = romfs::read_arm9(rom_path, &arm9.base, &arm9.len);
    if (!arm9.bytes) {
        fprintf(stderr, "romimage: could not read the ARM9\n");
        return false;
    }
    Image itcm, dtcm;
    const bool have_itcm = find_autoload(arm9, 0x01FF8000u, 0x02000000u, &itcm);
    const bool have_dtcm = find_autoload(arm9, 0x023C0000u, 0x023C4000u, &dtcm);

    const int MAX_OV = 256;
    Image *ovs = (Image *)calloc(MAX_OV, sizeof(Image));
    unsigned filled = 0, missing = 0;
    for (unsigned i = 0; i < rim_fill_count; ++i) {
        const RimFill &f = rim_fills[i];
        Image *img = nullptr;
        if (f.module == -1) img = &arm9;
        else if (f.module == -2) img = have_itcm ? &itcm : nullptr;
        else if (f.module == -3) img = have_dtcm ? &dtcm : nullptr;
        else if (f.module >= 0 && f.module < MAX_OV) {
            img = &ovs[f.module];
            if (!img->bytes && img->base != 0xFFFFFFFFu) {
                img->bytes = romfs::read_overlay(rom_path, (unsigned)f.module, &img->base, &img->len);
                if (!img->bytes) img->base = 0xFFFFFFFFu;     // tried, failed
            }
            if (!img->bytes) img = nullptr;
        }
        if (!img || f.ds_addr < img->base || f.ds_addr - img->base + f.size > img->len) {
            ++missing;
            continue;
        }
        memcpy(f.dst, img->bytes + (f.ds_addr - img->base), f.size);
        ++filled;
    }
    for (unsigned i = 0; i < rim_patch_count; ++i) {
        const uint32_t v = (uint32_t)(uintptr_t)rim_patches[i].target;
        memcpy(rim_patches[i].at, &v, 4);
    }
    sm64ds_romimage_filled = filled;
    sm64ds_romimage_missing = missing;
    printf("romimage: %u globals filled from the ROM, %u pointers patched", filled, rim_patch_count);
    if (missing) printf(", %u outside their module image", missing);
    printf("\n");
    for (int i = 0; i < MAX_OV; ++i) free(ovs[i].bytes);
    free(ovs);
    free(arm9.bytes);
    return true;
}

/* The ROM this run reads from (the card stand-in, port/vita/card_host.cpp) */
extern "C" const char *sm64ds_rom_path(void) { return g_rom_path; }

/* Which overlays loaded in what order (the retargeting's view), for save
   states: restored with the game's memory it describes. */
extern "C" unsigned sm64ds_romimage_state_size(void) { return sizeof g_load_seq + sizeof g_seq; }
extern "C" void sm64ds_romimage_state_save(void *out)
{
    memcpy(out, g_load_seq, sizeof g_load_seq);
    memcpy((char *)out + sizeof g_load_seq, &g_seq, sizeof g_seq);
}
extern "C" void sm64ds_romimage_state_load(const void *in)
{
    memcpy(g_load_seq, in, sizeof g_load_seq);
    memcpy(&g_seq, (const char *)in + sizeof g_load_seq, sizeof g_seq);
}
