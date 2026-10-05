# Third-party material

## Code under `loader/lib/`

| Directory | Upstream | Licence | Local changes |
|---|---|---|---|
| `loader/lib/vita2d` | [xerpi/libvita2d](https://github.com/xerpi/libvita2d) at `a8f15ab`: the core and its precompiled shaders | MIT (`LICENSE`) | none; the image and font loaders left out |
| `loader/lib/vitashaders` | [frangarcj/vita-shader-collection](https://github.com/frangarcj/vita-shader-collection) release `master-0.1-v86`: the compiled LCD3x and FXAA shaders | LCD3x: public domain (Gigaherz). FXAA: BSD 3-clause, below | none; `shaders.S` includes the bytes |

## Libraries linked from the toolchain

| Library | Source | Licence | How it is used |
|---|---|---|---|
| mpg123 1.33.4 | [mpg123.de](https://www.mpg123.de/), the vitasdk `mpg123` package (`Containerfile`) | LGPL 2.1 | statically linked into `eboot.bin`; plays the music. This repository at each release tag is the complete source of the application, so a modified mpg123 can be linked in by rebuilding |

### FXAA (`loader/lib/vitashaders/fxaa_*.gxp`)

```
Copyright (c) 2011 by Armin Ronacher.

Some rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.

    * The names of the contributors may not be used to endorse or
      promote products derived from this software without specific
      prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Music under `data/palmheroes/Music/Original/`

The 11 terrain tracks Palm Heroes 1.05.3 names, by Kevin MacLeod
([incompetech.com](https://incompetech.com)), licensed under
[Creative Commons: By Attribution 4.0](https://creativecommons.org/licenses/by/4.0/):
- "Arid Foothills"
- "Cambodian Odyssey"
- "Celtic Impulse"
- "Colossus"
- "Desert City"
- "Expeditionary"
- "Opium"
- "Phantasm"
- "Supernatural"
- "Temple of the Manes"
- "Willow and the Light"
- "Cambodian Odyssey"
- "Cambodean Odyssey.mp3"

Changes: resampled to 48 kHz, 320 kbps, where they were not already;
"Cambodian Odyssey" is saved as `Cambodean Odyssey.mp3`, the name the game
asks for. The same credits ship beside the tracks in `CREDITS.txt`.

## The game

Palm Heroes is copyright iO UPG. Its developers published
`PalmHeroes1.05.3.zip` on 2010-12-06 as "The game is 100% free!", and its
source as Apache-2.0 on Google Code. The VPK packs that release's `Data/`,
`Maps/` and its `hmmppc.exe` rebased for the Vita (`ph.img`); this
repository holds none of them. `tools/prepare_game.py` builds them from the
zip, checked by sha256.

The LiveArea art in `loader/livearea/` is upscaled from the game's own art.
