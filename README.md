# PMD-ExplorersofSkyRecomp
i saw a really mid "recomp" of Sky which is basically just MelonDS emulation called recompilation for idk clout? annoyed me so much i thought eff it i'll just do it myself. i'm putting this here for anyone to fork it, improve it further, it already works, just got modern QOL stuff to sort out my end. this my first github account lol so go easy onme

---


A static recompilation of **Pokémon Mystery Dungeon: Explorers of Sky** (NA) into a native Windows program.
The game's ARM9 code is lifted to C and runs natively. melonDS (patched) supplies the rest of the DS hardware:
ARM7, graphics, sound, timing. Anything not lifted falls back to melonDS's interpreter.

**This repo contains no game code or data.** The C code for the game is generated from your own legally dumped
ROM by the tools here. Never commit the ROM, saves or anything in `gen/`.

## Features
- 16:9 widescreen. Dungeons and towns draw real extra content, including the 3D layer (dungeon darkness, clouds),
  instead of stretching.
- Recomp Options in the in-game menu: widescreen, filters, Enhancements, autosave, save slots, exit to Windows.
- Filters: xBRZ, Scanlines, Smooth, Glow, DS Screen.
- Enhancements (toggleable, based on SkyTemple patches): Pick Any Starter, Full Team Control, Gentler Hunger,
  Keep Items on Defeat, IQ Grows with Level, Team EXP Share.
- Three save slots; windowed, borderless and fullscreen modes.
- A flight recorder (`replay_last.rpl`) and crash log (`recomp_log.txt`) next to the ROM, for debugging.

## Layout
| Path | What |
|---|---|
| `tools/` | Python: ROM reader, function inventory, ARM/Thumb lifter, Enhancements patch assembler |
| `runtime/` | Hybrid runtime (fibers, dispatch, interpreter fallback), native replacements and widescreen hooks, in-game menu |
| `frontend/` | SDL2 Windows frontend, filters, settings |
| `referee/` | Linux test harness: run, replay, savestate and screenshot tooling |
| `patches/melonds-recomp.patch` | Changes to melonDS (wide margins, wide 3D, debug watches, savestates) |
| `NOTES.md` | Engineering log: every fix and why |

## Building
Needs Python 3 (with `pyyaml`), clang, CMake, and:
- [melonDS](https://github.com/melonDS-emu/melonDS) at commit `ff972523f8a70e877dd054439876c3c8c6b79197`, with `patches/melonds-recomp.patch` applied
- [pmdsky-debug](https://github.com/UsernameFodder/pmdsky-debug) (symbols)
- [armips](https://github.com/Kingcom/armips) and [skytemple-files](https://github.com/SkyTemple/skytemple-files) (for Enhancements)
- For the Windows exe: [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) and SDL2 (mingw dev package)

Point the scripts at those with the environment variables `MELONDS`, `PMDSKY_DEBUG`, `ARMIPS`, `SKYTEMPLE_FILES`,
`LLVM_MINGW` and `SDL2_MINGW`. The defaults are the paths used during development.

```sh
# 1. melonDS core (Linux build for the harness, mingw build for Windows), patched
git -C $MELONDS apply patches/melonds-recomp.patch
# 2. generate from your ROM
python3 tools/inventory.py  path/to/pmd_sky_us.nds      # -> inventory.json
python3 tools/sadie_patches.py path/to/pmd_sky_us.nds   # -> runtime/sadie_patches.h, tools/sadie_funcs.json
python3 tools/lift_all.py   path/to/pmd_sky_us.nds      # -> gen/*.c
# 3. build
bash build/build_linux.sh      # test harness
bash build-win/build_win.sh    # SkyRecomp.exe
```

## Dev keys
F4 widescreen, F11 borderless, F1 layout. Dev builds (`DEV=1 bash build-win/build_win.sh`) add F5/F9 quicksave/quickload.

## License
GPL-3.0, like melonDS and the SkyTemple patches it builds on. Pokémon Mystery Dungeon is © Nintendo / Creatures /
GAME FREAK / Spike Chunsoft. This project is not affiliated with them.
