# sm64ds-vita v0.1.0 (alpha)

The first public build of the native PS Vita port of Super Mario 64 DS.

**You need your own European (ASMP) Super Mario 64 DS ROM.** Copy it to
`ux0:data/sm64ds/sm64ds.nds`. This release contains no game data.

## Install
1. Install `libshacccg.suprx` with [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D), if you haven't already.
2. Copy your ROM to `ux0:data/sm64ds/sm64ds.nds`.
3. Install `sm64ds-vita.vpk` with VitaShell, then start it from the LiveArea.

## What's in it
- **Native port**: the game's code runs directly on the Vita, not in an emulator.
- **GPU 3D at 1×–4× the DS resolution** (vitaGL).
- **Screen layouts**: side by side, big top screen, big touch screen, top screen only, and true **Widescreen 16:9**.
- **Sound**: a full DS sound driver (sequences, instruments, ADPCM, PSG/noise).
- **Settings menu** (D-pad Right + Square): layout, resolution, frame counter, rear-tap captures, game speed, right stick camera.
- **Cheats menu** (D-pad Left + Circle): infinite lives, moon jump, infinite power, infinite breath underwater, 99 coins.
- Right stick turns the camera. The touch screen works on the front panel.
- A **glitch recorder** for bug reports: a rear tap saves the last 1–2 seconds of 3D.

## Status
Tested on hardware so far:
- the castle;
- Bob-omb Battlefield (including King Bob-omb);
- Whomp's Fortress;
- Cool, Cool Mountain.

Other courses, the minigames and multiplayer are untested. Please report
crashes with `ux0:data/sm64ds/gate11.log` and the `.psp2dmp` crash dump.

## Known issues
- Peach's crown has the wrong colours.
- Garbage on the lower screen while the main menu's drawing toy loads.
- In widescreen, objects can pop in late at the screen edges.
- No microphone or wireless play.
