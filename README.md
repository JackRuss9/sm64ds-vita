# Super Mario 64 DS for PS Vita

A native PS Vita port of **Super Mario 64 DS**, built from the
[Super Mario 64 DS decompilation](https://github.com/tangosdev/sm64ds-decomp)
(see [README.decomp.md](README.decomp.md) for the decompilation project itself).

The game's code is compiled for the Vita's ARM CPU. The DS hardware the
game expects is supplied by the port:
- the 3D engine is drawn by the Vita's GPU through vitaGL, at up to 4× the DS resolution;
- the 2D engines are emulated;
- the sound is a full re-creation of the DS sound driver;
- the touch screen is the Vita's front panel.

> **This repository contains no Nintendo data.** No ROM, no graphics, no
> sound, no text. The game reads everything from **your own dump** of the
> cartridge at run time.

## What you need

| | |
|---|---|
| A PS Vita / PS TV with HENkaku/Ensō | Homebrew enabled, VitaShell installed |
| `libshacccg.suprx` in `ur0:data/` | The system shader compiler. Install it with [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D). Without it, the game falls back to the much slower CPU renderer. |
| **Your own Super Mario 64 DS ROM, European version (game code ASMP)** | Copy it to `ux0:data/sm64ds/sm64ds.nds`. Other regions are not supported yet. |

## Installing

1. Copy your ROM to `ux0:data/sm64ds/sm64ds.nds`; create the `sm64ds` folder if it doesn't exist.
2. Install `sm64ds-vita.vpk` with VitaShell.
3. Start **Super Mario 64 DS** from the LiveArea.

The first start takes a few seconds longer while the shaders are compiled
and cached (`ux0:data/sm64ds/shader_cache`).

Files the game uses, all in `ux0:data/sm64ds/`:

| File | What it is |
|---|---|
| `sm64ds.nds` | Your ROM (you provide it) |
| `settings.txt` | The Settings and Cheats menu choices |
| `state.bin` | Your save state (D-pad UP + R) |
| `gate11.log`, `results.txt` | The run's log, for bug reports |
| `boot_*.bmp`, `snapshot_*.bin` | Screen captures (see *Reporting graphics bugs*) |
| `sm64ds.sav` | Your save game (the cartridge's save memory) |

## Controls

| Vita | DS |
|---|---|
| Left stick / D-pad | D-pad (move) |
| Circle | A (jump) |
| Cross | B |
| Triangle | X |
| Square | Y |
| L / R | L / R |
| Start / Select | Start / Select |
| Front touch screen | DS touch screen (where the bottom screen is shown) |
| Right stick (left/right) | Turns the camera (can be switched off in Settings) |
| Rear touch panel | Screen capture (when switched on in Settings) |
| D-pad UP + R | **Save state** |
| D-pad DOWN + L | **Load state** |

## Settings menu: D-pad RIGHT + SQUARE

Use Up/Down to choose an option and Left/Right or Cross to change it.
Circle closes the menu. Choices are saved to `settings.txt`, and the game is
paused while a menu is open.

| Option | Choices |
|---|---|
| **Screen layout** | Side by side · Big top screen · Big touch screen · Top screen only · **Widescreen 16:9** (true widescreen: the 3D view is wider, not stretched, and the touch screen is a small inset) |
| **3D resolution** | **Screen size** (drawn at exactly the size it's shown: the sharpest) · 1× (DS) to 4× |
| **Frame rate** | Show or hide the frame counter. It also shows the game speed when that isn't 100%. |
| **Rear tap captures** | A tap on the rear panel saves the screens plus a recording of the last 1–2 seconds of 3D. While this is on, the port also captures automatically if a model suddenly changes size (glitch watch). |
| **Game speed** | 100 · 50 · 25 · 10 · 5 %. Slows the game down, useful to look at a glitch. |
| **Right stick camera** | On / off |

Layout and resolution only apply to the GPU renderer.

## Cheats menu: D-pad LEFT + CIRCLE

| Cheat | Effect |
|---|---|
| **Infinite lives** | The life counter never drops below 4 |
| **Moon jump** | Hold jump (Circle) to keep rising |
| **Infinite power** | The power meter refills whenever it drops. Falling out of the level still costs a life. |
| **Infinite breath underwater** | The power meter doesn't drain underwater |
| **99 coins** | The coin counter starts at 99: the next coin is the 100th and brings the course's 100-coin star |
| **God mode** | Enemies, fire and hazards can't hurt you, and the power meter stays full. Falling out of the level still costs a life. |

## Save states

**D-pad UP + R** saves the game's exact state and **D-pad DOWN + L** loads it
back. These are separate from the game's own save. There is one slot,
`ux0:data/sm64ds/state.bin`, and a message at the top of the screen confirms
each save or load. Save states work while you're playing, not during loading
screens. Saving and loading take about a second. A state belongs to the build
that made it: after updating the port, save a new one, because older states
are refused.

## How far it goes

This is an early, playable build. Tested on hardware so far:
- the title screen, file select and saving;
- the castle and its rooms;
- **Bob-omb Battlefield**, including King Bob-omb and his star;
- **Whomp's Fortress**;
- **Cool, Cool Mountain** (entering and playing);
- **freeing Mario** from his room, and switching characters at the doors;
- playing as Yoshi, Mario, Luigi and Wario;
- save states;
- Widescreen 16:9 and the Vita's native resolution;
- sound and music, which sound like the DS.

It runs at the DS game's own 30 fps at up to 4× resolution, and loads
almost instantly.

Later courses, the boss levels, the minigames and the multiplayer mode have
not been played through yet. Expect crashes there, and please report them.

### Known issues

- In Boo's Carousel and at the top of Whomp's tower, the camera can lose the player.
- The drawing toy in the main menu shows garbage on the lower screen.
- Choosing VS mode on the title screen returns to the title screen (it isn't supported yet).
- The DS microphone and wireless play are not supported.

## Reporting bugs

After a crash, VitaShell can show the crash dump in `ux0:data/` (a
`psp2core-*.psp2dmp` file). Please attach these files to the issue:
- `ux0:data/sm64ds/gate11.log`;
- `results.txt`;
- the `.psp2dmp` crash dump;
- the `gate11.syms` symbol file from your build (see below).

### Reporting graphics bugs

Glitches that last a frame are hard to catch by eye:
1. Turn on **Rear tap captures**.
2. Tap the rear panel just *after* you see the glitch. The port keeps the last 1–2 seconds of 3D, so a late tap still holds it.
3. Send the `boot_*.bmp`, `snapshot_*.bin` and `gate11.log` files.

Lowering **Game speed** helps too.

## Building

You need Linux or WSL with:
- [VitaSDK](https://vitasdk.org) installed and `$VITASDK` set;
- `cmake` (3.16 or later), `python3`, `git` and a host C/C++ compiler.

```sh
git clone https://github.com/JackRuss9/sm64ds-vita.git
cd sm64ds-vita
bash port/vita/deps.sh                 # builds vitaGL & co. (pinned versions) into build/vita-deps
cmake -S port/vita -B build/vita
cmake --build build/vita -j8
```

The package is `build/vita/sm64ds-vita.vpk`. For crash reports, also keep
the symbol list:

```sh
$VITASDK/bin/arm-vita-eabi-nm -n build/vita/gate11 > gate11.syms
```

The build needs no ROM. The game's data tables are filled from your ROM on
the console at start-up.

### Custom bubble and LiveArea (optional, not in this repository)

The bubble icon and LiveArea pictures are not part of the repository,
because they would be game artwork. To add your own, put these
**8-bit palette PNGs** in `port/vita/livearea/`:

| File | Size | Used for |
|---|---|---|
| `icon0.png` | 128×128 | The bubble |
| `bg.png` | 840×500 | LiveArea background (needs `startup.png` too) |
| `startup.png` | 280×158 | The start button picture |
| `pic0.png` | 960×544 | Optional: shown while the game starts |

Then run cmake again. `.gitignore` keeps these files out of git. Convert
your images with, for example, `pngquant --force 256 file.png` or
ImageMagick `convert in.jpg -resize 128x128! -colors 256 PNG8:icon0.png`.

## Credits

- The [Super Mario 64 DS decompilation](https://github.com/tangosdev/sm64ds-decomp)
  by Tango and contributors (MIT licence, see [LICENSE](LICENSE)). This port
  is built on it.
- [vitaGL](https://github.com/Rinnegatamante/vitaGL),
  [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) and math-neon by
  Rinnegatamante and contributors.
- [VitaSDK](https://vitasdk.org).
- The settings and cheats menus follow the
  [Super Mario Sunshine Vita port](https://github.com/JackRuss9/sms-vita).

Super Mario 64 DS is © Nintendo. This project isn't affiliated with or
endorsed by Nintendo. Buy the game and dump your own copy.
