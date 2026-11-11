// The DS's nine VRAM banks on the host (GBATEK "DS Memory Control - VRAM").
//
// The host keeps VRAM as one flat 9 MB window by DS address, and the game
// reads and writes it with plain stores. On the DS a bank's bytes live in the
// bank: VRAMCNT_x maps it into a CPU-visible view (engine A/B backgrounds or
// sprites, or LCDC) or hands it to the 3D engine (texture images, texture
// palettes) or the 2D engines' extended palettes. A game that loads graphics
// under one mapping and then remaps -- every scene change does -- would see
// stale bytes in a flat window.
//
// So each bank has a home: its LCDC slot (0x06800000..0x068A3FFF), where the
// renderers' texture lookups find it too. When VRAMCNT maps a bank into a 2D
// view its home is copied there; when the bank leaves that view (any later
// VRAMCNT write for it) the view's bytes are copied back home. Texture and
// palette slots stay at home and are found through vram_tex_addr /
// vram_pltt_addr. Remaps are rare (a few per scene), so the copies cost
// nothing that matters. port/ntr/io.cpp calls vram_cnt_write on every store
// to VRAMCNT_A..I.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ntr/mmio.h"

namespace ntr {
void gx_texture_cache_clear();      // port/ntr/gx.cpp
namespace {

struct Bank {
    uint32_t home;      // LCDC address
    uint32_t size;
};
// A B C D E F G H I
constexpr Bank kBanks[9] = {
    {0x06800000u, 0x20000u}, {0x06820000u, 0x20000u}, {0x06840000u, 0x20000u},
    {0x06860000u, 0x20000u}, {0x06880000u, 0x10000u}, {0x06890000u, 0x4000u},
    {0x06894000u, 0x4000u},  {0x06898000u, 0x8000u},  {0x068A0000u, 0x4000u},
};
// VRAMCNT register of each bank (0x04000247 is WRAMCNT, not a bank)
constexpr uint32_t kCntAddr[9] = {0x04000240u, 0x04000241u, 0x04000242u, 0x04000243u,
                                  0x04000244u, 0x04000245u, 0x04000246u, 0x04000248u,
                                  0x04000249u};

uint32_t g_view[9];     // the 2D view address a bank is copied into, 0 = none
uint8_t g_cnt[9];

/* Where VRAMCNT value v puts bank b in a CPU-visible 2D view (0 if it does
   not: LCDC, 3D, extended palettes, ARM7). */
uint32_t view_of(int b, uint8_t v)
{
    if (!(v & 0x80)) return 0;
    const unsigned mst = v & 7, ofs = (v >> 3) & 3;
    switch (b) {
    case 0: case 1: case 2: case 3:                 // A B C D, 128 KB
        if (mst == 1) return 0x06000000u + 0x20000u * ofs;              // engine A BG
        if (mst == 2 && b < 2) return 0x06400000u + 0x20000u * (ofs & 1);  // A OBJ
        if (mst == 4 && b == 2) return 0x06200000u;                     // engine B BG
        if (mst == 4 && b == 3) return 0x06600000u;                     // engine B OBJ
        return 0;
    case 4:                                          // E, 64 KB
        if (mst == 1) return 0x06000000u;
        if (mst == 2) return 0x06400000u;
        return 0;
    case 5: case 6: {                                // F G, 16 KB
        const uint32_t o = 0x4000u * (ofs & 1) + 0x10000u * ((ofs >> 1) & 1);
        if (mst == 1) return 0x06000000u + o;
        if (mst == 2) return 0x06400000u + o;
        return 0;
    }
    case 7:                                          // H, 32 KB
        return mst == 1 ? 0x06200000u : 0;
    case 8:                                          // I, 16 KB
        if (mst == 1) return 0x06208000u;
        if (mst == 2) return 0x06600000u;
        return 0;
    }
    return 0;
}

void copy(uint32_t dst, uint32_t src, uint32_t n)
{
    memmove(host_ptr(dst), host_ptr(src), n);
}

}  // namespace

/* The banks' mapping, for save states (port/vita/savestate_host.cpp): saved
   and restored with the VRAM window it describes. */
size_t vram_state_size() { return sizeof g_view + sizeof g_cnt; }
void vram_state_save(uint8_t *out)
{
    memcpy(out, g_view, sizeof g_view);
    memcpy(out + sizeof g_view, g_cnt, sizeof g_cnt);
}
void vram_state_load(const uint8_t *in)
{
    memcpy(g_view, in, sizeof g_view);
    memcpy(g_cnt, in + sizeof g_view, sizeof g_cnt);
    gx_texture_cache_clear();
}

void vram_cnt_write(uint32_t addr, uint8_t value)
{
    for (int b = 0; b < 9; ++b) {
        if (kCntAddr[b] != addr) continue;
        const uint32_t old_view = g_view[b];
        const uint32_t new_view = view_of(b, value);
        if (g_cnt[b] != value) gx_texture_cache_clear();
        g_cnt[b] = value;
        if (old_view == new_view) return;
        if (old_view) copy(kBanks[b].home, old_view, kBanks[b].size);   // back home
        if (new_view) copy(new_view, kBanks[b].home, kBanks[b].size);   // into the view
        g_view[b] = new_view;
        return;
    }
}

/* The bank storage behind texture-image offset `off` (slots of 128 KB: the
   bank with MST 3 and that slot number among A..D). 0 if no bank is there. */
/* Is any bank given to the 3D engine -- texture images (A-D) or texture
   palettes (E-G)? The smoke harnesses map none and use the flat slots. */
bool vram_3d_banks(bool palettes)
{
    for (int b = palettes ? 4 : 0; b < (palettes ? 7 : 4); ++b)
        if ((g_cnt[b] & 0x87) == 0x83) return true;
    return false;
}

uint32_t vram_tex_addr(uint32_t off, uint32_t *avail)
{
    const unsigned slot = (off >> 17) & 3;
    for (int b = 0; b < 4; ++b) {
        const uint8_t v = g_cnt[b];
        if ((v & 0x80) && (v & 7) == 3 && ((v >> 3) & 3) == slot)
        {
            if (avail) *avail = 0x20000u - (off & 0x1FFFFu);
            return kBanks[b].home + (off & 0x1FFFFu);
        }
    }
    return 0;
}

/* The bank storage behind texture-palette offset `off` (16 KB slots 0..5: E
   covers 0..3 from its offset, F and G one slot each). 0 if none. */
uint32_t vram_pltt_addr(uint32_t off, uint32_t *avail)
{
    const unsigned slot = off >> 14;
    if ((g_cnt[4] & 0x87) == 0x83) {                 // E: 64 KB at slot 0
        if (slot < 4) {
            if (avail) *avail = 0x10000u - off;
            return kBanks[4].home + off;
        }
    }
    for (int b = 5; b <= 6; ++b) {
        const uint8_t v = g_cnt[b];
        if (!((v & 0x80) && (v & 7) == 3)) continue;
        const unsigned ofs = (v >> 3) & 3;
        const unsigned s = (ofs & 1) + 4 * ((ofs >> 1) & 1);
        if (s == slot) {
            if (avail) *avail = 0x4000u - (off & 0x3FFFu);
            return kBanks[b].home + (off & 0x3FFFu);
        }
    }
    return 0;
}

/* An extended palette slot (8 KB: 16 palettes of 256 colours) of engine
   `eng` (0 A, 1 B): BG slots 0..3, or the OBJ slot (obj, slot 0). They are
   never in a CPU view, so they stay at home. 0 if no bank holds it. */
uint32_t vram_ext_pltt(int eng, bool obj, unsigned slot)
{
    auto on = [](int b, unsigned mst) { return (g_cnt[b] & 0x87) == (0x80 | mst); };
    if (eng == 0) {
        if (!obj) {
            if (on(4, 4) && slot < 4) return kBanks[4].home + slot * 0x2000u;     // E: 0..3
            for (int b = 5; b <= 6; ++b)                                          // F G: 0-1 or 2-3
                if (on(b, 4) && (slot >> 1) == ((g_cnt[b] >> 3) & 1u))
                    return kBanks[b].home + (slot & 1u) * 0x2000u;
            return 0;
        }
        if (on(4, 5)) return kBanks[4].home;                                     // E
        for (int b = 5; b <= 6; ++b)
            if (on(b, 5)) return kBanks[b].home;                                  // F G
        return 0;
    }
    if (!obj) return on(7, 2) && slot < 4 ? kBanks[7].home + slot * 0x2000u : 0;  // H
    return on(8, 3) ? kBanks[8].home : 0;                                         // I
}

/* Take the mapping from the VRAMCNT registers as they already stand, moving
   no bytes: a snapshot loaded whole (port/tools/snapshot_render.cpp) already
   holds every bank where the DS had it. */
void vram_restore_state()
{
    for (int b = 0; b < 9; ++b) {
        const uint8_t v = *static_cast<const uint8_t *>(host_ptr(kCntAddr[b]));
        g_cnt[b] = v;
        g_view[b] = view_of(b, v);
    }
    gx_texture_cache_clear();
}

}  // namespace ntr
