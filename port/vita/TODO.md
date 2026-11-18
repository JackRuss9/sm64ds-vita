# Open issues (Vita port)

Bugs we've looked at but not solved yet, with what we know so far.

## 1. The drawing toy ("Mario drawing") canvas -- fixed (confirmed after 0109)
- **Seen:** the canvas shows garbage. 0108 fixed it, and 0109 broke it again (the castle's textures came first).
- **Known:**
  - The canvas is a direct-colour bitmap BG (engine A BG3) in bank D. It's filled every frame by the DS's display capture (DISPCAPCNT `80370010`: engine A's picture into bank D at +0x8000, 256x192).
  - 0108 added display capture (source A only), and the canvas worked.
  - But in 0108 a capture aimed at bank A, while A held the castle's textures, wrote over them. So 0109 only writes a capture into a bank in LCDC or engine A BG use (VRAMCNT MST 0 or 1). That stopped the canvas captures as well, so in that frame bank D must be mapped differently from what the snapshot showed (0x81).
- **Next:** the log's `display capture:` lines (the first 8) give each capture's DISPCAPCNT and the bank's VRAMCNT at that moment. Work out from them which mappings the DS writes into, and allow D's but not A's while A holds textures.

## 2. The camera at the top of Whomp's Fortress tower (2nd painting, 2nd star), in Boo's Carousel and on the ship -- fixed in 0116 (confirmed)
- **Seen:** at some spots near the top of the tower, the camera drops below the spiral stairs. It's back to normal elsewhere, and drops again on returning. The DS camera never drops there.
- **Seen (Big Boo's Haunt):** in Boo's Carousel the camera loses the player in the same way and goes into the middle of the carousel. The x110 log has a camera jump there (frame 105978) but the camera logs had already run out.
- **Known:**
  - It's not a camera mode change. The follow camera stays in mode 10 the whole time.
  - It's not a jump: the camera moves less than 1500 units in a frame and stays within 600 units below Mario.
  - `-fwrapv` (0100) didn't change it.
  - The follow camera (`func_02009e70`, `src/engine/view/d_camera.cpp`) sets its height between a ground and a ceiling. It casts a line up from itself and one up from the midpoint, then probes the ground from the first line's end. Something in that gives a different answer from the DS.
- **Next (0110):** a tap on the rear panel now dumps the camera's last 2 seconds to the log (`[camera trace]`: position, target, modes, flags, angles, the player, the follow camera's limits and probes, and the camera object raw). Tap right after the camera goes wrong, on the tower or the carousel.
- Builds 0102 and later also log every probe on frames where the height goes wrong (`[camera height]` lines in `gate11.log`):
  - hit point, triangle, collider and owner actor, surface flags, normal;
  - the mode record, dumped once.
  A log of the camera dropping there will show which probe is wrong. Suspects:
  - the KCL line test (`d_bg_w_kc_itcm.cpp`);
  - object-collision transforms (`dBgW_KcMbg`);
  - the ground probe starting from a ceiling hit.

## Also pending
- Two-screen minigames (Mario's 2nd, 4th, 6th, 9th): in 0122 each frame's copy of the other screen was redrawn on the CPU (62 ms) and flickered beside the GPU's picture. 0123 keeps engine A's picture on the GPU and shows it on the other screen the next frame; the capture the game asked for is read back from it. Check: no flashing, full speed, pause screens still right.
- Sprite priority (0123): the sprite pixel kept is the best priority, then the lowest OAM index (it was the lowest index whatever its priority). Fixes the slot machine's BET button and the pinball's mushrooms and ball being cut.
- Touch: the panel's report came and went every other frame while a finger was held (0123 log), so drags reached the game as taps: Yoshi's face strips crawled, Bounce and Pounce's touch screen seemed dead. 0124 keeps the last point through a gap of up to 2 frames. Check both.
- Yoshi's face minigame froze in 0124 (strips not moving): the 0124 log shows every frame skipped between two screen captures, which waited on reading the picture back from the GPU after the swap. 0125 reads it back inside the present, right after drawing it, and logs any frame over 100 ms as "[slow frame]" (3D/2D/present times, textures made/dropped). Check, and send the log if still slow.
- 0125 log: the Yoshi minigame still crawled. The sound driver had started 7.5 million notes: a note of length 0 with note wait on made the track start another note at once, up to 10000 a tick. That took the third core, which the touch screen's drawing helper shares (2D frames of 1-2 s), and the sound lock the game waits on (Bounce and Pounce ran at 28 fps). 0126 makes such a track wait until its note has ended, like the DS driver. Check both minigames.
- 0126: Yoshi's face minigame confirmed. Bounce and Pounce (and the similar touch minigames): Mario only responded where his blue arrow was, away from where he was drawn. In two-screen 3D each screen showed the other screen's 3D: the DS shows a frame's 3D one frame later, with the next frame's screen choice. 0127 draws the last frame's 3D while two-screen capture is on. Check the Marios sit under their arrows.
- 0127: Bounce and Pounce confirmed (enemies now on the touch screen, as on the DS). Trampoline minigames: Mario looked see-through. BLDCNT 2e41 (3D over BG1-3, alpha mode): the 3D was mixed by EVA/EVB; the DS blends 3D over a second target by the pixel's own alpha only, so opaque 3D stays opaque. 0128 fixes it in the CPU compositor and the GPU mix shader. They also run at 25-35 fps at times (open).
- 0128: see-through Mario fixed (confirmed). The first trampoline minigame crashed starting after the second had been played (TrmpLine_Build, a virtual call on a freed object): sTrmpJumpers/sTrmp2MarioPtrs are source-defined ov006 .bss that the port never zeroed on overlay load, so the 1-Mario game found the 5-Mario game's freed objects. 0129 zeroes them when overlay 6 starts. Other source-defined overlay .bss (374 names in ov006 alone) is not zeroed either: watch for crashes on a minigame or level entered a second time.
- Two-screen minigames with many objects (Bounce and Pounce, trampolines) drop to 20-30 fps: the game thread takes 35-48 ms a frame there (about 16k 3D commands a frame). Open.
- A sound kept playing after Yoshi's face minigame until the game was closed: a track that ended with a note still held (its player stayed, 1 track, 0 running). 0124 releases a finished track's notes, and stops a player at most 10 s of ticks after its last track ended.
- Two-screen minigames ran at 17 fps in 0123 (every frame waited for, plus an 11 ms read back). 0124 draws a frame when the drawing core is free and it shows the other screen than the last one drawn, and skips the read back while two-screen 3D runs.
- Luigi's 4th minigame: a huge negative blue number when winning or losing. Needs a capture.
- The slot machine's white bars: the game's own (they fill with the payout); confirmed fine in 0123. Wario's pinball fixed (0123, confirmed).
- Minigames with 3D on both screens (Bounce and Pounce, the pause screen) showed stripes and garbage on the other screen: their display captures were dropped. 0120 kept the bank mapping of the capture's frame; 0121 shows every frame while the game captures (the live view showed every other one, so one of the two screens was never captured) and clears the request after copying it. Still striped in 0121 (lines of two different frames mixed, in the bank's own memory), and slow: about 15 fps, each capture redraws the 3D on the CPU. 0122 logs a `[capture]` line a frame with Debug log on. Idea if the timeline doesn't explain it: in two-screen mode, show the last A picture of each screen directly (kept on the GPU) instead of going through the banks.
- The white line down the middle of both screens in the minigame menu: two windows split the screen (0-127, 128-255) leaving columns 127 and 255 outside both (0121 closes the gap).
- Some minigames' screens show horizontal stripes while they open (slots, pinball, Bounce and Pounce top band): probably per-scanline effects (HBlank interrupts) the port doesn't run. Look at it if they bother.
- Moon jump: the stick didn't steer in the air (0115-0118): the state the game is in while A is held up there never calls the air control that was hooked (0118 log: steer calls 0). 0119 steers from the cheat itself, every frame, and leaves the game's air control out meanwhile.
- Bullies feel hard to push off (also before 0116): compare with the DS.
- Frame rate: 0116 sorted all polygons into the DS's order, which split the GPU's drawing into many small batches (menus at ~26 fps). 0118 sorts only far-plane opaque polygons and the translucent ones.
- Fixed in 0116, check them:
  - line probes hitting a moving or scaled object's collision answered the object's origin (`d_bg_w_kc_mbg*.cpp`: the scratch line's fields stored apart). Suspected cause of the camera at Whomp's tower, the carousel and the ship, and of ledge climbs ending in the middle of a block (the log showed the player put at a block's origin, 6900 -1250 -200).
  - Lethal Lava Land's three small Bullies were never created: `daDonketu_c::InitResources` returned 0 (`return {}`).
  - Bowser's shock waves in the last fight broke into stripes (0117): depth-"equal" decals weren't supported, and the fight's W depth mode was ignored.
  - The mission select's star eyes were cut: polygons are now drawn in the DS's order (sorted by screen row), which decides ties in depth.
- The crash at the end, after the last Bowser fight: fixed in 0114 (the global `new` now zeroes like the DS heap); the ending was reached in 0115.
- The other 31 functions ending in `return {}` (where the ROM returns whatever r0 holds) were checked in 0118: every caller ignores their result, so the 0 they return changes nothing. Only the two InitResources mattered (fixed in 0116).
- Title screen castle: a few green/white dots along the roof edges (capture 0547). The castle there is a 2D picture, not 3D.
- VS mode: returns to the title screen (the ov075 lobby crashed).
- Black doors, and the thing behind King Goomba (Big Goomboss): check them again with 0107. It fixed textures whose palette goes on into the next palette slot (Bowser's black patches), which may have been the same bug.
- The camera glitch when Mario's door opens (before the character-switch fix in 0092; check whether it's still there).
- Music once stopped before Mario's room. It didn't happen again; the `[sound]` lines log it.
- Menu music playing twice over itself (a "canon") after Adventure -> main menu: the game saw the music player idle right after starting it (the driver's status was written late) and lost track of it. Fixed in 0108; check it.
- The debugging logs added along the way (camera watch and traces, root motion, camera probes, sound players, display registers, input, glitch watch, drawing times) are off unless the DEBUG LOG setting is on (0120). The temporary Bully and moon jump lines were removed.
