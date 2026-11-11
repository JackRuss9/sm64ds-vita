// The DS's buttons and touch screen, from the PS Vita's (gate 11 on).
//
// Buttons: the game reads KEYINPUT (0x04000130: A B Select Start Right Left
// Up Down R L, a 0 bit is a held key) and the word the ARM7 keeps at
// 0x027fffa8 (X bit 10, Y bit 11, the debug key 13, the lid 15 -- 1 closed).
// Host memory starts at zero, which reads as every key held: both are set
// to "nothing held, lid open" at boot, then from the Vita every frame.
//
//   DS  A B X Y        Vita  circle cross triangle square (the same places)
//   DS  L R            Vita  L R
//   DS  Start Select   Vita  Start Select
//   DS  d-pad          Vita  d-pad or the left stick
//
// Touch: the Vita's front panel, where the right half of the display shows
// the DS touch screen (port/vita/vita_display.cpp), becomes the ARM7's
// touch samples (port/vita/arm7_host.cpp). The game turns raw readings into
// dots with the calibration in the firmware's user settings (0x027ffc80 +
// 0x58), which the DS's loader leaves there; this writes one where raw =
// 16 x dot. The settings' language is set to English.
//
// The rear panel is not the DS's: a tap there asks for a capture (the
// screenshots and a display snapshot, tests/smoke_boot.cpp), unless the
// settings menu turned that off. D-pad Right + Square opens the settings
// menu, D-pad Left + Circle the cheats menu (vita_display.cpp); either has the
// buttons while open. The cheats are applied here.
#include <stdint.h>
#include <string.h>

#include "ntr/mmio.h"

#if defined(__vita__)
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#endif

extern "C" {
extern volatile int sm64ds_cheat_infinite_lives, sm64ds_cheat_moon_jump, sm64ds_cheat_infinite_power,
    sm64ds_cheat_infinite_breath, sm64ds_cheat_99_coins, sm64ds_cheat_god_mode;   /* vita_display.cpp */
extern signed char data_0209f2f4;      /* the lives counter */
extern void *data_0209f394[];          /* the players, by slot */
extern unsigned char gActivePlayerSlot;
extern short data_02092144[];          /* each player's power, 0x100 a wedge */
extern short data_0209f358[];          /* each player's coins */
extern int data_0209d50c;              /* the main loop's step: 7 while playing */
bool sm64ds_in_game_arena(const void *p);   /* arena_host.cpp: the game's heaps */
extern char _ZTV6Player[];             /* Player's vtable (its objects point 8 bytes in) */
void sm64ds_arm7_touch(unsigned x, unsigned y, bool down);     /* arm7_host.cpp */
bool sm64ds_display_to_touch(int vx, int vy, int *x, int *y);  /* vita_display.cpp */
bool sm64ds_menu_pad(uint32_t buttons, uint32_t dpad_buttons);  /* vita_display.cpp */
bool sm64ds_rear_capture_enabled(void);                        /* vita_display.cpp */
bool sm64ds_right_stick_camera(void);                          /* vita_display.cpp */
void sm64ds_savestate_request(int action);                     /* savestate_host.cpp */
extern unsigned char data_0209f4ae[];  /* the controller records' mode byte, 0x18 apart */
extern volatile int sm64ds_wide_view;  /* vita_display.cpp: the 3D drawn 16:9 */
extern unsigned char data_0209f43c[];  /* the view clipper (dClipper) */
void _ZN8dClipper13Func_0201559CEv(void *self);  /* dClipper: rebuild the frustum planes */
}

namespace {
constexpr uint32_t KEYINPUT = 0x04000130u, XY_BUF = 0x027fffa8u, USER = 0x027ffc80u;
constexpr uint16_t XY_NONE = 0x2c00;    // X, Y, debug released; lid open
bool g_rear_down, g_rear_tap;

void put16(uint32_t ds, uint16_t v) { *static_cast<volatile uint16_t *>(::ntr::host_ptr(ds)) = v; }

#if defined(__vita__)
/* The cheats menu's, once a frame on the game's turn, before it runs:
     infinite lives  the lives counter (data_0209f2f4, which GiveLives keeps
                     in 0..100) never under 4;
     infinite power  the power meter refilled to 8 whenever it drops (not
                     once it is empty: a fall out of the level still ends);
     infinite breath underwater the meter does not drain;
     99 coins        the coin count kept at 99 at least: the next coin is
                     the hundredth, and brings the level's 100-coin star;
     moon jump       holding jump (A: Circle) sends the player up, steadily:
                     its vertical speed at least 0x14000 (1/4096 units a
                     frame) -- both copies: mVertSpeed (dActor_c +0xa8) and
                     Player's mPrevVertSpeed (+0x640), which the air states
                     work from and copy back over mVertSpeed (setting only
                     the first did nothing);
     god mode        no damage: the invincibility a hit gives (mInvincibleTimer,
                     +0x6a0, counted down each frame) kept at 3 -- odd before
                     the frame, so even (drawn, not blinking) when the player
                     is drawn -- and the power meter full. A fall out of the
                     level still costs a life. */
void apply_cheats(uint32_t vita_buttons)
{
    if (sm64ds_cheat_infinite_lives && data_0209f2f4 < 4) data_0209f2f4 = 4;
    // the player in play: only while playing (main-loop step 7). Between
    // levels the slot can still hold the last level's player, already freed:
    // the breath cheat's store into it landed in the next level's data while
    // it loaded (the particle textures' table: "out of video memory" on
    // entering the castle).
    const unsigned slot = gActivePlayerSlot;
    char *player = (data_0209d50c == 7 && slot < 4) ? static_cast<char *>(data_0209f394[slot]) : nullptr;
    if (player && !sm64ds_in_game_arena(player)) player = nullptr;
    if (player && *reinterpret_cast<void **>(player) != _ZTV6Player + 8) player = nullptr;   // a live Player
    const unsigned no = player ? *reinterpret_cast<uint8_t *>(player + 0x6d8) : 0;   /* mPlayerNo */
    // power: the meter is data_02092144[player] / 0x100 wedges, 8 full
    if (sm64ds_cheat_infinite_power && player && no < 4 && data_02092144[no] < 0x800 && data_02092144[no] > 0)
        data_02092144[no] = 0x800;
    // breath: the water states count mAirDrainCounter (+0x6ca) up and take a
    // wedge each time it comes round; held at 0 it never does
    if (sm64ds_cheat_infinite_breath && player)
        *reinterpret_cast<uint16_t *>(player + 0x6ca) = 0;
    // coins: data_0209f358[player]; the next coin makes 100 and brings the
    // level's 100-coin star
    if (sm64ds_cheat_99_coins && player && no < 4 && data_0209f358[no] < 99) data_0209f358[no] = 99;
    if (sm64ds_cheat_god_mode && player) {
        uint16_t *inv = reinterpret_cast<uint16_t *>(player + 0x6a0);   /* mInvincibleTimer */
        if (*inv < 3) *inv = 3;
        if (no < 4 && data_02092144[no] < 0x800 && data_02092144[no] > 0) data_02092144[no] = 0x800;
    }
    if (sm64ds_cheat_moon_jump && (vita_buttons & SCE_CTRL_CIRCLE)) {
        if (player) {
            int32_t *vert = reinterpret_cast<int32_t *>(player + 0xa8);
            int32_t *prev = reinterpret_cast<int32_t *>(player + 0x640);   /* mPrevVertSpeed */
            if (*vert < 0x14000) *vert = 0x14000;
            if (*prev < 0x14000) *prev = 0x14000;
        }
    }
}
#endif
void put8(uint32_t ds, uint8_t v) { *static_cast<volatile uint8_t *>(::ntr::host_ptr(ds)) = v; }
}  // namespace

extern "C" {

void sm64ds_input_init(void)
{
    put16(KEYINPUT, 0x03ff);
    put16(XY_BUF, XY_NONE);
    // NVRAM user settings: touch calibration (raw x1 y1, dot x1 y1, raw x2 y2,
    // dot x2 y2) and the language (bits 0-2 at +0x64: 1 = English)
    put16(USER + 0x58, 32 * 16 + 8);
    put16(USER + 0x5a, 24 * 16 + 8);
    put8(USER + 0x5c, 32);
    put8(USER + 0x5d, 24);
    put16(USER + 0x5e, 224 * 16 + 8);
    put16(USER + 0x60, 168 * 16 + 8);
    put8(USER + 0x62, 224);
    put8(USER + 0x63, 168);
    put16(USER + 0x64, 1);
#if defined(__vita__)
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
#endif
}

/* Once a frame, on the host's turn. */
void sm64ds_input_frame(void)
{
#if defined(__vita__)
    SceCtrlData pad;
    memset(&pad, 0, sizeof pad);
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t b = pad.buttons;
    if (pad.lx < 64) b |= SCE_CTRL_LEFT;
    if (pad.lx > 192) b |= SCE_CTRL_RIGHT;
    if (pad.ly < 64) b |= SCE_CTRL_UP;
    if (pad.ly > 192) b |= SCE_CTRL_DOWN;
    static const struct { uint32_t vita; uint16_t ds; } kMap[] = {
        {SCE_CTRL_CIRCLE, 1u << 0},   {SCE_CTRL_CROSS, 1u << 1},  {SCE_CTRL_SELECT, 1u << 2},
        {SCE_CTRL_START, 1u << 3},    {SCE_CTRL_RIGHT, 1u << 4},  {SCE_CTRL_LEFT, 1u << 5},
        {SCE_CTRL_UP, 1u << 6},       {SCE_CTRL_DOWN, 1u << 7},
        {SCE_CTRL_RTRIGGER, 1u << 8}, {0x800u, 1u << 8},            // R1 (PS TV pads)
        {SCE_CTRL_LTRIGGER, 1u << 9}, {0x400u, 1u << 9},            // L1
    };
    uint16_t keys = 0x03ff, xy = XY_NONE;
    // the menus (settings: D-pad Right + Square, cheats: D-pad Left +
    // Circle) take the buttons while open
    const bool menu = sm64ds_menu_pad(b, pad.buttons);
    if (menu) b = 0;
    // save states: D-pad Up + R saves, D-pad Down + L loads (the physical
    // D-pad, as for the menus); the game does not see the pair
    {
        static bool save_held, load_held;
        const uint32_t raw = pad.buttons;
        const bool save = !menu && (raw & SCE_CTRL_UP) && (raw & (SCE_CTRL_RTRIGGER | 0x800u));
        const bool load = !menu && (raw & SCE_CTRL_DOWN) && (raw & (SCE_CTRL_LTRIGGER | 0x400u));
        if (save && !save_held) sm64ds_savestate_request(1);
        if (load && !load_held) sm64ds_savestate_request(2);
        save_held = save;
        load_held = load;
        if (save) b &= ~(uint32_t)(SCE_CTRL_UP | SCE_CTRL_RTRIGGER | 0x800u);
        if (load) b &= ~(uint32_t)(SCE_CTRL_DOWN | SCE_CTRL_LTRIGGER | 0x400u);
    }
    apply_cheats(b);
    // widescreen on or off: the view clipper's frustum rebuilt for the width
    // shown (the camera rebuilds it too, but not every frame)
    static int wide_applied = 0;
    if (sm64ds_wide_view != wide_applied) {
        wide_applied = sm64ds_wide_view;
        _ZN8dClipper13Func_0201559CEv(data_0209f43c);
    }
    for (const auto &m : kMap)
        if (b & m.vita) keys &= static_cast<uint16_t>(~m.ds);
    if (b & SCE_CTRL_TRIANGLE) xy &= static_cast<uint16_t>(~(1u << 10));
    if (b & SCE_CTRL_SQUARE) xy &= static_cast<uint16_t>(~(1u << 11));
    put16(KEYINPUT, keys);
    put16(XY_BUF, xy);

    SceTouchData t;
    memset(&t, 0, sizeof t);
    int x = 0, y = 0;
    bool down = false;
    if (!menu && sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) >= 0 && t.reportNum > 0)
        // the panel reports 1920x1088: half that is the display
        down = sm64ds_display_to_touch(t.report[0].x / 2, t.report[0].y / 2, &x, &y);
    // the right stick turns the camera: the game turns it a step at a time
    // when the camera arrows on the touch screen are tapped, so while the
    // stick is held over, they are -- a tap every 12 frames. The arrows'
    // place in the touch screen's lower right corner (dScStage_c::
    // CheckCameraInput: 0xa7..0xd0 left, 0xd6.. right, from y 0x9a). Not in
    // the touch control mode (the record's mode byte 1), which has no arrows
    // there; a real touch wins.
    static unsigned stick_frames;
    const int rx = pad.rx - 128;
    const bool stick = !menu && sm64ds_right_stick_camera() && (rx < -64 || rx > 64)
                       && data_0209f4ae[gActivePlayerSlot * 0x18] != 1;
    if (stick && !down) {
        const bool tap = (stick_frames++ % 12) < 6;
        if (tap) {
            x = rx < 0 ? 0xbc : 0xe8;
            y = 0xb0;
            down = true;
        }
    } else if (!stick) {
        stick_frames = 0;
    }
    sm64ds_arm7_touch((unsigned)x, (unsigned)y, down);

    memset(&t, 0, sizeof t);
    const bool rear = sceTouchPeek(SCE_TOUCH_PORT_BACK, &t, 1) >= 0 && t.reportNum > 0;
    if (rear && !g_rear_down && sm64ds_rear_capture_enabled()) g_rear_tap = true;
    g_rear_down = rear;
#endif
}

/* A tap on the rear panel since the last call. */
bool sm64ds_input_capture_requested(void)
{
    const bool r = g_rear_tap;
    g_rear_tap = false;
    return r;
}

}  // extern "C"
