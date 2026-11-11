// DS geometry engine.
//
// Unlike the 2D engines, 3D is not readable register state -- it is a *stream*.
// Writing a geometry port or pushing a word into the FIFO at 0x4000400 executes a
// command, and the result exists only as the polygons it emitted. A host layer
// that only mirrors registers renders nothing here, which is why this is the one
// part of the port that cannot be done by mapping memory.
//
// SM64DS drives it both ways, so both are implemented:
//
//     *(volatile u32 *)0x4000400 = 0x1a;      // packed command word
//     Copy36Bytes(m, (int *)0x4000400);       // ...then its 9 parameters
//
//     DMAStartTransfer(ch, src, 0x4000400, 0x84400000 | (len >> 2));   // display list
//
// See notes/assessment.md section 2a and docs/mmio-inventory.md.

#ifndef NTR_GX_H
#define NTR_GX_H

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <vector>

#include "ntr/ppu.h"

namespace ntr {

// A vertex after the modelview+projection transform, in screen space.
struct GxVertex {
    float x, y, z, w;      // x/y in pixels, z in [0,1] after divide
    float u, v;            // texel coordinates (DS TEXCOORD is 1.4 fixed point)
    uint32_t color;        // 0xAARRGGBB
    float cx, cy, cz;      // clip-space position (with w above): what clipping works on
};

// The bound texture travels with the triangle. Material state is set by the
// game's C code around each display list, not inside it, so a triangle's
// texture is whatever was bound when it was assembled.
struct GxTriangle {
    GxVertex v[3];
    const uint32_t *tex;   // decoded RGBA, or null for untextured
    int tw, th;
    // From the POLYGON_ATTR in force at its BEGIN_VTXS (GBATEK 4000440h+0x29):
    // which faces draw (bit 0 back, bit 1 front) and alpha 0..31 (0 is
    // wireframe, 31 opaque). Harnesses that never send one get front + 31.
    uint8_t faces = 2, alpha = 31;
    uint32_t attr = 0x001F0080u;   // the whole POLYGON_ATTR word (mode bits 4-5, ID 24-29)
    // TEXIMAGE_PARAM bits 16-19: repeat S, repeat T, flip S, flip T (a
    // texture that neither repeats nor flips is clamped to its edge texels)
    uint8_t wrap = 3;
    // drawn after the opaque ones (alpha under 31, or an A3I5/A5I3 texture)
    bool translucent = false;
};

// Feed the engine. `gx_write_port` takes a geometry-port address in
// 0x4000440..0x40005FF; `gx_write_fifo` takes a word pushed at 0x4000400.
void gx_write_port(uint32_t addr, uint32_t value);
void gx_write_fifo(uint32_t word);

// Reset the command stream, matrix stacks and polygon list.
void gx_reset();

// Bind a decoded texture for subsequent geometry. Pass null for untextured.
void gx_bind_texture(const uint32_t *rgba, int width, int height);

// The game path: TEXIMAGE_PARAM / PLTT_BASE writes (ports 0x2A/0x2B) bind by
// decoding lazily out of the mapped VRAM texture/palette slot windows.
void gx_teximage_param(uint32_t value);
void gx_pltt_base(uint32_t value);

// Diagnostic stream hash: every word entering the engine since the last
// reset, so smokes can separate "different stream" from "ignored stream".
uint32_t gx_stream_hash_reset();
uint32_t gx_state_hash();  // pos matrix + first slots
int gx_store_count_reset();

// Preload a position-matrix stack slot. Display lists open with MTX_RESTORE
// against slots the *scene* filled in beforehand, so a model drawn on its own
// has to have them set up first. Row-major, row-vector convention.
void gx_set_matrix_slot(int slot, const float m[16]);

// Lighting. The DS computes a vertex's colour when it sees NORMAL, out of the
// current material and the four lights -- so lighting is not a rasteriser stage
// here, it happens during command decode, exactly as on hardware.
//
// Display lists carry NORMAL but never DIF_AMB or LIGHT_*: the material and the
// scene set those. Bind them before submitting geometry.
void gx_set_material(uint32_t dif_amb, uint32_t spe_emi);
void gx_set_light(int index, float dx, float dy, float dz, uint32_t bgr555);
void gx_enable_lights(uint32_t mask);           // bits 0..3

// Polygons emitted since the last gx_reset / SWAP_BUFFERS.
const GxTriangle *gx_polygons(size_t &count);
// Counters for a log: geometry commands run, vertex commands, SWAP_BUFFERS
// seen, triangles in the last swapped frame, triangles in the frame being built,
// projection-matrix loads (LOAD/MULT in projection mode).
void gx_debug_stats(uint32_t out[6]);
// Record every executed geometry command (up to max_words 32-bit words) for
// an offline replay of the exact 3D state: gx_reset() then gx_replay().
void gx_record_start(size_t max_words);
void gx_record_stop();
const uint32_t *gx_record_data(size_t &words);
void gx_replay(const uint32_t *words, size_t count);
// The rolling recorder: the same record, always on, kept for the last
// `words` words (0 turns it off and frees it); gx_ring_mark drops a tag (a
// NOP with one parameter word, harmless on replay) in the stream, and
// gx_ring_view points at the whole frames still held (from just after a
// SWAP_BUFFERS) up to now, `frames` of them, in place: one or two runs of
// the circular buffer, in order; it returns the words in all.
// Called (from whichever thread drops the last reference) as decoded texels
// are freed, with the address a triangle's `tex` pointed at: a cache keyed by
// that address must forget it, as the next texture may be decoded there.
void gx_set_texture_free_hook(void (*hook)(const uint32_t *texels));
void gx_ring_enable(size_t words);
bool gx_ring_active();
void gx_ring_mark(uint32_t tag);
size_t gx_ring_view(const uint32_t *part[2], size_t words[2], uint32_t &frames);

// Rasterise them into fb with a depth buffer. Does not clear fb -- the 3D layer
// composites over whatever the 2D engine already drew.
void gx_render(Framebuffer &fb);
// The 3D layer as the 2D engine receives it: cleared to CLEAR_COLOR, each
// pixel's alpha (0 transparent .. 31 opaque) in the top byte, translucency and
// the alpha test as the DS applies them. For ppu_compose.
void gx_render_layer(Framebuffer &fb);

// The same picture drawn elsewhere, later: gx_capture_frame copies what a
// render reads -- the shown triangle list, the 3D control registers, and a
// reference to every decoded texture those triangles may point at -- and
// gx_render_frame draws it. The game's thread captures (cheap); another
// thread renders while the game runs on.
struct GxFrame {
    std::vector<GxTriangle> tris;
    std::vector<std::shared_ptr<const std::vector<uint32_t>>> keep;
    uint16_t disp3d = 0;
    uint8_t alpha_ref = 0;
    uint32_t clear = 0;
};
void gx_capture_frame(GxFrame &out);
void gx_render_frame(const GxFrame &frame, Framebuffer &fb);

}  // namespace ntr

#endif  // NTR_GX_H
