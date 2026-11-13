# Open issues (Vita port)

Bugs we've looked at but not solved yet, with what we know so far.

## 1. The drawing toy ("Mario drawing") canvas
- **Seen:** the canvas shows garbage. 0108 fixed it, and 0109 broke it again (the castle's textures came first).
- **Known:**
  - The canvas is a direct-colour bitmap BG (engine A BG3) in bank D. It's filled every frame by the DS's display capture (DISPCAPCNT `80370010`: engine A's picture into bank D at +0x8000, 256x192).
  - 0108 added display capture (source A only), and the canvas worked.
  - But in 0108 a capture aimed at bank A, while A held the castle's textures, wrote over them. So 0109 only writes a capture into a bank in LCDC or engine A BG use (VRAMCNT MST 0 or 1). That stopped the canvas captures as well, so in that frame bank D must be mapped differently from what the snapshot showed (0x81).
- **Next:** the log's `display capture:` lines (the first 8) give each capture's DISPCAPCNT and the bank's VRAMCNT at that moment. Work out from them which mappings the DS writes into, and allow D's but not A's while A holds textures.

## 2. The camera at the top of Whomp's Fortress tower (2nd painting, 2nd star), and in Boo's Carousel
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
- VS mode: returns to the title screen (the ov075 lobby crashed).
- Black doors, and the thing behind King Goomba (Big Goomboss): check them again with 0107. It fixed textures whose palette goes on into the next palette slot (Bowser's black patches), which may have been the same bug.
- The camera glitch when Mario's door opens (before the character-switch fix in 0092; check whether it's still there).
- Music once stopped before Mario's room. It didn't happen again; the `[sound]` lines log it.
- Menu music playing twice over itself (a "canon") after Adventure -> main menu: the game saw the music player idle right after starting it (the driver's status was written late) and lost track of it. Fixed in 0108; check it.
- The debugging logs added along the way (camera watch, root-motion trace, camera probes, music lines) all stop after a fixed number of lines. Remove them once these issues are solved.
