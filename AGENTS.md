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

## Color / Pixel Format

- **`g_palette[16]`** in `screen.c` is packed at runtime via `SDL_MapRGB()` in `hal_sdl2.c:hal_init()` — never hardcode `0xAARRGGBB` literals for the palette. SDL_PIXELFORMAT_RGBA8888 on LE needs byte order R,G,B,A (uint32_t = R | (G<<8) | (B<<16) | (A<<24), i.e. `0xAABBGGRR`), but always use `SDL_MapRGB`/`SDL_MapRGBA` to be platform-safe.
- Sprites and border color also use `g_palette[color_idx]` directly, not manual packing.

## Key Gotchas

- **ROM required at runtime.** The original 32KB `.rom` file must be available. It provides music data, room scripts, tile descriptors.
- **`music_isr_tick()` lives in `hal_wait_vsync()`**, NOT in the game loop — mimics the MSX VBlank ISR.
- **`sub_6EE1`/`sub_6EAE` is a SPRITE attribute setter**, not a name-table writer. It sets sprite `B` at pixel position `(X=H, Y=L)` via BIOS `WRTVRM` (0x004D). The port translates this to `put_tile(col, row, tile)` because SDL2 has no sprite hardware. Enemies, effects and blanking in the Z80 all use sprites; the port writes directly to the name table instead.
- **`CALL 0x004D`** = BIOS `WRTVRM` (MSX Wiki confirmed). Takes HL=VRAM address, A=value. Used extensively by `sub_6EAE` for sprite attribute writes.
- **`LD HL,(0xF3CD)`** loads `GRPATR` (sprite attribute table base, set by INIGRP/SETGRP at init). Used in `sub_6EAE` to calculate sprite entry address.
- **`sub_4B07`** in Z80: positions sprites 8-13 at pixel (Y=0, X=0) with blank pattern — does NOT write to name table. No SDL equivalent (commented out in `intro_prepare_vram()` and `intro_cleanup()`).
- **Init order matters:** `hal_init` → `tiles_load_from_rom` → `game_init` → `enemies_init` → `particles_init` → `doors_init` → `music_init` → `camera_init` → `main_loop`.
- **`char_to_tile` Z80 formula (sub_62B0):** `chr - 0x30 + 0x5D` for ALL chr ≥ 0x30 (digits AND letters). `RET NC` means the letter case (`SUB 0x41, ADD C`) is ONLY for chr < 0x30 (punctuation). Two copies: `title.c:char_to_tile()` and `camera.c:camera_draw_string()` — both now match the Z80. `room.c` room scripts use a DIFFERENT encoding scheme.
- **Credit text tile map (Z80 char_to_tile, thirds 1-2):**
  - `'0'..'9'` → VRAM tiles **0x5D..0x66**
  - `'A'..'Z'` → VRAM tiles **0x6E..0x87**
  - `'['` → VRAM tile **0x88** (shows as "(c)" — custom edit)
  - `'\'` → VRAM tile **0x89** (shows as "?" — custom edit)
  - Digits loaded from ROM **0x86F6** (same data as ANIM_BG) by `load_credit_digit_tiles()` in title.c
  - Letters loaded from ROM **0x8796** (28 tiles, same data as font/WALLS) by `load_credit_font_tiles()` in title.c — count=28 includes the two custom symbol tiles at 0x8936 (`[` and `\`)
  - Both written to VRAM thirds 1-2 ONLY, leaving third 0's WALLS data intact
- **No embedded digit patterns.** The old `FONT_DIGITS` const arrays in `tiles.c` removed — digits come from ROM 0x86F6.
- **Title screen loads BG1_MAIN (4 tiles @ 0x8056) to VRAM 0x73-0x76** via `load_title_border_tiles()` after `intro_prepare_vram()` — the logo draws from `tile_base=0x73`.
- **`tiles_reload_walls_and_anim()`** reads from `g_tiles` (not ROM), writes WALLS 0x59-0x72 + ANIM_BG 0x47-0x50. Does NOT touch 0x73-0x76 (wall variants stay as loaded by `TILE_MAP` at init; title screen overrides them with BG1_MAIN border).
- **Stubs in `main.c`:** `update_roller_by_pos()` and `update_bat_by_slot()` are temporary wrappers in `main.c` that `doors.c` depends on.
- **Two map layers:** `g_map[0x400]` (20×30 collision map) and `g_tilemap[]` (30×30 visual map).
- **BCD room coords:** `g_room_x` uses BCD (hi-nibble=row, lo-nibble=column). Arithmetic is DAA-style, not binary.
- **Tiles loaded from ROM at runtime.** `tiles.c` reads raw 16-byte interleaved tiles from the game ROM using `TILE_MAP[]`. Hardcoded `VRAM_TILES[]` and `vram_tiles.c` removed.
- **Tile data is NOT compressed.** All verified tiles in ROM are raw 16-byte format (pattern/color interleaved).
- **ROM tile descriptor table at 0x7BC0+:** First 4 entries store full 16-bit ROM addresses; entries 0x7BC8+ use compact format (hi byte = context, lo byte = stored). `load_tileset()` in `room.c` handles both.
- **Title logo (70 tiles, 0x73-0xB8) comes entirely from ROM 0x8056.** First 4 tiles (0x73-0x76) are the decorative border; remaining 66 tiles (0x77-0xB8, from ROM 0x8096) form the logo body. The same ROM data is used at VRAM 0x27-0x42 as BG1_MAIN tileset during gameplay (first 28 tiles only).
- **Gameplay wall tiles 0x73-0x76** are stored separately after the main WALLS block: 0x73-0x74 @ 0x89C6, 0x75-0x76 @ 0x8966 (not contiguous with the 26-tile WALLS range 0x59-0x72 @ 0x8796). These are loaded to `g_tiles[]` at init, but VRAM is overwritten by the logo border during the title screen.
- **Per-third tile model:** tiles are loaded per-screen, not per `TILE_MAP`. Each of the 3 screen thirds can have different tile data at the same VRAM index. `TILE_MAP` only provides the "default" data for third 0; title screen and room loads write to individual thirds independently.
- **No external BIOS ROM dependency.** `tiles_load_bios_rom()` has been removed. All tiles come from the game ROM.
- **`vram_tiles.c` deleted** — removed from build.

## Controls

Arrows/WASD = move, Z/Space/Ctrl = fire, X = fire2, Esc = quit.

## Windows Build

```
cmake -B build -DSDL2_DIR="C:\SDL2\cmake"
cmake --build build --config Release
```

MSVC flags: `/W4 /WX- /wd4996`. GCC/Clang: `-Wall -Wextra -Wno-unused-parameter -Wno-unused-function`.

## HUD Tile Mapping (Pattern Table 0x00-0x72)

| Range | Count | Description |
|-------|-------|-------------|
| 0x00 | 1 | Blank |
| 0x01-0x0C | 12 | Key icons |
| 0x0D | 1 | Heart (life icon) |
| 0x0E-0x29 | 28 | Map graphic (7×4) |
| 0x2A-0x45 | 28 | Logo graphic (7×4, not title screen 14×5) |
| 0x46 | 1 | Vertical separator line (col 31, all 4 rows) |
| 0x47-0x50 | 10 | Digits 0-9 (ANIM_BG) |
| 0x51 | 1 | "Hi" label (HiScore) |
| 0x52-0x54 | 3 | "SCORE" label |
| 0x55-0x56 | 2 | "Key" label |
| 0x57-0x58 | 2 | "Life" label |
| 0x59-0x72 | 26 | Letters A-Z (WALLS/font, used as dynamic overlays) |

**Name table layout (rows 0-3):**
- Row 0: `blk SCORE blk blk blk Hi SCORE blk blk blk blk [MAP row 1] [LOGO row 1] |`
- Row 1: `blk ... [MAP row 2] *NN* *OO* [MAP end] [LOGO row 2] |`
- Row 2: `blk Key ... [MAP row 3] *MM* *AA* *PP* [MAP end] [LOGO row 3] |`
- Row 3: `blk Life ... [MAP row 4] [LOGO row 4] |`

**`draw_hud()` in camera.c** now replicates Z80 sub_4E0C + sub_64C3 logic:
- `hud_fill_rect()` = sub_64C3: writes incrementing tiles to a rectangle
- Calls in order: MAP(17,0,7,4,0x0E), LOGO(24,0,7,4,0x2A), separator col31 rows0-3, SCORE(1,0,3,1,0x52), HiSCORE(9,0,4,1,0x51), Key(1,2,2,1,0x55), Life(1,3,2,1,0x57)
- Then dynamic overlays: 6 score digits using 0x47-0x50, key icons 0x01-0x0C at row2 col3+, hearts 0x0D at row3 col3+
- Tiles 0x00-0x72 in **tercio 0 never change** — loaded once from ROM via TILE_MAP

**Init order:** `hal_init` → `tiles_load_from_rom` → `game_init` → `enemies_init` → `particles_init` → `doors_init` → `music_init` → `camera_init` → `main_loop`
