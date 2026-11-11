// DS 2D engine scan-out.
//
// The 2D engines are pure register state: DISPCNT and BGxCNT say where the
// tilemaps and tiles live, and VRAM holds them. Nothing about drawing is
// write-triggered, so the host can simply read the register file once per frame
// and rasterise -- no interception needed, which is why 68% of the decomp's
// hardware accesses need no more than a working memory map to behave correctly.
//
// See notes/assessment.md section 2a and docs/mmio-inventory.md.

#ifndef NTR_PPU_H
#define NTR_PPU_H

#include <stdint.h>

namespace ntr {

constexpr int SCREEN_W = 256;
constexpr int SCREEN_H = 192;

enum Engine { ENGINE_A = 0, ENGINE_B = 1 };

struct Framebuffer {
    uint32_t px[SCREEN_H][SCREEN_W];   // 0xAARRGGBB
};

// Rasterise one engine's current register state into fb. Text-mode BGs only for
// now; affine, bitmap, sprites and the blend/window units are not implemented.
void ppu_scanout(Engine eng, Framebuffer &fb);

// Rasterise one engine's OBJ layer (sprites) from its OAM + OBJ VRAM over
// whatever fb already holds. Plain and affine sprites, 16- and 256-color
// tiles, 1D mapping; bitmap OBJs, mosaic, windows and blending are not
// implemented. Transparent color-0 texels leave fb untouched.
void ppu_scanout_obj(Engine eng, Framebuffer &fb);

// The engine's master brightness (MASTER_BRIGHT, 0x0400006C / 0x0400106C)
// over a finished screen: mode 1 fades toward white, mode 2 toward black, by
// factor/16 (factor 0..16). The DS applies it last, after 2D, 3D and OBJ.
void ppu_master_brightness(Engine eng, Framebuffer &fb);
// BLDCNT's brightness effects (modes 2 up / 3 down, by BLDY/16) over the
// background result, before sprites are drawn on it: what a fade that
// targets the backgrounds and backdrop but not OBJ looks like. Applied only
// when every enabled background is a first target (the common fade setup);
// alpha blending (mode 1) is not done yet.
void ppu_bg_brightness_effect(Engine eng, Framebuffer &fb);
// The whole screen of one engine as the DS composes it (port/ntr/ppu_compose.cpp):
// every BG mode but the large bitmap, sprites of every kind, priorities,
// windows, alpha blending and brightness effects. `layer3d` is the 3D
// engine's picture (gx_render_layer: alpha 0..31 in the top byte) for engine A's
// BG0 when DISPCNT makes it the 3D layer; null leaves BG0 out. Master
// brightness is not applied: ppu_master_brightness does that after.
void ppu_compose(Engine eng, Framebuffer &fb, const Framebuffer *layer3d);
// The same from a copy of the display state, for drawing on another thread
// while the game runs on: `regions[r]` holds the bytes of DS region r
// (4 I/O, 5 palettes, 6 VRAM, 7 OAM) at their offsets. Calls are serialised.
void ppu_compose_from(Engine eng, Framebuffer &fb, const Framebuffer *layer3d,
                      const uint8_t *const regions[8]);
// The same, spread over threads: set the regions once, prepare each engine
// (its palettes), then compose ranges of lines anywhere. `part` (0..2) picks
// the line buffers: ranges that run at the same time need different parts.
// Not to be mixed with ppu_compose / ppu_compose_from while those run.
void ppu_compose_set_regions(const uint8_t *const regions[8]);
void ppu_compose_prepare(Engine eng);
//
// With `ctl` (engine A), the 3D layer is left to the GPU, which draws it at
// any resolution and mixes it in: `fb` gets each pixel as it is where the 3D
// picture has nothing (alpha 0), and `ctl` what the 3D picture does where it
// has something, as 0xCCRRGGBB -- RGB a 2D colour, CC a code:
//   bits 0-1  0: the 3D picture does not show here;
//             1: it is the top layer, over RGB (the next layer): its pixel
//                blends with RGB by its own alpha when bit 2 (RGB is a second
//                target), else gets the effect of BLDCNT when bit 3 (effects
//                on, 3D a first target) -- brightness, or alpha blending with
//                RGB (with bit 2) -- else shows as it is;
//             2: it is second, under RGB (the top layer), which blends with
//                it: RGB * wa/16 + 3D * wb/16, wa bits 2-6, wb the BLDALPHA
//                EVB when bit 7, else 16 - wa.
// Elsewhere the GPU uses BLDCNT's mode, EVA, EVB and EVY, as here.
void ppu_compose_lines(Engine eng, Framebuffer &fb, const Framebuffer *layer3d, int y_begin, int y_end, int part,
                       Framebuffer *ctl = nullptr);
// Master brightness from a given MASTER_BRIGHT value.
void ppu_apply_master_brightness(uint16_t master_bright, Framebuffer &fb);
// Debug output, so a frame can be inspected without a window yet.
bool ppu_write_bmp(const char *path, const Framebuffer &fb);

}  // namespace ntr

#endif  // NTR_PPU_H
