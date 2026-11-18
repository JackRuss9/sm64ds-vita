// The live view on the GPU (port/vita/vita_gl3d.cpp), as the drawing thread
// (port/vita/vita_display.cpp) calls it.
#pragma once
#include <stdint.h>

#include "ntr/gx.h"
#include "ntr/ppu.h"

/* What sm64ds_gl_present draws: the two engines' screens, where each goes,
   and how engine A's mixes in the 3D picture. */
struct GlScreens {
    const ntr::Framebuffer *a, *a_ctl, *b;   // a_ctl null: engine A is final as it is
    bool a_top;                              // engine A on the top screen
    const int *top_rect, *bottom_rect;       // {x, y, w, h}; a zero width hides one
    float mix_b[4], mix_m[4];                // the shader's u_b and u_m (kFsMix); mix_m[3] the
                                             // 2D picture's share of the width (0.75 widescreen)
    const uint8_t *text;                     // the frame-rate line, BGRA
    int text_w, text_h;
    const uint8_t *menu;                     // the settings menu, BGRA, or null
    bool menu_changed;                       // its picture is new
    int menu_w, menu_h;
    // Two-screen 3D (some minigames: engine A draws one screen a frame,
    // alternately, and the game captures it for engine B to show on the
    // other). hold: engine A's picture of this frame is also kept, on the
    // GPU; dual: the other screen shows the one kept last frame instead of
    // engine B's copy of it (which only the CPU could draw: 60 ms a frame,
    // and next to the GPU's picture it flickered).
    bool hold, dual;
    // with hold: the kept picture read back here, at the DS's resolution
    // (256x192, 0xAARRGGBB, top row first), right after it is drawn and
    // before the frame is shown; read_back_done says it was
    uint32_t *read_back;
    bool read_back_done;
};

extern "C" {
/* Once, on the drawing thread; false: the GPU path cannot run. */
bool sm64ds_gl_init(void);
/* The 3D picture of a frame at `scale` (1..4) times the DS's resolution,
   4/3 as wide when `wide`; read back into `fb` too when given (then at 1x,
   not wide). */
/* scale 0: drawn at native_w x native_h, the size the screen has on the
   display (1x when that is 0) */
void sm64ds_gl_render_3d(const ntr::GxFrame &frame, ntr::Framebuffer *fb, int scale, bool wide,
                         int native_w = 0, int native_h = 0);
/* For the debug log: GPU textures made and deleted since the last call. */
void sm64ds_gl_texture_counts(unsigned *made, unsigned *deleted, unsigned *cached);
/* Both screens and the frame-rate line, then shown. */
void sm64ds_gl_present(GlScreens &s);
}
