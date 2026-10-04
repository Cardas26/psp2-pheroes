# Palm Heroes for the PS Vita

Vibecoded. Palm Heroes 1.05.3 (iO UPG, 2010) is a Heroes-of-Might-and-Magic-style
strategy game for Windows Mobile. I initially started this project porting the 
released sourcecode. But the sourcecode is not a clean release. It appears to be 
a snapshot with both v1.04 and v1.05 components in beta state. Getting that to a 
polished state was not viable. I then discovered that v1.05.3 was released as free, 
and was in a decent state. Decided to build a loader for that instead, and create 
some general and touch specific enhancements (see below).

## What works
- All main gameplay functionality, 4 maps tested from beginning to end. Please open 
an issue if you encounter any bugs.
- The original game ran at 33fps, which doesn't look good on the vita, especially
when panning the map. Created a 30 fps mode for the "authentic" experience, and 60fps 
for the modern one (default). In 30fps, set the cpu to 111MHz in PSVShell for some 
extra battery saving.
- The original stylus controls were fine in WinMobile, but with touch controls you 
cannot see the melee attack angles very well. So I made a touch-friendly melee attack
ring that pops out on enemy unit selection.
- Added a cursor as an alternative for touch navigation.
- Positioning of pickups and enemy troops can be confusing at times, as objects can be
located behind others. This can make it hard to see which object is blocking your 
path. I created an outline option that shows outlines around all visible pickups and creatures.
- The original game shipped without music, but did credit Kevin MacLeod for it. 
The game also contains references to music files. I found the original music online
and added it.
- Palm Heroes shares DNA with several other HoMM titles, mostly HoMM2. Terrain and 
factions map almost 1 on 1. Decided to add support for optional HoMM2 music as well, 
so Castles and Battlefield don't have to be silent.
- Added support for several upscalers, defaulting to "Sharp".
- Ideas for further enhancements are welcome. 

## Requirements

- A PS Vita on HENkaku or Ensō.
- [kubridge](https://github.com/bythos14/kubridge/releases) installed under `*KERNEL` in `ur0:tai/config.txt`; reboot after adding it.
- About 15 MB free, plus 76 MB for the music.

## Install

- Download `palmheroes.vpk` from the [latest release](../../releases/latest) and install it with VitaShell.
- For music, download `palmheroes-music.zip` from the same release and extract it to `ux0:/data/`: it creates `ux0:/data/palmheroes/Music/` (see Music section below).

## Music

- **Original:** the 11 tracks Palm Heroes names for its terrains, by Kevin MacLeod (CC-BY 4.0), in `palmheroes-music.zip` (also this repository's `data/palmheroes/`); they end up in `ux0:/data/palmheroes/Music/Original/`. The default.
- **HoMM2:** put your own Heroes of Might and Magic II soundtrack in `ux0:/data/palmheroes/Music/HoMM2/`, named as its `README.txt` lists (GOG release). It adds town, battle and result themes.
- **None:** the game's own sounds only, as on Windows Mobile.

See Controls section below how to switch music sets in-game.

## Update

Install the new VPK over the old one. Saves and settings stay.

## Controls

| Input | Action |
|---|---|
| Touch | The stylus |
| D-pad, left stick | Scroll the map |
| Cross | Enter |
| Square | Hold and touch screen to move the map by touch |
| Triangle | Zoomed out map view. Use D-pad and left stick to scroll |
| Circle | Close the melee attack ring; otherwise unused (it minimized the game on Windows Mobile) |
| L | The game's E key: assign it in the in-game Key Map |
| Right stick, R | Move the cursor; R shows it, then taps the stylus at it |
| Start | Show or hide outlines around monsters and pickups |
| Circle + L | Next screen scaling: sharp bilinear, nearest, 2x, LCD3x, FXAA |
| Circle + R | 60 or 30 frames per second |
| Circle + Start | Melee aiming: ring tap, ring drag or original |
| Circle + Select | Music: Original, HoMM2 or none |

## Settings

`ux0:/data/palmheroes/settings.txt` keeps the scaling, frame rate, melee aiming and music choices. The game writes it on first start; edit it with the game closed.

## Report a bug

Install `palmheroes-diagnostics.vpk` from the [latest release](../../releases/latest) over the game, reproduce the problem, and attach
`ux0:/data/palmheroes/debug.log` and `ux0:/data/palmheroes/PHeroes.txt` to a
[bug report](../../issues/new/choose).

## Build from source

1. Build the toolchain image: `podman build -t vitasdk-pheroes .`
2. Download `PalmHeroes1.05.3.zip`, the developers' free release, and run `python3 tools/prepare_game.py PalmHeroes1.05.3.zip game`. It checks the zip and writes `game/`.
3. Build: `podman run --rm -v "$PWD":/src:Z -w /src/loader vitasdk-pheroes sh -c "cmake -B build && cmake --build build"`. Add `-DDIAGNOSTICS=ON` to the first `cmake` for the Diagnostics build.
4. Install `loader/build/palmheroes.vpk`.

## Credits

- Palm Heroes: iO UPG.
- Music: Kevin MacLeod (incompetech.com), CC-BY 4.0.
- [libvita2d](https://github.com/xerpi/libvita2d), [vita-shader-collection](https://github.com/frangarcj/vita-shader-collection), [kubridge](https://github.com/bythos14/kubridge), [VitaSDK](https://vitasdk.org).
- Written with [Claude Code](https://claude.com/claude-code).

## Legal

- Palm Heroes and its art are copyright iO UPG. 
- The LiveArea art is upscaled from the game's own. 
- This project is not affiliated with or endorsed by iO UPG. 
- The port's code is MIT (`LICENSE`); see `THIRD_PARTY.md` for the rest.
- No donations are accepted
