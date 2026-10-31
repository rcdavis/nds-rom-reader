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
    build/ndsviewer game.nds text dat/msg_new/bathelp.sdb        # print a message bank
    build/ndsviewer game.nds text-all out_dir                    # every .sdb as .txt
    build/ndsviewer game.nds sdat dat/SND/sound_data.sdat        # list an SDAT
    build/ndsviewer game.nds audio dat/SND/sound_data.sdat out   # SSEQ/SBNK + WAVs
    build/ndsviewer game.nds tex a/a.nsbtx out_dir               # textures as PNG
    build/ndsviewer game.nds model a/a.nsbmd out_dir             # OBJ + GLB (+ animations) + PNG + preview
    build/ndsviewer game.nds render a/a.nsbmd out.png --yaw 30 --pitch 25 --size 512
    build/ndsviewer game.nds model-all out_dir                   # every NSBMD

`png` finds a palette (and `png-all` a tile map) from same-named files in the
same directory when not given. Sprites are tiles in object-memory order: pass
`--cer <NCER>` to `png` (or let `png-all` write `*_cells.png`) to assemble them
into their frames. `anim` / `anim-all` combine the cells with NANR animation
data and write one animated GIF per sequence (`<prefix>_<n>.gif`). Rotation and
scaling in animations aren't supported.

`audio` writes each SDAT sequence, bank and archive under its symbol name, and
decodes every wave in the wave archives (PCM8, PCM16, IMA-ADPCM) and every
stream to WAV, with loop points in a `smpl` chunk. Sequences are not converted
to MIDI.

`text` decodes the 16-bit message banks (`.sdb`) using the English font order.
Punctuation codes are partly inferred, unknown codes print as `<HEX>`, control
codes as `{name}`. Japanese banks and the `stxt_*`/`DigZknHelp` banks use other
layouts and are skipped or unreadable.
`model` / `render` read NSBMD models (display lists, node/matrix-stack render
commands) and decode NSBTX textures (all seven formats, including 4x4
compressed). Textures come from the model itself, `--tex <nsbtx>`, or the
same-stem `.nsbtx`. `model` writes OBJ + MTL, a self-contained GLB (glTF 2.0 binary, textures embedded) and PNGs, plus a `_preview.png`
software render (orthographic, no lighting).
Joint animation (NSBCA) is read from the same-stem `.nsbca` or `--anim <nsbca>`
and written into the GLB as a node hierarchy with one glTF animation per clip
(30 fps, sampled per frame); clips are matched to models by name.
`render ... --anim-index N --frame F` poses the model for a still.
Not supported: NSBTA/NSBVA (texture/visibility) animation, texture matrices and
normals; skinned (NODEMIX) triangles stay static. Golden Sun: Dark Dawn compresses most 3D files with a
game-specific LZ variant (tag 0x40), which `unpack` and the viewer handle.

## Test

    ctest --test-dir build
