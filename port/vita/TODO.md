# Open issues (Vita port)

Bugs we've looked at but not solved yet, with what we know so far.

## 1. Peach's crown, in the intro
- **Seen:** in the intro movie, Peach's crown lies flat instead of sitting on her head. Everything else in the intro looks right.
- **Known:**
  - The crown is drawn by a separate demo actor (`daDemo`).
  - The port turns it only left or right (yaw), so it ends up flat compared with the camera-facing scene.
  - Its forward tilt (X angle) should come from the intro script's record. The handler that sets the angles from that record is `func_ov002_020f6e48`.
- **Next:** log the angles that handler reads and writes for the crown, and check how the record is read (field offsets, sign of the s16 values).

## 2. The drawing toy ("Mario drawing") shows rubbish
- **Seen:** opening the drawing toy from the main menu shows garbage on the lower screen while it loads.
- **Known:** the canvas background (engine B, BG3) shows tiles that are still in VRAM from the previous screen before the game draws over them.
- **Next:** find what clears that VRAM on the DS. It could be a DMA/memory fill the port skips, or display brightness or the BG enable bit keeping it hidden until it's ready. Compare BG3 and VRAMCNT and the master brightness over the first frames.

## 3. The camera at the top of Whomp's Fortress tower (2nd painting, 2nd star)
- **Seen:** at some spots near the top of the tower, the camera drops below the spiral stairs. It's back to normal elsewhere, and drops again on returning. The DS camera never drops there.
- **Known:**
  - It's not a camera mode change. The follow camera stays in mode 10 the whole time.
  - It's not a jump: the camera moves less than 1500 units in a frame and stays within 600 units below Mario.
  - `-fwrapv` (0100) didn't change it.
  - The follow camera (`func_02009e70`, `src/engine/view/d_camera.cpp`) sets its height between a ground and a ceiling. It casts a line up from itself and one up from the midpoint, then probes the ground from the first line's end. Something in that gives a different answer from the DS.
- **Next:** builds 0102 and later log every probe on frames where the height goes wrong (`[camera height]` lines in `gate11.log`):
  - hit point, triangle, collider and owner actor, surface flags, normal;
  - the mode record, dumped once.
  A log of the camera dropping there will show which probe is wrong. Suspects:
  - the KCL line test (`d_bg_w_kc_itcm.cpp`);
  - object-collision transforms (`dBgW_KcMbg`);
  - the ground probe starting from a ceiling hit.

## Also pending
- VS mode: returns to the title screen (the ov075 lobby crashed).
- Black doors, and the thing behind King Goomba (Big Goomboss).
- The camera glitch when Mario's door opens (before the character-switch fix in 0092; check whether it's still there).
- Music once stopped before Mario's room. It didn't happen again; the `[sound]` lines log it.
- The debugging logs added along the way (camera watch, root-motion trace, camera probes, music lines) all stop after a fixed number of lines. Remove them once these issues are solved.
