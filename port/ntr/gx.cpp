// DS geometry engine: command decode, matrix stacks, vertex assembly, raster.
//
// Command encoding and parameter counts from GBATEK. Fixed-point inputs are 4.12
// (fx32); the transform runs in float, which is what any host renderer would do
// and is not a fidelity question for geometry this size.

#include "ntr/gx.h"
#include "ntr/mmio.h"

#include "ntr/texture.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace ntr {
namespace {

constexpr float FX12 = 1.0f / 4096.0f;

// --- matrices ---------------------------------------------------------------
// DS matrices are row-vector convention: v' = v * M, translation in row 3.
struct Mat {
    float m[16];
    static Mat identity() {
        Mat r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
};

Mat mul(const Mat &a, const Mat &b) {
    Mat r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}

struct Vec4 { float x, y, z, w; };

Vec4 mul(const Vec4 &v, const Mat &m) {
    return {v.x * m.m[0] + v.y * m.m[4] + v.z * m.m[8] + v.w * m.m[12],
            v.x * m.m[1] + v.y * m.m[5] + v.z * m.m[9] + v.w * m.m[13],
            v.x * m.m[2] + v.y * m.m[6] + v.z * m.m[10] + v.w * m.m[14],
            v.x * m.m[3] + v.y * m.m[7] + v.z * m.m[11] + v.w * m.m[15]};
}

// --- engine state -----------------------------------------------------------
enum MtxMode { MTX_PROJ = 0, MTX_POS = 1, MTX_POSVEC = 2, MTX_TEX = 3 };

struct State {
    int mode = MTX_POS;
    Mat proj = Mat::identity();
    Mat pos = Mat::identity();
    Mat vec = Mat::identity();
    Mat tex = Mat::identity();

    Mat proj_stack[2];
    Mat pos_stack[32];
    Mat vec_stack[32];
    int proj_sp = 0, pos_sp = 0;

    uint32_t color = 0xFFFFFFFFu;
    float u = 0, v = 0;                 // current texture coordinates, in texels (after the transform)
    float s0 = 0, t0 = 0;               // the last TEXCOORD as sent, in texels
    int texmode = 0;                    // TEXIMAGE_PARAM bits 30-31: how coordinates are made
    uint8_t tex_wrap = 3;               // TEXIMAGE_PARAM bits 16-19 of the bound texture
    const uint32_t *tex_rgba = nullptr; // bound texture (Mat tex above is the
    int tw = 0, th = 0;                 // texture *matrix* -- different thing)
    int prim = -1;                 // BEGIN_VTXS type, -1 when not inside a primitive
    int16_t vx = 0, vy = 0, vz = 0;
    std::vector<GxVertex> strip;   // vertices accumulated in the current primitive
    int strip_parity = 0;

    int vp_x = 0, vp_y = 0, vp_w = SCREEN_W, vp_h = SCREEN_H;

    // Lighting state. diffuse/ambient/specular/emission are 0..1 per channel.
    float diffuse[3] = {1, 1, 1}, ambient[3] = {0, 0, 0};
    float emission[3] = {0, 0, 0};
    struct Light { float dx, dy, dz; float r, g, b; } lights[4] = {};
    uint32_t light_mask = 0;
    // POLYGON_ATTR: latched by 0x29, takes effect at the next BEGIN_VTXS.
    uint32_t attr_pending = 0x001F0080u, attr = 0x001F0080u;
    bool attr_seen = false;     // harnesses that never send one keep their lights

    std::vector<GxTriangle> tris;
    // The DS double-buffers its 3D: polygons sent during a frame show only
    // after SWAP_BUFFERS (0x50). `shown` is the last swapped frame; until the
    // first swap (the smoke harnesses draw one scene and never swap) the
    // frame in progress is what renders.
    std::vector<GxTriangle> shown;
    bool swapped = false;
};

State g;
int g_store_count;

uint32_t bgr555_to_argb(uint16_t c) {
    const uint32_t r = c & 0x1F, gg = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((gg << 3 | gg >> 2) << 8)
           | (b << 3 | b >> 2);
}

Mat &current_pos() { return g.pos; }

// Clip space -> screen: x/y in pixels, z in [0,1].
void to_screen(GxVertex &v) {
    const float iw = (std::fabs(v.w) > 1e-6f) ? 1.0f / v.w : 0.0f;
    // DS screen y runs bottom-up; the framebuffer is top-down.
    v.x = (v.cx * iw + 1.0f) * 0.5f * g.vp_w + g.vp_x;
    v.y = (1.0f - (v.cy * iw + 1.0f) * 0.5f) * g.vp_h + g.vp_y;
    v.z = (v.cz * iw + 1.0f) * 0.5f;
}

// Project a model-space vertex all the way to screen space.
GxVertex project(int16_t x, int16_t y, int16_t z) {
    const Vec4 v{x * FX12, y * FX12, z * FX12, 1.0f};
    const Vec4 c = mul(mul(v, current_pos()), g.proj);

    GxVertex out{};
    out.cx = c.x; out.cy = c.y; out.cz = c.z;
    out.w = c.w;
    to_screen(out);
    if (g.texmode == 3) {
        // vertex source (GBATEK "Texture Coordinates Transformation"): the
        // model-space vertex through the texture matrix, added to TEXCOORD
        const Mat &t = g.tex;
        const float vx = x * FX12, vy = y * FX12, vz = z * FX12;
        g.u = (vx * t.m[0] + vy * t.m[4] + vz * t.m[8]) / 16.0f + g.s0;
        g.v = (vx * t.m[1] + vy * t.m[5] + vz * t.m[9]) / 16.0f + g.t0;
    }
    out.u = g.u;
    out.v = g.v;
    out.color = g.color;
    return out;
}

void emit_clipped(const GxVertex &a, const GxVertex &b, const GxVertex &c);

// A point on the edge a-b in clip space, everything interpolated.
GxVertex clip_lerp(const GxVertex &a, const GxVertex &b, float t) {
    GxVertex o{};
    o.cx = a.cx + (b.cx - a.cx) * t; o.cy = a.cy + (b.cy - a.cy) * t;
    o.cz = a.cz + (b.cz - a.cz) * t; o.w = a.w + (b.w - a.w) * t;
    o.u = a.u + (b.u - a.u) * t; o.v = a.v + (b.v - a.v) * t;
    uint32_t col = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        const float ca = (a.color >> sh) & 0xFF, cb = (b.color >> sh) & 0xFF;
        col |= static_cast<uint32_t>(ca + (cb - ca) * t + 0.5f) << sh;
    }
    o.color = col;
    to_screen(o);
    return o;
}

// The DS clips every polygon to the view volume before it is drawn. What
// matters to a rasteriser is the near and far planes (-w <= z <= w): a
// triangle that reaches behind the camera has no meaningful screen
// position there -- the perspective divide flips it -- and drew as a huge
// stray wedge across the picture (the castle's walls in the intro
// cutscene). The side planes need no clipping: pixels off the screen are
// simply not drawn.
void emit_tri(const GxVertex &a, const GxVertex &b, const GxVertex &c) {
    auto inside = [](const GxVertex &v) { return v.w > 1e-5f && v.cz >= -v.w && v.cz <= v.w; };
    if (inside(a) && inside(b) && inside(c)) { emit_clipped(a, b, c); return; }
    GxVertex poly[8], tmp[8];
    int n = 3;
    poly[0] = a; poly[1] = b; poly[2] = c;
    for (int plane = 0; plane < 2 && n >= 3; ++plane) {
        // distance to the plane, positive inside: near z + w, far w - z
        auto dist = [plane](const GxVertex &v) { return plane == 0 ? v.cz + v.w : v.w - v.cz; };
        int m = 0;
        for (int i = 0; i < n; ++i) {
            const GxVertex &p = poly[i], &q = poly[(i + 1) % n];
            const float dp = dist(p), dq = dist(q);
            if (dp >= 0) tmp[m++] = p;
            if ((dp >= 0) != (dq >= 0)) tmp[m++] = clip_lerp(p, q, dp / (dp - dq));
        }
        n = m;
        for (int i = 0; i < n; ++i) poly[i] = tmp[i];
    }
    for (int i = 1; i + 1 < n; ++i)
        if (poly[0].w > 1e-5f && poly[i].w > 1e-5f && poly[i + 1].w > 1e-5f)
            emit_clipped(poly[0], poly[i], poly[i + 1]);
}

void emit_clipped(const GxVertex &a, const GxVertex &b, const GxVertex &c) {
    GxTriangle t;
    t.v[0] = a; t.v[1] = b; t.v[2] = c;
    t.tex = g.tex_rgba; t.tw = g.tw; t.th = g.th; t.wrap = g.tex_wrap;
    t.faces = static_cast<uint8_t>((g.attr >> 6) & 3);
    t.alpha = static_cast<uint8_t>((g.attr >> 16) & 31);
    t.attr = g.attr;
    // The DS's polygon RAM holds 2048 per frame; anything past twice that is a
    // frame that never swapped, not a scene.
    if (g.tris.size() < 4096) g.tris.push_back(t);
}

// Assemble according to the active BEGIN_VTXS primitive type.
void push_vertex(const GxVertex &v) {
    g.strip.push_back(v);
    const size_t n = g.strip.size();
    switch (g.prim) {
        case 0:                                        // separate triangles
            if (n == 3) { emit_tri(g.strip[0], g.strip[1], g.strip[2]); g.strip.clear(); }
            break;
        case 1:                                        // separate quads
            if (n == 4) {
                emit_tri(g.strip[0], g.strip[1], g.strip[2]);
                emit_tri(g.strip[0], g.strip[2], g.strip[3]);
                g.strip.clear();
            }
            break;
        case 2:                                        // triangle strip
            if (n >= 3) {
                const GxVertex &p0 = g.strip[n - 3], &p1 = g.strip[n - 2], &p2 = g.strip[n - 1];
                if ((n - 3) & 1) emit_tri(p1, p0, p2);  // alternate winding
                else emit_tri(p0, p1, p2);
            }
            break;
        case 3:                                        // quad strip
            if (n >= 4 && (n % 2) == 0) {
                const GxVertex &p0 = g.strip[n - 4], &p1 = g.strip[n - 3];
                const GxVertex &p2 = g.strip[n - 2], &p3 = g.strip[n - 1];
                emit_tri(p0, p1, p3);
                emit_tri(p0, p3, p2);
            }
            break;
        default: break;
    }
}

void vertex(int16_t x, int16_t y, int16_t z) {
    g.vx = x; g.vy = y; g.vz = z;
    if (g.prim >= 0) push_vertex(project(x, y, z));
}

// --- command execution ------------------------------------------------------
void load_mtx(Mat &dst, const uint32_t *p, int n) {
    Mat m = Mat::identity();
    if (n == 16) {
        for (int i = 0; i < 16; ++i) m.m[i] = static_cast<int32_t>(p[i]) * FX12;
    } else {  // 4x3: three rows of 3 plus a translation row
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) m.m[r * 4 + c] = static_cast<int32_t>(p[r * 3 + c]) * FX12;
        m.m[3] = m.m[7] = m.m[11] = 0.0f;
        m.m[15] = 1.0f;
    }
    dst = m;
}

Mat mat_3x3(const uint32_t *p) {
    Mat m = Mat::identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) m.m[r * 4 + c] = static_cast<int32_t>(p[r * 3 + c]) * FX12;
    return m;
}

struct GxStats { uint32_t cmds, swaps, vtx, last_shown, proj_loads; } g_stats;
void drop_old_retired();     // the texture cache, below

// Command recorder (gx_record_start): every executed command as
// [cmd | np << 8] then np parameter words, for an offline replay.
std::vector<uint32_t> g_rec;
size_t g_rec_cap;
bool g_recording;

// The rolling recorder (gx_ring_enable): the same words, always, into one
// fixed circular buffer, with where each frame (each SWAP_BUFFERS) starts --
// so a capture can hand over the last second or two that went by, not only
// the frames after the tap.
uint32_t *g_ring;
uint64_t g_ring_n, g_ring_pos;                 // size, words ever written
uint64_t g_ring_starts[512];                   // frame starts (positions)
uint32_t g_ring_frames;                        // frames started
size_t g_ring_at;                              // g_ring_pos % g_ring_n, kept
inline void ring_put(uint32_t w) {
    g_ring[g_ring_at] = w;
    if (++g_ring_at == g_ring_n) g_ring_at = 0;
    ++g_ring_pos;
}

void exec(uint8_t cmd, const uint32_t *p, int np) {
    ++g_stats.cmds;
    if (g_ring) {
        ring_put(cmd | (uint32_t)np << 8);
        for (int i = 0; i < np; ++i) ring_put(p[i]);
        if (cmd == 0x50) g_ring_starts[g_ring_frames++ % 512] = g_ring_pos;
    }
    if (g_recording) {
        if (g_rec.size() + 1 + (size_t)np > g_rec_cap) {
            g_recording = false;
        } else {
            g_rec.push_back(cmd | (uint32_t)np << 8);
            for (int i = 0; i < np; ++i) g_rec.push_back(p[i]);
        }
    }
    if (cmd >= 0x23 && cmd <= 0x28) ++g_stats.vtx;
    if (cmd >= 0x16 && cmd <= 0x19 && g.mode == 0) ++g_stats.proj_loads;
    (void)np;
    switch (cmd) {
        case 0x00: break;                                        // NOP
        case 0x10: g.mode = p[0] & 3; break;                     // MTX_MODE
        case 0x11:                                               // MTX_PUSH
            if (g.mode == MTX_PROJ) { if (g.proj_sp < 1) g.proj_stack[g.proj_sp++] = g.proj; }
            else if (g.pos_sp < 31) { g.pos_stack[g.pos_sp] = g.pos; g.vec_stack[g.pos_sp] = g.vec; ++g.pos_sp; }
            break;
        case 0x12: {                                             // MTX_POP
            int n = static_cast<int32_t>(p[0] << 26) >> 26;      // signed 6-bit
            if (g.mode == MTX_PROJ) { if (g.proj_sp > 0) g.proj = g.proj_stack[--g.proj_sp]; }
            else { g.pos_sp -= n; if (g.pos_sp < 0) g.pos_sp = 0;
                   if (g.pos_sp < 31) { g.pos = g.pos_stack[g.pos_sp]; g.vec = g.vec_stack[g.pos_sp]; } }
            break;
        }
        case 0x13: {                                             // MTX_STORE
            ++g_store_count;
            const int i = p[0] & 31;
            if (g.mode == MTX_PROJ) g.proj_stack[0] = g.proj;
            else { g.pos_stack[i] = g.pos; g.vec_stack[i] = g.vec; }
            break;
        }
        case 0x14: {                                             // MTX_RESTORE
            const int i = p[0] & 31;
            if (g.mode == MTX_PROJ) g.proj = g.proj_stack[0];
            else { g.pos = g.pos_stack[i]; g.vec = g.vec_stack[i]; }
            break;
        }
        case 0x15:                                               // MTX_IDENTITY
            if (g.mode == MTX_PROJ) g.proj = Mat::identity();
            else if (g.mode == MTX_TEX) g.tex = Mat::identity();
            else { g.pos = Mat::identity(); if (g.mode == MTX_POSVEC) g.vec = Mat::identity(); }
            break;
        case 0x16: case 0x17: {                                  // MTX_LOAD_4x4 / 4x3
            Mat m; load_mtx(m, p, cmd == 0x16 ? 16 : 12);
            if (g.mode == MTX_PROJ) g.proj = m;
            else if (g.mode == MTX_TEX) g.tex = m;
            else { g.pos = m; if (g.mode == MTX_POSVEC) g.vec = m; }
            break;
        }
        case 0x18: case 0x19: case 0x1A: {                       // MTX_MULT_4x4 / 4x3 / 3x3
            Mat m;
            if (cmd == 0x1A) m = mat_3x3(p);
            else load_mtx(m, p, cmd == 0x18 ? 16 : 12);
            if (g.mode == MTX_PROJ) g.proj = mul(m, g.proj);
            else if (g.mode == MTX_TEX) g.tex = mul(m, g.tex);
            else { g.pos = mul(m, g.pos); if (g.mode == MTX_POSVEC) g.vec = mul(m, g.vec); }
            break;
        }
        case 0x1B: {                                             // MTX_SCALE
            Mat m = Mat::identity();
            m.m[0] = static_cast<int32_t>(p[0]) * FX12;
            m.m[5] = static_cast<int32_t>(p[1]) * FX12;
            m.m[10] = static_cast<int32_t>(p[2]) * FX12;
            if (g.mode == MTX_PROJ) g.proj = mul(m, g.proj);
            else if (g.mode == MTX_TEX) g.tex = mul(m, g.tex);
            else g.pos = mul(m, g.pos);
            break;
        }
        case 0x1C: {                                             // MTX_TRANS
            Mat m = Mat::identity();
            m.m[12] = static_cast<int32_t>(p[0]) * FX12;
            m.m[13] = static_cast<int32_t>(p[1]) * FX12;
            m.m[14] = static_cast<int32_t>(p[2]) * FX12;
            if (g.mode == MTX_PROJ) g.proj = mul(m, g.proj);
            else if (g.mode == MTX_TEX) g.tex = mul(m, g.tex);
            else { g.pos = mul(m, g.pos); if (g.mode == MTX_POSVEC) g.vec = mul(m, g.vec); }
            break;
        }
        case 0x20: g.color = bgr555_to_argb(static_cast<uint16_t>(p[0] & 0x7FFF)); break;
        case 0x21: {                                             // NORMAL
            // 3 x 10-bit signed, 1.9 fixed point.
            auto n10 = [](uint32_t v) {
                return (static_cast<int32_t>(v << 22) >> 22) / 512.0f;
            };
            float nx = n10(p[0] & 0x3FF);
            float ny = n10((p[0] >> 10) & 0x3FF);
            float nz = n10((p[0] >> 20) & 0x3FF);
            if (g.texmode == 2) {
                // normal source -- sphere/environment maps (Peach's crown):
                // the model-space normal through the texture matrix, added
                // to the last TEXCOORD
                const Mat &t = g.tex;
                g.u = (nx * t.m[0] + ny * t.m[4] + nz * t.m[8]) / 16.0f + g.s0;
                g.v = (nx * t.m[1] + ny * t.m[5] + nz * t.m[9]) / 16.0f + g.t0;
            }

            // Normals are transformed by the directional matrix, not position.
            const Mat &v = g.vec;
            const float tx = nx * v.m[0] + ny * v.m[4] + nz * v.m[8];
            const float ty = nx * v.m[1] + ny * v.m[5] + nz * v.m[9];
            const float tz = nx * v.m[2] + ny * v.m[6] + nz * v.m[10];
            const float len = std::sqrt(tx * tx + ty * ty + tz * tz);
            nx = len > 1e-6f ? tx / len : 0;
            ny = len > 1e-6f ? ty / len : 0;
            nz = len > 1e-6f ? tz / len : 1;

            float c[3] = {g.emission[0], g.emission[1], g.emission[2]};
            for (int i = 0; i < 4; ++i) {
                if (!((g.light_mask >> i) & 1)) continue;
                const State::Light &L = g.lights[i];
                // GBATEK: diffuse level is max(0, -dot(light_vector, normal)).
                float d = -(L.dx * nx + L.dy * ny + L.dz * nz);
                if (d < 0) d = 0;
                const float lc[3] = {L.r, L.g, L.b};
                for (int k = 0; k < 3; ++k)
                    c[k] += g.diffuse[k] * lc[k] * d + g.ambient[k] * lc[k];
            }
            auto ch = [](float f) {
                const int i = static_cast<int>(f * 255.0f + 0.5f);
                return static_cast<uint32_t>(i < 0 ? 0 : (i > 255 ? 255 : i));
            };
            g.color = 0xFF000000u | (ch(c[0]) << 16) | (ch(c[1]) << 8) | ch(c[2]);
            break;
        }
        case 0x22:                                               // TEXCOORD (1.4 fx)
            g.s0 = static_cast<int16_t>(p[0] & 0xFFFF) / 16.0f;
            g.t0 = static_cast<int16_t>(p[0] >> 16) / 16.0f;
            if (g.texmode == 1) {
                // TexCoord source: (S, T, 1/16, 1/16) through the texture matrix
                const Mat &t = g.tex;
                g.u = g.s0 * t.m[0] + g.t0 * t.m[4] + (t.m[8] + t.m[12]) / 16.0f;
                g.v = g.s0 * t.m[1] + g.t0 * t.m[5] + (t.m[9] + t.m[13]) / 16.0f;
            } else {
                g.u = g.s0;
                g.v = g.t0;
            }
            break;
        case 0x23:                                               // VTX_16
            vertex(static_cast<int16_t>(p[0] & 0xFFFF), static_cast<int16_t>(p[0] >> 16),
                   static_cast<int16_t>(p[1] & 0xFFFF));
            break;
        case 0x24: {                                             // VTX_10  (s4.6 -> 4.12)
            auto s10 = [](uint32_t v) { return static_cast<int16_t>((static_cast<int32_t>(v << 22) >> 22) << 6); };
            vertex(s10(p[0] & 0x3FF), s10((p[0] >> 10) & 0x3FF), s10((p[0] >> 20) & 0x3FF));
            break;
        }
        case 0x25: vertex(static_cast<int16_t>(p[0] & 0xFFFF), static_cast<int16_t>(p[0] >> 16), g.vz); break;
        case 0x26: vertex(static_cast<int16_t>(p[0] & 0xFFFF), g.vy, static_cast<int16_t>(p[0] >> 16)); break;
        case 0x27: vertex(g.vx, static_cast<int16_t>(p[0] & 0xFFFF), static_cast<int16_t>(p[0] >> 16)); break;
        case 0x28: {                                             // VTX_DIFF
            auto d10 = [](uint32_t v) { return static_cast<int32_t>(v << 22) >> 22; };
            vertex(static_cast<int16_t>(g.vx + d10(p[0] & 0x3FF)),
                   static_cast<int16_t>(g.vy + d10((p[0] >> 10) & 0x3FF)),
                   static_cast<int16_t>(g.vz + d10((p[0] >> 20) & 0x3FF)));
            break;
        }
        case 0x29: g.attr_pending = p[0]; g.attr_seen = true; break;   // POLYGON_ATTR
        case 0x2A: gx_teximage_param(p[0]); break;               // TEXIMAGE_PARAM
        case 0x2B: gx_pltt_base(p[0]); break;                    // PLTT_BASE
        case 0x30: {                                             // DIF_AMB
            auto unpack = [](uint32_t v, float *o) {
                o[0] = (v & 0x1F) / 31.0f;
                o[1] = ((v >> 5) & 0x1F) / 31.0f;
                o[2] = ((v >> 10) & 0x1F) / 31.0f;
            };
            unpack(p[0] & 0x7FFF, g.diffuse);
            unpack((p[0] >> 16) & 0x7FFF, g.ambient);
            // Bit 15 sets the vertex colour to the diffuse colour immediately,
            // which is what an unlit polygon then draws with.
            if ((p[0] >> 15) & 1) {
                auto ch = [](float f) { return static_cast<uint32_t>(f * 255.0f + 0.5f); };
                g.color = 0xFF000000u | (ch(g.diffuse[0]) << 16)
                          | (ch(g.diffuse[1]) << 8) | ch(g.diffuse[2]);
            }
            break;
        }
        case 0x31: {                                             // SPE_EMI
            g.emission[0] = ((p[0] >> 16) & 0x1F) / 31.0f;
            g.emission[1] = ((p[0] >> 21) & 0x1F) / 31.0f;
            g.emission[2] = ((p[0] >> 26) & 0x1F) / 31.0f;
            break;
        }
        case 0x32: {                                             // LIGHT_VECTOR
            const int i = (p[0] >> 30) & 3;
            auto n10 = [](uint32_t v) {
                return (static_cast<int32_t>(v << 22) >> 22) / 512.0f;
            };
            // The DS turns the light vector by the directional matrix
            // when it is set (GBATEK: "light vector * directional matrix").
            const float lx = n10(p[0] & 0x3FF), ly = n10((p[0] >> 10) & 0x3FF),
                        lz = n10((p[0] >> 20) & 0x3FF);
            const Mat &v = g.vec;
            g.lights[i].dx = lx * v.m[0] + ly * v.m[4] + lz * v.m[8];
            g.lights[i].dy = lx * v.m[1] + ly * v.m[5] + lz * v.m[9];
            g.lights[i].dz = lx * v.m[2] + ly * v.m[6] + lz * v.m[10];
            break;
        }
        case 0x33: {                                             // LIGHT_COLOR
            const int i = (p[0] >> 30) & 3;
            g.lights[i].r = (p[0] & 0x1F) / 31.0f;
            g.lights[i].g = ((p[0] >> 5) & 0x1F) / 31.0f;
            g.lights[i].b = ((p[0] >> 10) & 0x1F) / 31.0f;
            break;
        }
        case 0x34: break;                                        // SHININESS
        case 0x40:                                               // BEGIN_VTXS
            g.prim = p[0] & 3;
            if (g.attr_seen) {
                g.attr = g.attr_pending;
                g.light_mask = g.attr & 0xF;
            }
            g.strip.clear();
            g.strip_parity = 0;
            break;
        case 0x41: g.prim = -1; g.strip.clear(); break;          // END_VTXS
        case 0x50:                                               // SWAP_BUFFERS
            ++g_stats.swaps;
            drop_old_retired();
            g_stats.last_shown = static_cast<uint32_t>(g.tris.size());
            g.shown.swap(g.tris);
            g.tris.clear();
            g.swapped = true;
            break;
        case 0x60:                                               // VIEWPORT
            g.vp_x = p[0] & 0xFF;
            g.vp_y = (p[0] >> 8) & 0xFF;
            g.vp_w = ((p[0] >> 16) & 0xFF) - g.vp_x + 1;
            g.vp_h = ((p[0] >> 24) & 0xFF) - g.vp_y + 1;
            break;
        default: break;
    }
}

// GBATEK parameter counts, indexed by command byte.
int param_count(uint8_t cmd) {
    switch (cmd) {
        case 0x00: case 0x11: case 0x15: case 0x41: return 0;
        case 0x16: case 0x18: return 16;
        case 0x17: case 0x19: return 12;
        case 0x1A: return 9;
        case 0x1B: case 0x1C: case 0x70: return 3;
        case 0x23: case 0x71: return 2;
        case 0x34: return 32;
        default: return 1;
    }
}

// --- packed FIFO state machine ---------------------------------------------
uint8_t g_queue[4];
int g_queued = 0, g_qpos = 0;
uint32_t g_params[32];
int g_have = 0;

void feed(uint32_t word) {
    if (g_queued == 0) {                       // expecting a command word
        for (int i = 0; i < 4; ++i) g_queue[i] = static_cast<uint8_t>(word >> (i * 8));
        g_queued = 4;
        g_qpos = 0;
        g_have = 0;
        // Commands taking no parameters execute immediately.
        while (g_qpos < g_queued && param_count(g_queue[g_qpos]) == 0) {
            exec(g_queue[g_qpos], nullptr, 0);
            ++g_qpos;
        }
        if (g_qpos >= g_queued) g_queued = 0;
        return;
    }
    const uint8_t cmd = g_queue[g_qpos];
    g_params[g_have++] = word;
    if (g_have >= param_count(cmd)) {
        exec(cmd, g_params, g_have);
        g_have = 0;
        ++g_qpos;
        while (g_qpos < g_queued && param_count(g_queue[g_qpos]) == 0) {
            exec(g_queue[g_qpos], nullptr, 0);
            ++g_qpos;
        }
        if (g_qpos >= g_queued) g_queued = 0;
    }
}

// --- direct port writes -----------------------------------------------------
uint8_t g_port_cmd = 0;
uint32_t g_port_params[32];
int g_port_have = 0;

}  // namespace

void gx_stream_note(uint32_t w);
void gx_write_fifo(uint32_t word) { gx_stream_note(word); feed(word); }

void gx_set_matrix_slot(int slot, const float m[16]) {
    if (slot < 0 || slot >= 32) return;
    Mat mm;
    for (int i = 0; i < 16; ++i) mm.m[i] = m[i];
    g.pos_stack[slot] = mm;
    g.vec_stack[slot] = mm;
}

void gx_set_material(uint32_t dif_amb, uint32_t spe_emi) {
    exec(0x30, &dif_amb, 1);
    exec(0x31, &spe_emi, 1);
}

void gx_set_light(int index, float dx, float dy, float dz, uint32_t bgr555) {
    if (index < 0 || index > 3) return;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len > 1e-6f) { dx /= len; dy /= len; dz /= len; }
    g.lights[index] = {dx, dy, dz,
                       (bgr555 & 0x1F) / 31.0f, ((bgr555 >> 5) & 0x1F) / 31.0f,
                       ((bgr555 >> 10) & 0x1F) / 31.0f};
}

void gx_enable_lights(uint32_t mask) { g.light_mask = mask & 0xF; }

void gx_bind_texture(const uint32_t *rgba, int width, int height) {
    g.tex_wrap = 3;     // harness textures repeat; the VRAM path sets its own below
    g.tex_rgba = rgba;
    g.tw = width;
    g.th = height;
}

// --- VRAM-sourced texturing: the game path ----------------------------------
// The game binds textures by writing TEXIMAGE_PARAM / PLTT_BASE around each
// display list; the texel data was already uploaded through GX::LoadTex into
// the mapped texture-slot window and palettes through GX::LoadTexPltt. On
// hardware the slot addresses come from bank assignment; the port's HAL
// points the game's upload-base globals at these same windows, so a slot
// offset here is a plain host address.
namespace {

constexpr uintptr_t TEX_SLOT_BASE  = 0x06800000u;   // texture slots, mapped
constexpr uintptr_t PLTT_SLOT_BASE = 0x06880000u;   // palette slots, mapped

uint32_t g_teximage, g_plttbase;
// Decoded textures are shared: the cache holds one reference, and a frame
// handed to another thread to draw (gx_capture_frame) holds its own, so a
// cache clear -- every VRAMCNT store -- cannot free texels a triangle still
// points at. Cleared entries wait in g_retired until a few swaps have passed
// (the triangle lists of those frames point at them too).
using TexData = std::shared_ptr<const std::vector<uint32_t>>;
std::map<uint64_t, TexData> g_vram_tex_cache;
std::vector<std::pair<uint32_t, TexData>> g_retired;     // (swap count when retired, texels)
size_t g_tex_cache_bytes;                                 // decoded texels the cache holds
void retire_all() {
    for (auto &e : g_vram_tex_cache) g_retired.emplace_back(g_stats.swaps, e.second);
    g_vram_tex_cache.clear();
    g_tex_cache_bytes = 0;
}
void drop_old_retired() {
    size_t k = 0;
    for (auto &e : g_retired)
        if (g_stats.swaps - e.first < 4) g_retired[k++] = std::move(e);
    g_retired.resize(k);
}

void bind_from_vram() {
    const uint32_t fmt = (g_teximage >> 26) & 7;
    if (fmt == 0) { gx_bind_texture(nullptr, 0, 0); return; }
    const uint64_t key = (static_cast<uint64_t>(g_plttbase) << 32) | g_teximage;
    auto it = g_vram_tex_cache.find(key);
    if (it == g_vram_tex_cache.end()) {
        TextureDesc d;
        const uint32_t off = (g_teximage & 0xFFFF) << 3;
        d.width = 8 << ((g_teximage >> 20) & 7);
        d.height = 8 << ((g_teximage >> 23) & 7);
        d.format = static_cast<int>(fmt);
        d.color0_transparent = (g_teximage >> 29) & 1;
        // The bank VRAMCNT gives that texture slot (port/ntr/vram.cpp); the
        // flat slot window when none is mapped (the smoke harnesses).
        uint32_t avail = 0;
        if (const uint32_t a = vram_tex_addr(off, &avail)) {
            d.data = host_ptr_t<const uint8_t>(a);
            d.data_len = static_cast<int32_t>(avail);
        } else if (vram_3d_banks(false)) {
            // a slot no bank holds: the DS reads nothing there -- no texture
            d.data = nullptr;
            d.data_len = 0;
        } else {
            d.data = host_ptr_t<const uint8_t>(TEX_SLOT_BASE + off);
            d.data_len = off < 0x80000 ? 0x80000 - static_cast<int32_t>(off) : 0;
        }
        // Format 5 keeps its 4x4 palette-index words in slot 1, at half the
        // block offset (GBATEK); the game uploads them right after the blocks.
        if (fmt == 5) {
            uint32_t ia = 0, iav = 0;
            if ((ia = vram_tex_addr(0x20000 + off / 2, &iav))) {
                d.index = host_ptr_t<const uint8_t>(ia);
                d.index_len = static_cast<int32_t>(iav);
            } else if (vram_3d_banks(false)) {
                d.index = nullptr;
                d.index_len = 0;
            } else {
                d.index = host_ptr_t<const uint8_t>(TEX_SLOT_BASE + 0x20000 + off / 2);
                d.index_len = off / 2 < 0x20000 ? 0x20000 - static_cast<int32_t>(off / 2) : 0;
            }
        }
        const uint32_t pal_off = g_plttbase * (fmt == 2 ? 8u : 16u);
        uint32_t pav = 0;
        if (const uint32_t pa = vram_pltt_addr(pal_off, &pav)) {
            d.pal = host_ptr_t<const uint8_t>(pa);
            d.pal_len = static_cast<int32_t>(pav);
        } else if (vram_3d_banks(true)) {
            // a palette slot no bank holds: every colour reads as 0
            d.pal = nullptr;
            d.pal_len = 0;
        } else {
            d.pal = host_ptr_t<const uint8_t>(PLTT_SLOT_BASE + pal_off);
            d.pal_len = pal_off < 0x18000 ? 0x18000 - static_cast<int32_t>(pal_off) : 0;
        }
        std::vector<uint32_t> rgba;
        if (!texture_decode(d, rgba)) { gx_bind_texture(nullptr, 0, 0); return; }
        // The cache only empties at a VRAMCNT store; a scene that walks
        // through many palette or texture offsets meanwhile (an explosion's
        // particles) would grow it without end. Past a bound, start again:
        // the triangles already built keep theirs through g_retired.
        g_tex_cache_bytes += rgba.size() * 4;
        if (g_vram_tex_cache.size() >= 768 || g_tex_cache_bytes > (24u << 20)) {
            retire_all();
            g_tex_cache_bytes = rgba.size() * 4;
        }
        it = g_vram_tex_cache.emplace(key, std::make_shared<const std::vector<uint32_t>>(std::move(rgba))).first;
    }
    const int w = 8 << ((g_teximage >> 20) & 7), h = 8 << ((g_teximage >> 23) & 7);
    gx_bind_texture(it->second->data(), w, h);
    g.tex_wrap = static_cast<uint8_t>((g_teximage >> 16) & 0xF);
}

}  // namespace

void gx_teximage_param(uint32_t v) {
    g_teximage = v;
    g.texmode = static_cast<int>(v >> 30);
    bind_from_vram();
}
// The decoded-texture cache is keyed by slot offsets: a bank remap makes it stale.
void gx_texture_cache_clear() { retire_all(); }
void gx_pltt_base(uint32_t v) { g_plttbase = v; bind_from_vram(); }

// Diagnostic: a running hash of every word entering the engine, so a smoke
// can tell "the game emitted a different stream" from "the decode ignored
// it". Reset returns the previous value.
static uint32_t g_stream_hash = 2166136261u;
void gx_stream_note(uint32_t w) { g_stream_hash = (g_stream_hash ^ w) * 16777619u; }
uint32_t gx_stream_hash_reset()
{
    const uint32_t h = g_stream_hash;
    g_stream_hash = 2166136261u;
    return h;
}

int gx_store_count_reset() { int n = g_store_count; g_store_count = 0; return n; }

uint32_t gx_state_hash()
{
    uint32_t h = 2166136261u;
    const unsigned char *b = reinterpret_cast<const unsigned char *>(&g.pos);
    for (size_t i = 0; i < sizeof(Mat); ++i) h = (h ^ b[i]) * 16777619u;
    b = reinterpret_cast<const unsigned char *>(g.pos_stack);
    for (size_t i = 0; i < sizeof(Mat) * 4; ++i) h = (h ^ b[i]) * 16777619u;
    return h;
}

void gx_write_port(uint32_t addr, uint32_t value) {
    gx_stream_note(addr ^ value);
    const uint8_t cmd = static_cast<uint8_t>((addr - 0x04000400u) >> 2);
    const int need = param_count(cmd);
    if (need <= 1) {
        exec(cmd, &value, 1);
        return;
    }
    // Multi-parameter ports are written repeatedly; accumulate until satisfied.
    if (cmd != g_port_cmd) { g_port_cmd = cmd; g_port_have = 0; }
    if (g_port_have < 32) g_port_params[g_port_have++] = value;
    if (g_port_have >= need) {
        exec(cmd, g_port_params, g_port_have);
        g_port_have = 0;
        g_port_cmd = 0;
    }
}

void gx_reset() {
    g = State{};
    // VRAM-decoded texture cache: uploads can be replaced between scenes
    // (the soak reuses slot offsets per model), so stale entries must go.
    g_vram_tex_cache.clear();
    g_tex_cache_bytes = 0;
    g_teximage = g_plttbase = 0;
    // The stack slots must start as identity, not zero. dExtSimpleModel_c display lists open
    // with MTX_RESTORE against a slot the *scene* filled in earlier; rendering a
    // model on its own would otherwise load an all-zero matrix and collapse every
    // vertex onto the origin -- geometry decodes fine and nothing draws.
    for (int i = 0; i < 32; ++i) {
        g.pos_stack[i] = Mat::identity();
        g.vec_stack[i] = Mat::identity();
    }
    g.proj_stack[0] = g.proj_stack[1] = Mat::identity();
    g_queued = g_qpos = g_have = 0;
    g_port_cmd = 0; g_port_have = 0;
}

const GxTriangle *gx_polygons(size_t &count) {
    const std::vector<GxTriangle> &v = g.swapped ? g.shown : g.tris;
    count = v.size();
    return v.empty() ? nullptr : v.data();
}

bool g_gx_debug_nodepth;     // tools: draw in submission order, no depth test

namespace {
// `layer`: the picture as the 2D engine receives it (gx_render_layer) --
// cleared to CLEAR_COLOR with its alpha, each pixel's alpha (0..31) in the top
// byte, translucent polygons over the clear colour keeping their own alpha,
// shadow polygons left out (they only mask), the alpha test applied.
// Otherwise the older debug picture gx_render has always drawn.
std::mutex g_render_lock;       // the depth buffer is static: one render at a time

void render_impl(Framebuffer &fb, bool layer, const std::vector<GxTriangle> &tris, uint16_t disp3d,
                 unsigned alpha_ref_reg, uint32_t clear) {
    std::lock_guard<std::mutex> hold(g_render_lock);
    static float depth[SCREEN_H][SCREEN_W];
    for (int y = 0; y < SCREEN_H; ++y)
        for (int x = 0; x < SCREEN_W; ++x) depth[y][x] = 1e30f;
    const unsigned alpha_ref = (disp3d & 4) ? alpha_ref_reg & 31 : 0;
    if (layer) {
        const uint32_t r = clear & 31, gg = (clear >> 5) & 31, b = (clear >> 10) & 31;
        const uint32_t px = ((clear >> 16) & 31) << 24 | (r << 3 | r >> 2) << 16 |
                            (gg << 3 | gg >> 2) << 8 | (b << 3 | b >> 2);
        for (int y = 0; y < SCREEN_H; ++y)
            for (int x = 0; x < SCREEN_W; ++x) fb.px[y][x] = px;
    }

    for (const GxTriangle &t : tris) {
        const GxVertex &a = t.v[0], &b = t.v[1], &c = t.v[2];
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-6f) continue;

        // Face culling per POLYGON_ATTR (bit 0 back faces, bit 1 front). Screen Y
        // is flipped relative to DS space, so a front face is clockwise here
        // (area < 0). Without culling the far side of a closed mesh can win the
        // depth test and draw through the near side (the ? block's question
        // mark read backwards until this existed).
        if (area > 0.0f ? !(t.faces & 1) : !(t.faces & 2)) continue;
        if (t.alpha == 0) continue;                   // wireframe: not drawn yet
        if (layer && ((t.attr >> 4) & 3) == 3) continue;  // shadow polygon: a mask, not drawn

        int minx = static_cast<int>(std::floor(std::fmin(a.x, std::fmin(b.x, c.x))));
        int maxx = static_cast<int>(std::ceil(std::fmax(a.x, std::fmax(b.x, c.x))));
        int miny = static_cast<int>(std::floor(std::fmin(a.y, std::fmin(b.y, c.y))));
        int maxy = static_cast<int>(std::ceil(std::fmax(a.y, std::fmax(b.y, c.y))));
        if (minx < 0) minx = 0;
        if (miny < 0) miny = 0;
        if (maxx > SCREEN_W - 1) maxx = SCREEN_W - 1;
        if (maxy > SCREEN_H - 1) maxy = SCREEN_H - 1;

        // The three edge functions, linear in the pixel centre (px, py) and
        // already divided by the area: one multiply-free step per pixel
        // instead of three divisions.
        // Perspective-correct texture coordinates: u/w, v/w and 1/w are what
        // is linear across the screen. (A flat projection has every w equal
        // and this reduces to the plain interpolation.)
        const float ia = a.w > 1e-6f ? 1.0f / a.w : 1.0f, ib = b.w > 1e-6f ? 1.0f / b.w : 1.0f,
                    ic = c.w > 1e-6f ? 1.0f / c.w : 1.0f;
        const float inv = 1.0f / area;
        const float e0x = -(b.y - a.y) * inv, e0y = (b.x - a.x) * inv;
        const float e0c = ((b.y - a.y) * a.x - (b.x - a.x) * a.y) * inv;
        const float e1x = -(c.y - b.y) * inv, e1y = (c.x - b.x) * inv;
        const float e1c = ((c.y - b.y) * b.x - (c.x - b.x) * b.y) * inv;
        const float e2x = -(a.y - c.y) * inv, e2y = (a.x - c.x) * inv;
        const float e2c = ((a.y - c.y) * c.x - (a.x - c.x) * c.y) * inv;
        for (int y = miny; y <= maxy; ++y) {
            const float py = y + 0.5f, px0 = minx + 0.5f;
            float w0 = e0x * px0 + e0y * py + e0c;
            float w1 = e1x * px0 + e1y * py + e1c;
            float w2 = e2x * px0 + e2y * py + e2c;
            for (int x = minx; x <= maxx; ++x, w0 += e0x, w1 += e1x, w2 += e2x) {
                // Accept either winding; back-face culling is a POLYGON_ATTR job.
                // (area > 0 or < 0 flips the signs of all three together, and
                // dividing by it already made inside >= 0.)
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float l0 = w1, l1 = w2, l2 = w0;   // barycentric for a, b, c
                const float z = l0 * a.z + l1 * b.z + l2 * c.z;
                if (!g_gx_debug_nodepth && z >= depth[y][x]) continue;
                // Depth is written only after the texel passes the alpha test
                // below -- a transparent texel must not occlude what is behind it.
                // Texture first; the vertex colour modulates it. UVs are
                // interpolated linearly -- correct here because the projection
                // is affine (w == 1); perspective-correct division belongs with
                // a real perspective matrix.
                uint32_t texel = 0xFFFFFFFFu;
                if (t.tex && t.tw > 0 && t.th > 0) {
                    const float q0 = l0 * ia, q1 = l1 * ib, q2 = l2 * ic;
                    const float iq = 1.0f / (q0 + q1 + q2);
                    const float uu = (q0 * a.u + q1 * b.u + q2 * c.u) * iq;
                    const float vv = (q0 * a.v + q1 * b.v + q2 * c.v) * iq;
                    // repeat, mirrored repeat (flip) or clamp, per axis
                    auto wrapc = [](int c, int n, bool rep, bool flip) {
                        if (!rep) return c < 0 ? 0 : (c >= n ? n - 1 : c);
                        if (flip) {
                            int m = c % (2 * n);
                            if (m < 0) m += 2 * n;
                            return m < n ? m : 2 * n - 1 - m;
                        }
                        int m = c % n;
                        return m < 0 ? m + n : m;
                    };
                    const int ui = wrapc(static_cast<int>(std::floor(uu)), t.tw, t.wrap & 1, t.wrap & 4);
                    const int vi = wrapc(static_cast<int>(std::floor(vv)), t.th, t.wrap & 2, t.wrap & 8);
                    texel = t.tex[vi * t.tw + ui];
                    if ((texel >> 24) == 0) continue;      // transparent texel
                }

                // Round, do not truncate: barycentrics sum to 0.9999 rather than
                // exactly 1, and a truncating cast bands a flat surface 255/254.
                auto ch = [&](int sh) {
                    const float v = l0 * ((a.color >> sh) & 0xFF)
                                    + l1 * ((b.color >> sh) & 0xFF)
                                    + l2 * ((c.color >> sh) & 0xFF);
                    const float m = ((texel >> sh) & 0xFF) / 255.0f;
                    const int i = static_cast<int>(v * m + 0.5f);
                    return static_cast<uint32_t>(i < 0 ? 0 : (i > 255 ? 255 : i));
                };
                const uint32_t src = (ch(16) << 16) | (ch(8) << 8) | ch(0);
                if (layer) {
                    // the pixel's alpha: the polygon's, times the texel's
                    unsigned a = t.alpha;
                    if (t.tex) a = (((a + 1) * ((texel >> 27) + 1)) - 1) >> 5;
                    if (a == 0 || ((disp3d & 4) && a <= alpha_ref)) continue;
                    const uint32_t dst = fb.px[y][x];
                    const unsigned da = dst >> 24;
                    if (a >= 31) {
                        depth[y][x] = z;
                        fb.px[y][x] = 31u << 24 | src;
                    } else if (da == 0 || !(disp3d & 8)) {
                        if (t.attr & 0x800) depth[y][x] = z;
                        fb.px[y][x] = a << 24 | src;
                    } else {
                        if (t.attr & 0x800) depth[y][x] = z;
                        uint32_t out = (a > da ? a : da) << 24;
                        for (int sh = 0; sh < 24; sh += 8) {
                            const uint32_t sc = (src >> sh) & 0xFF, dc = (dst >> sh) & 0xFF;
                            out |= ((sc * (a + 1) + dc * (31 - a)) >> 5) << sh;
                        }
                        fb.px[y][x] = out;
                    }
                    continue;
                }
                if (t.alpha >= 31) {
                    depth[y][x] = z;
                    fb.px[y][x] = 0xFF000000u | src;
                } else {
                    // translucent: blended over what is there, depth kept
                    const uint32_t dst = fb.px[y][x];
                    const uint32_t a = t.alpha + 1;
                    uint32_t out = 0xFF000000u;
                    for (int sh = 0; sh < 24; sh += 8) {
                        const uint32_t sc = (src >> sh) & 0xFF, dc = (dst >> sh) & 0xFF;
                        out |= ((sc * a + dc * (32 - a)) >> 5) << sh;
                    }
                    fb.px[y][x] = out;
                }
            }
        }
    }
}

}  // namespace

namespace {
void render_now(Framebuffer &fb, bool layer) {
    render_impl(fb, layer, g.swapped ? g.shown : g.tris,
                static_cast<uint16_t>(io_read(0x04000060u, 2)),
                static_cast<unsigned>(io_read(0x04000340u, 1)),
                static_cast<uint32_t>(io_read(0x04000350u, 4)));
}
}  // namespace
void gx_render(Framebuffer &fb) { render_now(fb, false); }
void gx_render_layer(Framebuffer &fb) { render_now(fb, true); }

void gx_capture_frame(GxFrame &out) {
    out.tris = g.swapped ? g.shown : g.tris;
    out.keep.clear();
    for (const auto &e : g_vram_tex_cache) out.keep.push_back(e.second);
    for (const auto &e : g_retired) out.keep.push_back(e.second);
    out.disp3d = static_cast<uint16_t>(io_read(0x04000060u, 2));
    out.alpha_ref = static_cast<uint8_t>(io_read(0x04000340u, 1));
    out.clear = static_cast<uint32_t>(io_read(0x04000350u, 4));
}

void gx_render_frame(const GxFrame &frame, Framebuffer &fb) {
    render_impl(fb, true, frame.tris, frame.disp3d, frame.alpha_ref, frame.clear);
}

void gx_debug_stats(uint32_t out[6]) {
    out[0] = g_stats.cmds;
    out[1] = g_stats.vtx;
    out[2] = g_stats.swaps;
    out[3] = g_stats.last_shown;
    out[4] = static_cast<uint32_t>(g.tris.size());
    out[5] = g_stats.proj_loads;
}

void gx_record_start(size_t max_words) {
    g_rec.clear();
    g_rec.reserve(max_words < (1u << 20) ? max_words : (1u << 20));
    g_rec_cap = max_words;
    g_recording = true;
}
void gx_record_stop() { g_recording = false; }
const uint32_t *gx_record_data(size_t &words) {
    words = g_rec.size();
    return g_rec.data();
}
void gx_ring_enable(size_t words) {
    std::free(g_ring);
    g_ring = nullptr;
    g_ring_pos = 0;
    g_ring_at = 0;
    g_ring_frames = 0;
    if (!words) return;
    g_ring = static_cast<uint32_t *>(std::malloc(words * 4));
    g_ring_n = g_ring ? words : 0;
    g_ring_starts[g_ring_frames++] = 0;
}
bool gx_ring_active() { return g_ring != nullptr; }
void gx_ring_mark(uint32_t tag) {
    if (!g_ring) return;
    ring_put(0x00 | 1u << 8);                 // a NOP with one word: the tag
    ring_put(tag);
}
size_t gx_ring_view(const uint32_t *part[2], size_t words[2], uint32_t &frames) {
    part[0] = part[1] = nullptr;
    words[0] = words[1] = 0;
    frames = 0;
    if (!g_ring) return 0;
    // the oldest frame start still whole in the buffer, up to now
    const uint64_t end = g_ring_pos, low = end > g_ring_n ? end - g_ring_n : 0;
    const uint32_t keep = g_ring_frames < 512 ? g_ring_frames : 512;
    uint64_t from = end;
    for (uint32_t k = keep; k-- > 0;) {
        const uint64_t s = g_ring_starts[(g_ring_frames - 1 - k) % 512];
        if (s >= low) { from = s; frames = k; break; }       // whole frames
    }
    // in place, as one or two runs of the circular buffer
    const uint64_t a = from % g_ring_n, n = end - from;
    part[0] = g_ring + a;
    words[0] = (size_t)(a + n <= g_ring_n ? n : g_ring_n - a);
    if (words[0] < n) { part[1] = g_ring; words[1] = (size_t)(n - words[0]); }
    return (size_t)n;
}
void gx_replay(const uint32_t *w, size_t words) {
    size_t i = 0;
    while (i < words) {
        const uint8_t cmd = w[i] & 0xff;
        const int np = (w[i] >> 8) & 0xff;
        if (i + 1 + np > words) break;
        exec(cmd, w + i + 1, np);
        i += 1 + np;
    }
}

}  // namespace ntr
