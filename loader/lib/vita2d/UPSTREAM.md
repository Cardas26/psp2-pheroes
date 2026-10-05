# vita2d — vendored

verified: 2026-10-05 @ dfefb73

- Origin: https://github.com/xerpi/libvita2d @ `a8f15ab` (2021-11-26), its `libvita2d/` directory.
- License: MIT (`LICENSE`).
- Imported: `source/vita2d.c`, `vita2d_texture.c`, `vita2d_draw.c`, `utils.c`, `include/vita2d.h`, `utils.h`, `shared.h`, and the shaders as linkable objects in `shader/`.
- Left out: the image loaders (`vita2d_image_*.c`), the font code (`vita2d_font.c`, `vita2d_pgf.c`, `vita2d_pvf.c`, `texture_atlas.c`, `bin_packing_2d.c`, `int_htab.c`), the `.cg` shader sources and upstream's build files.
- Built by `loader/CMakeLists.txt` as a static library, warnings off (`-w`).

## Local patches

None: the sources are byte-identical to upstream at `a8f15ab` (checked 2026-10-05).
