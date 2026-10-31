# nds-rom-reader

A Nintendo DS ROM reader in C.

- `ndsrom` – library: `include/ndsrom/ndsrom.h` parses the header and the
  FNT/FAT filesystem and reads/extracts asset files; `include/ndsrom/ndsgfx.h`
  handles LZ77/RLE decompression, NCLR/NCGR/NSCR/NCER graphics and PNG output.
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

`png` finds a palette (and `png-all` a tile map) from same-named files in the
same directory when not given. Sprites are tiles in object-memory order: pass
`--cer <NCER>` to `png` (or let `png-all` write `*_cells.png`) to assemble them
into their frames. NANR animation data and 3D formats aren't decoded yet.

## Test

    ctest --test-dir build
