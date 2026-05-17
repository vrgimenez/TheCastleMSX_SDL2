# AGENTS.md — The Castle MSX → SDL2 Port

## Project

Port of *The Castle* (ASCII, 1986) MSX game from Z80 assembly to C99 with SDL2.
Entrypoint: `main.c:main()`. All game variables defined in `main.c`, declared `extern` in `game.h`.

## Build & Run

```
cmake -B build [-DPAL_TIMING=ON] [-DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON]
cmake --build build
build/the_castle [path/to/the_castle.rom]
```

Defaults: RelWithDebInfo build type. ROM is copied to `build/the_castle.rom` by CMake if present.

## Architecture

- `hal.h` — pure interface (portability layer). `hal_sdl2.c` implements it.
- Core logic (`the_castle.c`, `room.c`, `camera.c`, etc.) calls only `hal.h` + `game.h`.
- To add a platform: write `hal_<platform>.c`, add to CMakeLists.txt.

## Key Gotchas

- **ROM required at runtime.** The original 32KB `.rom` file must be available. It provides music data, room scripts, tile descriptors.
- **`music_isr_tick()` lives in `hal_wait_vsync()`**, NOT in the game loop — mimics the MSX VBlank ISR.
- **Init order matters:** `hal_init` → `tiles_load_from_rom` → `tiles_load_bios_rom` → `game_init` → `enemies_init` → `particles_init` → `doors_init` → `music_init` → `camera_init` → `main_loop`.
- **`char_to_tile` digit formula:** `chr - 0x30 + 0x1C + tile_base` (Z80 falls through `ADD 0x5D` → `SUB 0x41` → `ADD C`). Three copies: `title.c`, `camera.c` (both fixed), `room.c` (different encoding).
- **Credit text uses `tile_base=0x01`**, NOT 0x73. `draw_credit_row()` in `title.c` passes `0x01u`.
- **Title screen loads BG1_MAIN (4 tiles @ 0x8056) to VRAM 0x73-0x76** via `load_title_border_tiles()` after `intro_prepare_vram()` — the logo draws from `tile_base=0x73`.
- **`tiles_reload_walls_and_anim()`** reads from `g_tiles` (not ROM), writes WALLS 0x59-0x72 + ANIM_BG 0x47-0x50. Does NOT touch 0x73-0x76 (wall variants stay as loaded by `TILE_MAP` at init; title screen overrides them with BG1_MAIN border).
- **Stubs in `main.c`:** `update_roller_by_pos()` and `update_bat_by_slot()` are temporary wrappers in `main.c` that `doors.c` depends on.
- **Two map layers:** `g_map[0x400]` (20×30 collision map) and `g_tilemap[]` (30×30 visual map).
- **BCD room coords:** `g_room_x` uses BCD (hi-nibble=row, lo-nibble=column). Arithmetic is DAA-style, not binary.
- **Tiles loaded from ROM at runtime.** `tiles.c` reads raw 16-byte interleaved tiles from the game ROM using a 19-entry `TILE_MAP[]` table. Hardcoded `VRAM_TILES[]` and `vram_tiles.c` removed.
- **Tile data is NOT compressed.** All verified tiles in ROM are raw 16-byte format (pattern/color interleaved).
- **ROM tile descriptor table at 0x7BC0+:** First 4 entries store full 16-bit ROM addresses; entries 0x7BC8+ use compact format (hi byte = context, lo byte = stored). `load_tileset()` in `room.c` handles both.
- **All title-screen tiles verified via VRAM dump:** block A (0x77-0x87 @ 0x9116), block B (0x88-0x95 @ 0x81A6, **not** 0x9926), block C (0x96-0xB8 @ 0x8286). Title screen uses BG1_MAIN data (0x8056) at indices 0x73-0x76 for borders.
- **Gameplay wall tiles 0x73-0x76** are stored separately after the main WALLS block: 0x73-0x74 @ 0x89C6, 0x75-0x76 @ 0x8966 (not contiguous with the 26-tile WALLS range 0x59-0x72 @ 0x8796).
- **`vram_tiles.c` deleted** — removed from build.

## Controls

Arrows/WASD = move, Z/Space/Ctrl = fire, X = fire2, Esc = quit.

## Windows Build

```
cmake -B build -DSDL2_DIR="C:\SDL2\cmake"
cmake --build build --config Release
```

MSVC flags: `/W4 /WX- /wd4996`. GCC/Clang: `-Wall -Wextra -Wno-unused-parameter -Wno-unused-function`.
