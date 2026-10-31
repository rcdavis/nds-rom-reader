# nds-rom-reader

A Nintendo DS ROM reader in C.

- `ndsrom` – library (`include/ndsrom/ndsrom.h`): parses the header and the
  FNT/FAT filesystem, and reads/extracts asset files.
- `ndsviewer` – command-line viewer built on the library.

## Build

    cmake -S . -B build && cmake --build build

## Use

    build/ndsviewer game.nds info
    build/ndsviewer game.nds list
    build/ndsviewer game.nds hex data/file.bin 128
    build/ndsviewer game.nds extract data/file.bin out.bin
    build/ndsviewer game.nds extract-all out_dir
