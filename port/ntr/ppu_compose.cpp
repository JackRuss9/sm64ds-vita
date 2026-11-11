// DS 2D engine: the whole screen, composed as the hardware does it.
//
// ppu_scanout / ppu_scanout_obj (ppu.cpp) draw layers over one another, which
// is enough to see that graphics arrive. The DS does more, per pixel
// (GBATEK "DS Video", "LCD Color Special Effects", "LCD Window Feature"):
//
//   layers    BG0..BG3 in the mode DISPCNT bits 0-2 gives them -- text,
//             affine, extended (affine with 16-bit entries, 256-colour or
//             direct-colour bitmap); on engine A BG0 can be the 3D engine's
//             picture (DISPCNT bit 3), with the 3D alpha of each pixel;
//             sprites: normal, semi-transparent, OBJ-window and bitmap OBJs,
//             1D or 2D tile mapping, affine, 16/256 colours, extended
//             palettes;
//   order     the topmost two opaque layers at each pixel, by priority
//             (sprites before backgrounds at equal priority, then the lower
//             BG number, the backdrop last);
//   windows   WIN0, WIN1 and the OBJ window choose, per pixel, which layers
//             show and whether effects apply;
//   effects   BLDCNT: alpha blending of the top layer over the second
//             (BLDALPHA), brightness up/down (BLDY) on the top layer; a
//             semi-transparent sprite, and a 3D pixel that is not opaque,
//             blend with the layer below whenever that one is a second
//             target, whatever the mode.
//
// Master brightness is left to ppu_master_brightness, as before. Not done:
// mosaic, the large-bitmap mode 6, display capture, main-memory display.
#include "ntr/ppu.h"

#include "ntr/mmio.h"

#include <string.h>

#include <mutex>

namespace ntr {
namespace {

// DS address -> host bytes by the address's top byte (I/O, palettes, VRAM,
// OAM: each region one host block), looked up once per screen instead of
// through host_ptr() on every texel read -- the composer reads ~10 per pixel.
const uint8_t *g_base[8];
inline const uint8_t *at(uint32_t a) { return g_base[(a >> 24) & 7] + (a & 0xFFFFFFu); }
inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, at(a), 2); return v; }
inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, at(a), 4); return v; }
inline uint8_t rd8(uint32_t a) { return *at(a); }
void set_bases(const uint8_t *const regions[8])
{
    for (uint32_t r = 4; r < 8; ++r)
        g_base[r] = regions ? regions[r] : static_cast<const uint8_t *>(host_ptr(r << 24));
}

inline uint32_t expand(uint16_t c)       // BGR555 -> 0x00RRGGBB
{
    const uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

struct Eng {
    uint32_t reg, bg_vram, obj_vram, pltt;
    bool is_a;
};
constexpr Eng kEng[2] = {
    {0x04000000u, 0x06000000u, 0x06400000u, 0x05000000u, true},
    {0x04001000u, 0x06200000u, 0x06600000u, 0x05000400u, false},
};

enum { L_BG0, L_BG1, L_BG2, L_BG3, L_OBJ, L_BACK };

// One line of one layer: colour, and whether anything is there.
struct Line {
    uint32_t c[SCREEN_W];
    uint8_t on[SCREEN_W];
};
// The sprite line also keeps each pixel's priority, its kind, and 3D-style alpha.
struct ObjLine {
    uint32_t c[SCREEN_W];
    uint8_t on[SCREEN_W], prio[SCREEN_W], semi[SCREEN_W], alpha[SCREEN_W], win[SCREEN_W];
};

enum BgKind { BG_OFF, BG_TEXT, BG_AFFINE, BG_EXT_AFFINE, BG_BMP256, BG_BMP_DIRECT, BG_3D };

BgKind bg_kind(const Eng &e, int bg, uint32_t dispcnt, uint16_t cnt)
{
    if (!((dispcnt >> (8 + bg)) & 1)) return BG_OFF;
    const unsigned mode = dispcnt & 7;
    if (bg == 0) return (e.is_a && (dispcnt & 8)) ? BG_3D : BG_TEXT;
    if (bg == 1) return mode <= 5 ? BG_TEXT : BG_OFF;
    auto ext = [&]() {
        if (!(cnt & 0x80)) return BG_EXT_AFFINE;
        return (cnt & 0x04) ? BG_BMP_DIRECT : BG_BMP256;
    };
    if (bg == 2) {
        switch (mode) {
        case 0: case 1: case 3: return BG_TEXT;
        case 2: case 4: return BG_AFFINE;
        case 5: return ext();
        default: return BG_OFF;     // mode 6: large bitmap, not done
        }
    }
    switch (mode) {                  // BG3
    case 0: return BG_TEXT;
    case 1: case 2: return BG_AFFINE;
    case 3: case 4: case 5: return ext();
    default: return BG_OFF;
    }
}

// Each frame's palettes, expanded to 0x00RRGGBB once instead of on every
// pixel: the engine's 256 BG and 256 OBJ colours, and its extended palette
// slots (16 palettes of 256) when DISPCNT turns them on, expanded on first use.
struct Palettes {
    uint32_t bg[256], obj[256];
    uint32_t ext_bg[4][4096], ext_obj[4096];
    bool have_bg[4], have_obj;
    int eng;
};
// One set per engine, so the two engines can be composed at once (on two
// cores, ppu_compose_engine_from); `e` is in scope wherever it is used.
Palettes g_pals[2];
#define g_pal (g_pals[e.is_a ? 0 : 1])

void load_palettes(const Eng &e)
{
    for (unsigned i = 0; i < 256; ++i) {
        g_pal.bg[i] = expand(rd16(e.pltt + i * 2u));
        g_pal.obj[i] = expand(rd16(e.pltt + 0x200u + i * 2u));
    }
    g_pal.have_bg[0] = g_pal.have_bg[1] = g_pal.have_bg[2] = g_pal.have_bg[3] = false;
    g_pal.have_obj = false;
    g_pal.eng = e.is_a ? 0 : 1;
}
// The 4096 colours of an extended slot, or null if no bank holds it.
const uint32_t *ext_slot(const Eng &e, bool obj, unsigned slot)
{
    uint32_t *dst = obj ? g_pal.ext_obj : g_pal.ext_bg[slot & 3];
    bool &have = obj ? g_pal.have_obj : g_pal.have_bg[slot & 3];
    if (!have) {
        const uint32_t base = vram_ext_pltt(g_pal.eng, obj, slot);
        if (!base) return nullptr;
        for (unsigned i = 0; i < 4096; ++i) dst[i] = expand(rd16(base + i * 2u));
        have = true;
    }
    return dst;
}
// The BG's 256-colour palette table for a 16-palette index: the extended
// slot when DISPCNT bit 30 is on (BG0/BG1 pick slot 2/3 with BGCNT bit 13),
// else the standard one (`ext_stride` 256 then, 0 otherwise).
const uint32_t *bg_table(const Eng &e, int bg, uint32_t dispcnt, uint16_t cnt, unsigned &ext_stride)
{
    ext_stride = 0;
    if (dispcnt & 0x40000000u) {
        unsigned slot = bg;
        if (bg < 2 && (cnt & 0x2000)) slot += 2;
        if (const uint32_t *t = ext_slot(e, false, slot)) {
            ext_stride = 256;
            return t;
        }
    }
    return g_pal.bg;
}

void draw_text(const Eng &e, int bg, uint32_t dispcnt, uint16_t cnt, int y, Line &out)
{
    const uint32_t char_off = e.is_a ? ((dispcnt >> 24) & 7) << 16 : 0;
    const uint32_t scr_off = e.is_a ? ((dispcnt >> 27) & 7) << 16 : 0;
    const uint32_t screen = e.bg_vram + scr_off + (((cnt >> 8) & 31u) << 11);
    const uint8_t *chars = at(e.bg_vram + char_off + (((cnt >> 2) & 15u) << 14));
    const bool bpp8 = cnt & 0x80;
    const int tw = (cnt & 0x4000) ? 64 : 32, th = (cnt & 0x8000) ? 64 : 32;
    const int hofs = rd16(e.reg + 0x10 + bg * 4) & 0x1ff, vofs = rd16(e.reg + 0x12 + bg * 4) & 0x1ff;
    const int py = (y + vofs) & (th * 8 - 1);
    const int ty = py >> 3;
    unsigned ext_stride = 0;
    const uint32_t *pal8 = bpp8 ? bg_table(e, bg, dispcnt, cnt, ext_stride) : nullptr;
    const uint8_t *row = at(screen + (ty >> 5) * (tw >> 5) * 0x800u + (ty & 31) * 64u);
    int last_tx = -1;
    uint16_t se = 0;
    const uint8_t *tdata = nullptr;
    int fy = 0;
    for (int x = 0; x < SCREEN_W; ++x) {
        const int px = (x + hofs) & (tw * 8 - 1);
        const int tx = px >> 3;
        if (tx != last_tx) {            // one screen entry per tile, not per pixel
            last_tx = tx;
            memcpy(&se, row + (tx >> 5) * 0x800u + (tx & 31) * 2u, 2);
            fy = (se & 0x800) ? 7 - (py & 7) : (py & 7);
            tdata = chars + (se & 0x3ffu) * (bpp8 ? 64u : 32u);
        }
        const int fx = (se & 0x400) ? 7 - (px & 7) : (px & 7);
        unsigned index;
        if (bpp8) {
            index = tdata[fy * 8 + fx];
            if (!index) continue;
            out.c[x] = pal8[(se >> 12) * ext_stride + index];
        } else {
            const uint8_t pair = tdata[fy * 4 + (fx >> 1)];
            index = (fx & 1) ? pair >> 4 : pair & 15;
            if (!index) continue;
            out.c[x] = g_pal.bg[(se >> 12) * 16u + index];
        }
        out.on[x] = 1;
    }
}

// Affine and extended backgrounds: walk the texture through PA..PD from the
// reference point. The DS latches X/Y at the frame start and adds PB/PD each
// line; the registers as written are that start.
void draw_affine(const Eng &e, int bg, uint32_t dispcnt, uint16_t cnt, BgKind kind, int y, Line &out)
{
    const uint32_t r = e.reg + (bg == 2 ? 0x20 : 0x30);
    const int pa = (int16_t)rd16(r), pb = (int16_t)rd16(r + 2);
    const int pc = (int16_t)rd16(r + 4), pd = (int16_t)rd16(r + 6);
    int32_t rx = (int32_t)(rd32(r + 8) << 4) >> 4, ry = (int32_t)(rd32(r + 12) << 4) >> 4;
    rx += pb * y;
    ry += pd * y;
    const bool wrap = cnt & 0x2000;
    const unsigned size = (cnt >> 14) & 3;
    int w, h;
    if (kind == BG_BMP256 || kind == BG_BMP_DIRECT) {
        static const int bw[4] = {128, 256, 512, 512}, bh[4] = {128, 256, 256, 512};
        w = bw[size];
        h = bh[size];
    } else {
        w = h = 128 << size;
    }
    const uint32_t char_off = e.is_a ? ((dispcnt >> 24) & 7) << 16 : 0;
    const uint32_t scr_off = e.is_a ? ((dispcnt >> 27) & 7) << 16 : 0;
    const uint32_t screen = e.bg_vram + scr_off + (((cnt >> 8) & 31u) << 11);
    const uint32_t chars = e.bg_vram + char_off + (((cnt >> 2) & 15u) << 14);
    const uint32_t bitmap = e.bg_vram + (((cnt >> 8) & 31u) << 14);
    for (int x = 0; x < SCREEN_W; ++x, rx += pa, ry += pc) {
        int tx = rx >> 8, ty = ry >> 8;
        if (wrap) {
            tx &= w - 1;
            ty &= h - 1;
        } else if (tx < 0 || ty < 0 || tx >= w || ty >= h) {
            continue;
        }
        switch (kind) {
        case BG_AFFINE: {
            const unsigned tile = rd8(screen + (ty >> 3) * (w >> 3) + (tx >> 3));
            const unsigned index = rd8(chars + tile * 64u + (ty & 7) * 8u + (tx & 7));
            if (!index) continue;
            out.c[x] = g_pal.bg[index];
            break;
        }
        case BG_EXT_AFFINE: {
            const uint16_t se = rd16(screen + ((ty >> 3) * (w >> 3) + (tx >> 3)) * 2u);
            int fx = tx & 7, fy = ty & 7;
            if (se & 0x400) fx = 7 - fx;
            if (se & 0x800) fy = 7 - fy;
            const unsigned index = rd8(chars + (se & 0x3ffu) * 64u + fy * 8u + fx);
            if (!index) continue;
            {
                unsigned stride;
                const uint32_t *t = bg_table(e, bg, dispcnt, cnt, stride);
                out.c[x] = t[(se >> 12) * stride + index];
            }
            break;
        }
        case BG_BMP256: {
            const unsigned index = rd8(bitmap + ty * w + tx);
            if (!index) continue;
            out.c[x] = g_pal.bg[index];
            break;
        }
        default: {                                   // direct colour
            const uint16_t c = rd16(bitmap + (ty * w + tx) * 2u);
            if (!(c & 0x8000)) continue;
            out.c[x] = expand(c);
            break;
        }
        }
        out.on[x] = 1;
    }
}

void draw_objs(const Eng &e, uint32_t dispcnt, int y, ObjLine &o)
{
    static const int kSizes[3][4][2] = {
        {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
        {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
        {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
    };
    const uint32_t oam = e.is_a ? 0x07000000u : 0x07000400u;
    const bool map1d = dispcnt & 0x10;
    const uint32_t boundary = 32u << ((dispcnt >> 20) & 3);
    const uint32_t *ext_obj = (dispcnt & 0x80000000u) ? ext_slot(e, true, 0) : nullptr;
    // The lowest OAM index whose pixel is opaque wins the pixel (its priority
    // then competes with the backgrounds); so draw in index order and keep the
    // first. The OBJ window is separate: every window sprite adds to it.
    for (int i = 0; i < 128; ++i) {
        const uint16_t a0 = rd16(oam + i * 8u), a1 = rd16(oam + i * 8u + 2), a2 = rd16(oam + i * 8u + 4);
        const bool affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue;
        const unsigned mode = (a0 >> 10) & 3;
        const int shape = (a0 >> 14) & 3;
        if (shape == 3) continue;
        const int size = (a1 >> 14) & 3;
        const int w = kSizes[shape][size][0], h = kSizes[shape][size][1];
        const bool dbl = affine && (a0 & 0x200);
        const int bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;
        int sy = (y - (a0 & 0xff)) & 0xff;            // the y wraps at 256
        if (sy >= bh) continue;
        int x0 = a1 & 0x1ff;
        if (x0 >= 256) x0 -= 512;
        const bool c256 = a0 & 0x2000;
        const unsigned tile = a2 & 0x3ff, prio = (a2 >> 10) & 3, pal = a2 >> 12;
        int pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            const uint32_t g = oam + ((a1 >> 9) & 31u) * 32u + 6;
            pa = (int16_t)rd16(g);
            pb = (int16_t)rd16(g + 8);
            pc = (int16_t)rd16(g + 16);
            pd = (int16_t)rd16(g + 24);
        }
        for (int sx = 0; sx < bw; ++sx) {
            const int px = x0 + sx;
            if (px < 0 || px >= SCREEN_W) continue;
            int tx, ty;
            if (affine) {
                const int cx = sx - bw / 2, cy = sy - bh / 2;
                tx = ((pa * cx + pb * cy) >> 8) + w / 2;
                ty = ((pc * cx + pd * cy) >> 8) + h / 2;
                if (tx < 0 || tx >= w || ty < 0 || ty >= h) continue;
            } else {
                tx = (a1 & 0x1000) ? w - 1 - sx : sx;
                ty = (a1 & 0x2000) ? h - 1 - sy : sy;
            }
            uint32_t colour;
            uint8_t alpha = 31;
            if (mode == 3) {                                       // bitmap OBJ
                uint32_t addr;
                if (dispcnt & 0x40) {                               // 1D
                    addr = e.obj_vram + tile * (128u << ((dispcnt >> 22) & 1)) + (ty * w + tx) * 2u;
                } else if (dispcnt & 0x20) {                        // 2D, 256 dots wide
                    addr = e.obj_vram + (tile & 0x1f) * 0x10u + (tile & ~0x1fu) * 0x80u + (ty * 256 + tx) * 2u;
                } else {                                            // 2D, 128 dots wide
                    addr = e.obj_vram + (tile & 0x0f) * 0x10u + (tile & ~0x0fu) * 0x80u + (ty * 128 + tx) * 2u;
                }
                const uint16_t c = rd16(addr);
                if (!(c & 0x8000) || pal == 0) continue;
                colour = expand(c);
                alpha = static_cast<uint8_t>(pal * 2 + 1);          // 1..15 -> 3..31
            } else {
                const int tcol = tx >> 3, trow = ty >> 3, fx = tx & 7, fy = ty & 7;
                uint32_t taddr;
                if (map1d)
                    taddr = e.obj_vram + tile * boundary + (trow * (w >> 3) + tcol) * (c256 ? 64u : 32u);
                else
                    taddr = e.obj_vram + tile * 32u + trow * 1024u + tcol * (c256 ? 64u : 32u);
                unsigned index;
                if (c256) {
                    index = rd8(taddr + fy * 8u + fx);
                    if (!index) continue;
                    colour = ext_obj ? ext_obj[pal * 256u + index] : g_pal.obj[index];
                } else {
                    const uint8_t b = rd8(taddr + fy * 4u + (fx >> 1));
                    index = (fx & 1) ? b >> 4 : b & 15;
                    if (!index) continue;
                    colour = g_pal.obj[pal * 16u + index];
                }
            }
            if (mode == 2) {                                       // OBJ window
                o.win[px] = 1;
                continue;
            }
            if (o.on[px]) continue;
            o.on[px] = 1;
            o.c[px] = colour;
            o.prio[px] = static_cast<uint8_t>(prio);
            o.semi[px] = mode == 1;
            o.alpha[px] = alpha;
        }
    }
}

inline uint32_t blend(uint32_t a, uint32_t b, unsigned eva, unsigned evb)   // /16
{
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        unsigned v = (((a >> sh) & 255) * eva + ((b >> sh) & 255) * evb) >> 4;
        out |= (v > 255 ? 255 : v) << sh;
    }
    return out;
}
inline uint32_t blend32(uint32_t a, uint32_t b, unsigned eva)                // /32
{
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8)
        out |= ((((a >> sh) & 255) * eva + ((b >> sh) & 255) * (32 - eva)) >> 5) << sh;
    return out;
}
inline uint32_t brighten(uint32_t c, unsigned evy, bool up)
{
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        const unsigned v = (c >> sh) & 255;
        out |= (up ? v + (((255 - v) * evy) >> 4) : v - ((v * evy) >> 4)) << sh;
    }
    return out;
}

}  // namespace

namespace {
std::mutex g_compose_lock;      // the composer's state is static: one at a time
void compose(Engine eng, Framebuffer &fb, const Framebuffer *layer3d);
void prepare(Engine eng);
void compose_lines(Engine eng, Framebuffer &fb, const Framebuffer *layer3d, int y_begin, int y_end, int part,
                   Framebuffer *ctl = nullptr);
}

void ppu_compose(Engine eng, Framebuffer &fb, const Framebuffer *layer3d)
{
    std::lock_guard<std::mutex> hold(g_compose_lock);
    set_bases(nullptr);
    compose(eng, fb, layer3d);
}

void ppu_compose_from(Engine eng, Framebuffer &fb, const Framebuffer *layer3d,
                      const uint8_t *const regions[8])
{
    std::lock_guard<std::mutex> hold(g_compose_lock);
    set_bases(regions);
    compose(eng, fb, layer3d);
}

void ppu_compose_set_regions(const uint8_t *const regions[8])
{
    set_bases(regions);
}

void ppu_compose_prepare(Engine eng)
{
    prepare(eng);
}

void ppu_compose_lines(Engine eng, Framebuffer &fb, const Framebuffer *layer3d, int y_begin, int y_end, int part,
                       Framebuffer *ctl)
{
    compose_lines(eng, fb, layer3d, y_begin, y_end, part, ctl);
}

namespace {
/* A frame's palettes for engine `eng`, extended slots included, before any
   of its lines are composed (on one thread or several). */
void prepare(Engine eng)
{
    const Eng &e = kEng[eng];
    load_palettes(e);
    const uint32_t dispcnt = rd32(e.reg);
    if (dispcnt & 0x40000000u)
        for (unsigned slot = 0; slot < 4; ++slot) ext_slot(e, false, slot);
    if (dispcnt & 0x80000000u) ext_slot(e, true, 0);
}

void compose_lines(Engine eng, Framebuffer &fb, const Framebuffer *layer3d, int y_begin, int y_end, int part,
                   Framebuffer *ctl = nullptr);

void compose(Engine eng, Framebuffer &fb, const Framebuffer *layer3d)
{
    prepare(eng);
    compose_lines(eng, fb, layer3d, 0, SCREEN_H, eng == ENGINE_A ? 0 : 2);
}

/* Lines y_begin..y_end-1 of engine `eng` (its palettes prepared); `part`
   picks the line buffers, so different parts can run at once.

   With `ctl`, the 3D picture is not here at all: it is drawn on the GPU (at
   any resolution) and mixed in there, and this works out what each pixel
   needs for that (ppu.h, ppu_compose_lines). `fb` gets the pixel as it is
   where the 3D layer has nothing; `ctl` how the 3D layer changes it where it
   has something. */
void compose_lines(Engine eng, Framebuffer &fb, const Framebuffer *layer3d, int y_begin, int y_end, int part,
                   Framebuffer *ctl)
{
    const Eng &e = kEng[eng];
    const uint32_t dispcnt = rd32(e.reg);
    const unsigned disp_mode = (dispcnt >> 16) & 3;
    if (ctl && (disp_mode != 1 || (dispcnt & 0x80)))         // no layers: nothing to mix in
        for (int y = y_begin; y < y_end; ++y) memset(ctl->px[y], 0, sizeof ctl->px[y]);
    if (disp_mode == 0 || (dispcnt & 0x80)) {              // display off / forced blank: white
        for (int y = y_begin; y < y_end; ++y)
            for (int x = 0; x < SCREEN_W; ++x) fb.px[y][x] = 0xffffffffu;
        return;
    }
    if (disp_mode == 2 && e.is_a) {                         // VRAM display: an LCDC bank, direct
        const uint32_t bank = 0x06800000u + ((dispcnt >> 18) & 3) * 0x20000u;
        for (int y = y_begin; y < y_end; ++y)
            for (int x = 0; x < SCREEN_W; ++x)
                fb.px[y][x] = 0xff000000u | expand(rd16(bank + (y * 256 + x) * 2u));
        return;
    }
    if (disp_mode == 3) {                                   // main-memory display: not done
        for (int y = y_begin; y < y_end; ++y)
            for (int x = 0; x < SCREEN_W; ++x) fb.px[y][x] = 0xff000000u;
        return;
    }

    uint16_t cnt[4];
    BgKind kind[4];
    for (int b = 0; b < 4; ++b) {
        cnt[b] = rd16(e.reg + 0x08 + b * 2);
        kind[b] = bg_kind(e, b, dispcnt, cnt[b]);
        if (kind[b] == BG_3D && !layer3d && !ctl) kind[b] = BG_OFF;
    }
    const bool objs = dispcnt & 0x1000;
    const bool win0 = dispcnt & 0x2000, win1 = dispcnt & 0x4000, objwin = dispcnt & 0x8000;
    const bool any_win = win0 || win1 || objwin;
    const uint16_t winin = rd16(e.reg + 0x48), winout = rd16(e.reg + 0x4a);
    const uint16_t w0h = rd16(e.reg + 0x40), w1h = rd16(e.reg + 0x42);
    const uint16_t w0v = rd16(e.reg + 0x44), w1v = rd16(e.reg + 0x46);
    const uint16_t bldcnt = rd16(e.reg + 0x50), bldalpha = rd16(e.reg + 0x52);
    const unsigned bmode = (bldcnt >> 6) & 3;
    unsigned eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = rd16(e.reg + 0x54) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    const uint32_t backdrop = g_pal.bg[0];
    const bool deferred = ctl && kind[0] == BG_3D;
    if (ctl && !deferred)
        for (int y = y_begin; y < y_end; ++y) memset(ctl->px[y], 0, sizeof ctl->px[y]);

    auto inside = [](uint16_t hreg, uint16_t vreg, int x, int y) {
        const int x1 = hreg >> 8, x2 = hreg & 0xff, y1 = vreg >> 8, y2 = vreg & 0xff;
        const bool in_x = x1 <= x2 ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
        const bool in_y = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
        return in_x && in_y;
    };

    // the enabled BGs by priority, then number
    int order[4], norder = 0;
    for (int p = 0; p < 4; ++p)
        for (int b = 0; b < 4; ++b)
            if (kind[b] != BG_OFF && (cnt[b] & 3) == (unsigned)p) order[norder++] = b;

    static Line bgl_part[3][4];       // one set per part (see compose_lines)
    static ObjLine ol_part[3];
    Line *const bgl = bgl_part[part];
    ObjLine &ol = ol_part[part];
    for (int y = y_begin; y < y_end; ++y) {
        for (int b = 0; b < 4; ++b) {
            memset(bgl[b].on, 0, sizeof bgl[b].on);
            switch (kind[b]) {
            case BG_OFF: break;
            case BG_TEXT: draw_text(e, b, dispcnt, cnt[b], y, bgl[b]); break;
            case BG_3D: {
                const int hofs = rd16(e.reg + 0x10) & 0x1ff;   // BG0HOFS scrolls the 3D picture
                for (int x = 0; x < SCREEN_W; ++x) {
                    int sx = (x + (hofs >= 256 ? hofs - 512 : hofs));
                    if (sx < 0 || sx >= SCREEN_W) continue;
                    if (ctl) {                  // deferred: assume a 3D pixel, the GPU knows
                        bgl[b].c[x] = 0;
                        bgl[b].on[x] = 1;
                        continue;
                    }
                    const uint32_t p = layer3d->px[y][sx];
                    if (!(p >> 24)) continue;
                    bgl[b].c[x] = p;            // alpha (0..31) kept in the top byte
                    bgl[b].on[x] = 1;
                }
                break;
            }
            default: draw_affine(e, b, dispcnt, cnt[b], kind[b], y, bgl[b]); break;
            }
        }
        memset(ol.on, 0, sizeof ol.on);
        memset(ol.win, 0, sizeof ol.win);
        if (objs || objwin) draw_objs(e, dispcnt, y, ol);

        for (int x = 0; x < SCREEN_W; ++x) {
            unsigned ctl_bits = 0x3f;                       // layers 0-4 + effects
            if (any_win) {
                if (win0 && inside(w0h, w0v, x, y)) ctl_bits = winin & 0x3f;
                else if (win1 && inside(w1h, w1v, x, y)) ctl_bits = (winin >> 8) & 0x3f;
                else if (objwin && ol.win[x]) ctl_bits = (winout >> 8) & 0x3f;
                else ctl_bits = winout & 0x3f;
            }
            // the top two layers: priority 0 first; at each priority the
            // sprite, then BG0..BG3 (the BGs come pre-sorted that way)
            // the top three layers: priority 0 first; at each priority the
            // sprite, then BG0..BG3 (the BGs come pre-sorted that way); the
            // backdrop under them all
            int lay[3] = {L_BACK, L_BACK, L_BACK};
            uint32_t col[3] = {backdrop, backdrop, backdrop};
            const int want = deferred ? 3 : 2;
            int found = 0;
            bool obj_here = objs && ol.on[x] && (ctl_bits & 0x10);
            auto put = [&](int layer, uint32_t c) {
                lay[found] = layer;
                col[found] = c;
                ++found;
            };
            for (int k = 0; k < norder && found < want; ++k) {
                const int b = order[k];
                if (obj_here && ol.prio[x] <= (cnt[b] & 3)) {
                    put(L_OBJ, ol.c[x]);
                    obj_here = false;
                    if (found == want) break;
                }
                if (!bgl[b].on[x] || !(ctl_bits & (1u << b))) continue;
                put(b, bgl[b].c[x]);
            }
            if (found < want && obj_here) put(L_OBJ, ol.c[x]);
            const bool fx = ctl_bits & 0x20;
            // the DS's rules for a top layer over a second one
            auto mix = [&](int top, uint32_t ct, int second, uint32_t cs) {
                uint32_t out = ct & 0xffffffu;
                const bool second_ok = (bldcnt >> (8 + second)) & 1;
                const bool first_ok = (bldcnt >> top) & 1;
                if (top == L_BG0 && kind[0] == BG_3D) {
                    const unsigned a = ct >> 24;            // 1..31
                    if (a < 31 && second_ok) out = blend32(ct & 0xffffffu, cs & 0xffffffu, a + 1);
                    else if (fx && first_ok && (bmode == 2 || bmode == 3)) out = brighten(out, evy, bmode == 2);
                    else if (fx && first_ok && bmode == 1 && second_ok) out = blend(out, cs & 0xffffffu, eva, evb);
                } else if (top == L_OBJ && (ol.semi[x] || ol.alpha[x] < 31) && second_ok) {
                    if (ol.alpha[x] < 31)                    // bitmap OBJ alpha
                        out = blend(out, cs & 0xffffffu, (ol.alpha[x] + 1) / 2, 16 - (ol.alpha[x] + 1) / 2);
                    else
                        out = blend(out, cs & 0xffffffu, eva, evb);
                } else if (fx && first_ok) {
                    if (bmode == 1 && second_ok) out = blend(out, cs & 0xffffffu, eva, evb);
                    else if (bmode == 2 || bmode == 3) out = brighten(out, evy, bmode == 2);
                }
                return out;
            };
            if (!deferred) {
                fb.px[y][x] = 0xff000000u | mix(lay[0], col[0], lay[1], col[1]);
                continue;
            }
            // Deferred 3D: BG0 counted present at every pixel, so where it
            // is among the layers says what the GPU must do (ppu.h).
            uint32_t code = 0, cx = 0, out;
            if (lay[0] == L_BG0) {
                // 3D on top: without it, the next two; with it, the GPU mixes
                // the 3D pixel over the next layer by the rules above
                out = mix(lay[1], col[1], lay[2], col[2]);
                cx = col[1] & 0xffffffu;
                const bool second_ok = (bldcnt >> (8 + lay[1])) & 1;
                const bool first_ok = bldcnt & 1;
                code = 1u | (second_ok ? 4u : 0u) | ((fx && first_ok) ? 8u : 0u);
            } else if (lay[1] == L_BG0) {
                // 3D second, under a 2D pixel: that pixel blends with it, or
                // the 3D does not show
                out = mix(lay[0], col[0], lay[2], col[2]);
                const bool second_ok = (bldcnt >> 8) & 1;
                const bool first_ok = (bldcnt >> lay[0]) & 1;
                unsigned wa = 0;
                bool uniform_b = false, blends = false;
                if (lay[0] == L_OBJ && (ol.semi[x] || ol.alpha[x] < 31) && second_ok) {
                    blends = true;
                    if (ol.alpha[x] < 31) wa = (ol.alpha[x] + 1) / 2;
                    else { wa = eva; uniform_b = true; }
                } else if (fx && first_ok && bmode == 1 && second_ok) {
                    blends = true;
                    wa = eva;
                    uniform_b = true;
                }
                if (blends) {
                    cx = col[0] & 0xffffffu;
                    code = 2u | (wa << 2) | (uniform_b ? 0x80u : 0u);
                }
            } else {
                out = mix(lay[0], col[0], lay[1], col[1]);
            }
            fb.px[y][x] = 0xff000000u | out;
            ctl->px[y][x] = code << 24 | cx;
        }
    }
}

}  // namespace

}  // namespace ntr
