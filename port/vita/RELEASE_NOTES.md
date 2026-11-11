# sm64ds-vita v0.2.0 (alpha)

The second build of the native PS Vita port of Super Mario 64 DS.

**You need your own European (ASMP) Super Mario 64 DS ROM.** Copy it to
`ux0:data/sm64ds/sm64ds.nds`. This release contains no game data.

## Install
1. Install `libshacccg.suprx` with [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D), if you haven't already.
2. Copy your ROM to `ux0:data/sm64ds/sm64ds.nds`.
3. Install `sm64ds-vita.vpk` with VitaShell, then start it from the LiveArea.

## New in this version
- **Save states that work**: D-pad Up + R saves, D-pad Down + L loads, each in about a second.
- **Instant loading**: choosing a file used to take about 15 seconds, and now it's instant.
- **Freeing Mario and switching characters** work: no more jumping camera after a rescue or at the character doors.
- **Screen size resolution**: the 3D can be drawn at exactly the size it's shown on the Vita.
- **Widescreen 16:9 shows more**: the game now draws what is at the sides of the wider view.
- Fixed: giant Goombas, Koopas and Yoshi; the Cool, Cool Mountain crash; the Big Goomboss crash;
  Yoshi's running smoke drawn as plain squares; a crash entering the castle a second time;
  the slowdown near Piranha Plants; black doors with Yoshi's eyes on them.
- VS mode returns to the title screen instead of crashing.

## What's in it
- **Native port**: the game's code runs directly on the Vita, not in an emulator.
- **GPU 3D** at the Vita's screen size or 1×–4× the DS resolution (vitaGL).
- **Screen layouts**: side by side, big top screen, big touch screen, top screen only, and true **Widescreen 16:9**.
- **Sound**: a full DS sound driver (sequences, instruments, ADPCM, PSG/noise).
- **Settings menu** (D-pad Right + Square) and **Cheats menu** (D-pad Left + Circle): infinite lives, moon jump, infinite power, infinite breath underwater, 99 coins.
- Right stick turns the camera. The touch screen works on the front panel.
- A **glitch recorder** for bug reports: a rear tap saves the last 1–2 seconds of 3D.

## Status
Tested on hardware so far:
- the castle, freeing Mario, and switching characters;
- Bob-omb Battlefield (including King Bob-omb);
- Whomp's Fortress;
- Cool, Cool Mountain;
- the Big Goomboss.

Other courses, the minigames and multiplayer haven't been played through yet. Please report
crashes with `ux0:data/sm64ds/gate11.log`, the `.psp2dmp` crash dump and the `gate11.syms` of your build.

## Known issues
- In the intro, Peach's crown lies flat instead of sitting on her head.
- Garbage on the lower screen while the main menu's drawing toy loads.
- VS mode isn't supported yet.
- No microphone or wireless play.
