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
};

extern "C" {
/* Once, on the drawing thread; false: the GPU path cannot run. */
bool sm64ds_gl_init(void);
/* The 3D picture of a frame at `scale` (1..4) times the DS's resolution,
   4/3 as wide when `wide`; read back into `fb` too when given (then at 1x,
   not wide). */
void sm64ds_gl_render_3d(const ntr::GxFrame &frame, ntr::Framebuffer *fb, int scale, bool wide);
/* Both screens and the frame-rate line, then shown. */
void sm64ds_gl_present(const GlScreens &s);
}
