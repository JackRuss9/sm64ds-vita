// Startup fill of the game's ROM-resident globals. See romimage_rt.h.
#include "romimage_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int fill_of(const unsigned char *at)
{
    for (unsigned i = 0; i < rim_fill_count; ++i)
        if (at >= rim_fills[i].dst && at < rim_fills[i].dst + rim_fills[i].size)
            return (int)i;
    return -1;
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

extern "C" void sm64ds_romimage_run_inits(int module)
{
    for (unsigned i = 0; i < rim_init_count; ++i) {
        if (rim_inits[i].module != module) continue;
        for (void (*const *f)(void) = rim_inits[i].start; f < rim_inits[i].end; ++f)
            if (*f) (*f)();
    }
}

extern "C" bool sm64ds_romimage_load_module(int module)
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
