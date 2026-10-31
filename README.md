# nds-rom-reader

A Nintendo DS ROM reader in C.

- `ndsrom` – library: `include/ndsrom/ndsrom.h` parses the header and the
  FNT/FAT filesystem and reads/extracts asset files; `include/ndsrom/ndsgfx.h`
  handles LZ77/RLE decompression, NCLR/NCGR/NSCR/NCER/NANR graphics and PNG/GIF output.
- `ndsviewer` – command-line viewer built on the library.

## Build

    cmake -S . -B build && cmake --build build

## Use

    build/ndsviewer game.nds info
    build/ndsviewer game.nds list
    build/ndsviewer game.nds hex data/file.bin 128
    build/ndsviewer game.nds extract data/file.bin out.bin
    build/ndsviewer game.nds extract-all out_dir
    build/ndsviewer game.nds unpack dat/spr/0c out.NCGR          # decompress
    build/ndsviewer game.nds png dat/bg/b_dwaku.NCGR out.png \
        --pal dat/bg/b_dwaku.NCLR --scr dat/bg/b_dwaku.NSCR      # render a background
    build/ndsviewer game.nds png-all out_dir                     # render every graphic
    build/ndsviewer game.nds anim dat/BTCHR/228.c out/dragon     # animated GIFs
    build/ndsviewer game.nds anim-all out_dir                    # every sprite's animations

`png` finds a palette (and `png-all` a tile map) from same-named files in the
same directory when not given. Sprites are tiles in object-memory order: pass
`--cer <NCER>` to `png` (or let `png-all` write `*_cells.png`) to assemble them
into their frames. `anim` / `anim-all` combine the cells with NANR animation
data and write one animated GIF per sequence (`<prefix>_<n>.gif`). Rotation and
scaling in animations and 3D formats aren't supported yet.

## Test

    ctest --test-dir build
