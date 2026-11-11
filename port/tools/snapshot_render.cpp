// Offline redraw of a display snapshot from gate 11 (snapshot_<frame>.bin):
// loads the I/O registers, palettes, VRAM and OAM into the host's DS memory,
// replays every recorded 3D command from a reset 3D engine, and writes what
// the renderers make of it -- engine A's 2D, engine B's 2D, the 3D picture --
// as BMPs. The renderers can then be worked on against the real title screen
// without a trip to the console for every change.
//
//   snapshot_render <snapshot.bin> <out_prefix>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ntr/gx.h"
#include "ntr/mmio.h"
#include "ntr/ppu.h"

namespace ntr { extern bool g_gx_debug_nodepth; }
// The ntr library logs through port/vita/vita_log.h; here stdout is fine.
extern "C" { FILE *sm64ds_log_file = nullptr; }

/* The deferred-3D composition (what the Vita's GPU view uses: 2D on the CPU,
   the 3D picture mixed in on the GPU, ppu.h) against the ordinary one: the
   GPU's mixing done here in integers, every pixel compared. */
static void defer_check(const ntr::Framebuffer &want, const ntr::Framebuffer &g3d)
{
    static ntr::Framebuffer off, ctl;
    const uint8_t *regions[8] = {};
    for (uint32_t r = 4; r < 8; ++r) regions[r] = static_cast<const uint8_t *>(ntr::host_ptr(r << 24));
    ntr::ppu_compose_set_regions(regions);
    ntr::ppu_compose_prepare(ntr::ENGINE_A);
    ntr::ppu_compose_lines(ntr::ENGINE_A, off, nullptr, 0, ntr::SCREEN_H, 0, &ctl);
    const uint16_t bldcnt = *static_cast<const uint16_t *>(ntr::host_ptr(0x04000050));
    const uint16_t bldalpha = *static_cast<const uint16_t *>(ntr::host_ptr(0x04000052));
    const unsigned bmode = (bldcnt >> 6) & 3;
    unsigned eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = *static_cast<const uint16_t *>(ntr::host_ptr(0x04000054)) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    const int hofs0 = *static_cast<const uint16_t *>(ntr::host_ptr(0x04000010)) & 0x1ff;
    const int hofs = hofs0 >= 256 ? hofs0 - 512 : hofs0;
    auto ch = [](uint32_t c, int sh) { return (c >> sh) & 255u; };
    int bad = 0, n3d = 0;
    for (int y = 0; y < ntr::SCREEN_H; ++y)
        for (int x = 0; x < ntr::SCREEN_W; ++x) {
            uint32_t out = off.px[y][x] & 0xffffff;
            const uint32_t c = ctl.px[y][x], code = c >> 24, cx = c & 0xffffff;
            const int sx = x + hofs;
            const uint32_t p3 = (sx >= 0 && sx < ntr::SCREEN_W) ? g3d.px[y][sx] : 0;
            const unsigned a3 = p3 >> 24;
            if ((code & 3) && a3) {
                ++n3d;
                const uint32_t c3 = p3 & 0xffffff;
                out = 0;
                for (int sh = 0; sh < 24; sh += 8) {
                    const unsigned v3 = ch(c3, sh), vx = ch(cx, sh);
                    unsigned v;
                    if ((code & 3) == 1) {
                        if (a3 < 31 && (code & 4)) v = (v3 * (a3 + 1) + vx * (31 - a3)) >> 5;
                        else if ((code & 8) && (bmode == 2 || bmode == 3))
                            v = bmode == 2 ? v3 + (((255 - v3) * evy) >> 4) : v3 - ((v3 * evy) >> 4);
                        else if ((code & 8) && bmode == 1 && (code & 4)) { v = (v3 * eva + vx * evb) >> 4; if (v > 255) v = 255; }
                        else v = v3;
                    } else {
                        const unsigned wa = (code >> 2) & 31, wb = (code & 0x80) ? evb : 16 - wa;
                        v = (vx * wa + v3 * wb) >> 4;
                        if (v > 255) v = 255;
                    }
                    out |= v << sh;
                }
            }
            if (out != (want.px[y][x] & 0xffffff)) {
                if (bad < 5) std::printf("defer: (%d,%d) want %06x got %06x code %02x\n", x, y, want.px[y][x] & 0xffffff, out, code);
                ++bad;
            }
        }
    std::printf("defer: %d pixels differ (%d with the 3D picture mixed in)\n", bad, n3d);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s snapshot.bin out_prefix\n", argv[0]);
        return 2;
    }
    FILE *f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 1; }
    char magic[8];
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "SM64SNAP", 8)) {
        std::fprintf(stderr, "not a snapshot\n");
        return 1;
    }
    ntr::io_init();
    std::vector<uint32_t> gx;
    uint32_t h[3];
    while (std::fread(h, 4, 3, f) == 3) {
        std::vector<unsigned char> buf(h[2]);
        if (std::fread(buf.data(), 1, h[2], f) != h[2]) break;
        char tag[5] = {};
        std::memcpy(tag, &h[0], 4);
        std::printf("section %s at %08x, %u bytes\n", tag, h[1], h[2]);
        if (!std::memcmp(tag, "GX3D", 4)) {
            gx.resize(h[2] / 4);
            std::memcpy(gx.data(), buf.data(), gx.size() * 4);
        } else if (h[1] >= 0x04000000u && h[1] < 0x08000000u) {      // I/O, palettes, VRAM, OAM
            std::memcpy(ntr::host_ptr(h[1]), buf.data(), h[2]);
        }   // MAIN, OAMS (the game's own buffers): for reading, not needed to draw
    }
    std::fclose(f);
    ntr::vram_restore_state();

    ntr::gx_reset();
    if (std::getenv("SNAP_FRAMES")) {
        // the rolling record: every frame it holds, one 3D picture each
        // (<prefix>_f<frame>_3d.bmp, the frame number from the game's tags)
        static ntr::Framebuffer one;
        size_t i = 0, from = 0;
        unsigned tag = 0, n = 0;
        while (i < gx.size()) {
            const unsigned cmd = gx[i] & 0xff, np = (gx[i] >> 8) & 0xff;
            if (i + 1 + np > gx.size()) break;
            if (cmd == 0 && np == 1 && (gx[i + 1] >> 16) == 0x4652) tag = gx[i + 1] & 0xffff;
            i += 1 + np;
            if (cmd == 0x50) {
                ntr::gx_replay(gx.data() + from, i - from);
                from = i;
                ntr::gx_render_layer(one);
                char fn[512];
                std::snprintf(fn, sizeof fn, "%s_f%05u_3d.bmp", argv[2], tag);
                ntr::ppu_write_bmp(fn, one);
                ++n;
            }
        }
        std::printf("wrote %u frames\n", n);
        return 0;
    }
    ntr::gx_replay(gx.data(), gx.size());
    uint32_t st[6];
    ntr::gx_debug_stats(st);
    size_t ntri = 0;
    const ntr::GxTriangle *t = ntr::gx_polygons(ntri);
    std::printf("3D replay: %u commands, %u swaps, %u projection loads, %zu triangles shown\n", st[0],
                st[2], st[5], ntri);
    const int verbose = std::getenv("SNAP_TRIS") ? std::atoi(std::getenv("SNAP_TRIS")) : 8;
    for (size_t i = 0; i < ntri && (int)i < verbose; ++i) {
        std::printf("  tri %zu: tex %p %dx%d attr %08x wrap %x\n", i, (const void *)t[i].tex, t[i].tw, t[i].th, t[i].attr, t[i].wrap);
        for (int k = 0; k < 3; ++k)
            std::printf("     v%d x %.1f y %.1f z %.3f w %.3f uv %.1f,%.1f color %08x\n", k,
                        t[i].v[k].x, t[i].v[k].y, t[i].v[k].z, t[i].v[k].w, t[i].v[k].u,
                        t[i].v[k].v, t[i].v[k].color);
    }

    static ntr::Framebuffer fb, g3d;
    char name[512];
    // each engine composed as the DS shows it (ppu_compose), the 3D layer
    // as engine A's BG0 when DISPCNT says so; SNAP_LAYERS=1 also writes the
    // older layer-over-layer pictures (_A_old, _B_old) for comparison
    if (const char *nb = std::getenv("SNAP_BENCH")) {
        // time the live view's work: the 3D layer, then both screens composed
        const int n = std::atoi(nb);
        auto ms = [] { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; };
        double t0 = ms();
        for (int i = 0; i < n; ++i) ntr::gx_render_layer(g3d);
        double t1 = ms();
        for (int i = 0; i < n; ++i) {
            ntr::ppu_compose(ntr::ENGINE_A, fb, &g3d);
            ntr::ppu_compose(ntr::ENGINE_B, fb, nullptr);
        }
        double t2 = ms();
        std::printf("bench: 3D layer %.2f ms, both screens composed %.2f ms (each of %d runs)\n",
                    (t1 - t0) / n, (t2 - t1) / n, n);
    }
    ntr::gx_render_layer(g3d);
    const bool has3d = *static_cast<const uint32_t *>(ntr::host_ptr(0x04000000)) & 8;
    for (int e = 0; e < 2; ++e) {
        const ntr::Engine eng = e ? ntr::ENGINE_B : ntr::ENGINE_A;
        ntr::ppu_compose(eng, fb, (!e && has3d) ? &g3d : nullptr);
        if (!e && has3d && std::getenv("SNAP_DEFER")) defer_check(fb, g3d);
        ntr::ppu_master_brightness(eng, fb);
        std::snprintf(name, sizeof name, "%s_%c.bmp", argv[2], e ? 'B' : 'A');
        ntr::ppu_write_bmp(name, fb);
        if (std::getenv("SNAP_LAYERS")) {
            ntr::ppu_scanout(eng, fb);
            ntr::ppu_scanout_obj(eng, fb);
            std::snprintf(name, sizeof name, "%s_%c_old.bmp", argv[2], e ? 'B' : 'A');
            ntr::ppu_write_bmp(name, fb);
        }
    }
    for (int y = 0; y < ntr::SCREEN_H; ++y)
        for (int x = 0; x < ntr::SCREEN_W; ++x) fb.px[y][x] = 0xFF000000u;
    ntr::g_gx_debug_nodepth = std::getenv("SNAP_NODEPTH") != nullptr;
    ntr::gx_render(fb);
    std::snprintf(name, sizeof name, "%s_3d.bmp", argv[2]);
    ntr::ppu_write_bmp(name, fb);
    std::printf("wrote %s_{A,B,3d}.bmp\n", argv[2]);
    return 0;
}
