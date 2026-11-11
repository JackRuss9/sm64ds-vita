// The game's ROM-resident globals on host: fill + pointer patching at startup.
//
// port/tools/romimage.py emits storage for every game global that no C source
// defines (tables, strings, pointer arrays the decomp still keeps as ROM
// bytes), plus two tables:
//
//   rim_fills    which DS range of which module (ARM9, DTCM, overlay N) each
//                storage holds;
//   rim_patches  every DS pointer inside that data, as (where in host
//                storage, host address of what it points to).
//
// sm64ds_romimage_load() reads the ARM9 and overlay images from the user's
// own .nds, copies each fill's bytes, then writes every patch. The tables
// carry no ROM bytes; the bytes never leave the user's dump.
#ifndef SM64DS_PORT_ROMIMAGE_RT_H
#define SM64DS_PORT_ROMIMAGE_RT_H

#include <stdint.h>

/* module: -1 ARM9 main, -2 ITCM, -3 DTCM, >= 0 overlay id */
struct RimFill {
    int module;
    uint32_t ds_addr;
    uint32_t size;
    unsigned char *dst;
};
struct RimPatch {
    unsigned char *at;          /* 32-bit slot in host storage */
    const void *target;         /* host address the DS pointer meant */
};

/* .bss storage, zeroed when its module (re)loads */
struct RimBss {
    int module;
    unsigned char *dst;
    uint32_t size;
};
/* a module's C++ static initializers (see romimage.py --out-init-ld) */
struct RimInit {
    int module;
    void (*const *start)(void);
    void (*const *end)(void);
};

/* A DS pointer into an address several overlays share (they load at the same
   place, one at a time): one entry per candidate. The DS reads whichever is
   resident; sm64ds_romimage_load_module() points the slot at the overlay
   loaded most recently. */
struct RimRetarget {
    unsigned char *at;          /* 32-bit slot in host storage */
    int overlay;                /* the candidate overlay */
    const void *target;         /* host address in that overlay */
};
extern const RimRetarget rim_retargets[];
extern const unsigned rim_retarget_count;

extern const RimFill rim_fills[];
extern const RimBss rim_bss[];
extern const unsigned rim_bss_count;
extern const RimInit rim_inits[];
extern const unsigned rim_init_count;
extern const unsigned rim_fill_count;
extern const RimPatch rim_patches[];
extern const unsigned rim_patch_count;

/* Fill and patch everything; false (with a message on stderr) if the ROM
   cannot be read. */
extern "C" bool sm64ds_romimage_load(const char *rom_path);

/* The DS loading module `module` (an overlay id; -1 the ARM9): its globals
   back to the ROM's bytes with pointers patched, its .bss zeroed, then its
   static initializers run -- what FS_LoadOverlay + FS_StartOverlay do on the
   DS. False if the ROM cannot be read. */
extern "C" bool sm64ds_romimage_load_module(int module);
/* Run only the static initializers of `module` (the ARM9's at boot). */
extern "C" void sm64ds_romimage_run_inits(int module);

#endif
