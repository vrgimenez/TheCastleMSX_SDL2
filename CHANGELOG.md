# Changelog

## 2026-06-03 — Identified sub_6EE1 as sprite-setter, not name-table write

### Fixed

- **title.c** (`intro_prepare_vram`, `intro_cleanup`): Removed incorrect blank-tile
  writes to name table row 0 cols 8-13. Z80 `sub_4B07` positions sprites 8-13 at
  pixel (Y=0, X=0) with blank pattern — it does NOT write to the name table. No
  SDL equivalent needed.

### Changed

- **README.md**: Port status table now includes `sub_6EE1` entry. Key Gotchas
  section documents `sub_6EE1` as a sprite attribute setter via BIOS `WRTVRM`
  (`CALL 0x004D`), not a name-table writer.

### Research

- **`CALL 0x004D`** confirmed as BIOS `WRTVRM` (Write VRAM) per MSX Wiki.
- **`sub_6EE1`/`sub_6EAE`**: Uses `LD HL,(GRPATR)` (sprite attribute table base at
  0xF3CD, set by INIGRP) to calculate sprite entry address = GRPATR + sprite_num*4,
  then calls WRTVRM 4 times to set Y=lo(HL), X=hi(HL), Pattern=tile*4, Color=translated_tile.
  Port reimplements this as `put_tile(col, row, tile)` writing directly to the name table
  since SDL2 has no sprite hardware — a semantic translation, not 1:1 bytecode.
- **Z80 enemies/effects render via sprites**, not name-table tiles. The port's
  direct name-table writes are functionally equivalent but architecturally different.

## 2026-05-25 — Per-third cleanup + music fix + credit strip comments

### Fixed

- **music.c** (`music_play_game`): Game music data now loaded from `0x7A73`
  (channel A) and `0x7A8F` (channel B) instead of `0x7ABE` — `0x7ABE` contains
  demo AI keyframe data, not music.

- **tiles.c**: Removed all `-Wtype-limits` warnings from `TILES_PER_TERCIO`:
  bounds checks comparing `uint8_t` against `256` were always true/false.

### Changed

- **tiles.c**: `TILES_PER_TERCIO`, `VRAM_THIRD_SIZE` defines removed. All
  VRAM write functions (`write_tile_to_vdp`, `tiles_rom_to_vram`,
  `tiles_vram_from_rom`) no longer take per-third parameters — single flat
  write only. Removed `tiles_write_range_to_thirds()` (unused).
  `tiles_dump_vram()` simplified (no per-third dump).

- **game.h**: Updated declarations for `tiles_rom_to_vram`,
  `tiles_vram_from_rom`; removed `tiles_write_range_to_thirds`.

- **room.c** (`load_tileset`): Removed `all_thirds` parameter.

- **room.c**, **title.c**: All callers updated, per-third comments cleaned up.

- **title.c**: CREDIT_STRIPS defines, array, and comments now include the
  decoded text: `[ 1985  ISAO YOSHIDA`, `[ 1986 KEISUKE IWAKURA`,
  `PRESENTED`, `BY`, `ASCII CORPORATION`.

## 2026-05-24 — Flat tile architecture + screen compositor + SDL_MapRGB palette

### Added

- **tiledata.h/c**: New module — overlay tile arrays loaded from ROM:
  `g_font[28]` (A-Z + symbols @ 0x8796), `g_digits[10]` @ 0x86F6,
  `g_title_logo[70]` @ 0x8056, `g_hud_logo[28]` @ 0x7E96+,
  `g_hud_map[28]` @ 0x84B6, `g_wall_variants[4]`, `g_door[1]`,
  `g_key_base[2]`, plus `g_keys[12]` generated at runtime for 6 ink colors.

- **screen.h/c**: New module — flat screen buffer `g_screen_buf[24][32]`
  + background tiles `g_bg_tiles[256][16]`. `screen_render()` composites
  background from these arrays. Overlay tile renderers: `screen_put_tile()`
  and `screen_put_tile_array()`.

### Changed

- **tiles.c**: `g_tiles` reduced from 768 to 256 entries (only third 0).
  `tiles_load_from_rom()` loads TILE_MAP only to third 0.
  `tiles_reload_all()` writes only third 0.
  Added `tiles_vram_from_rom()` — writes ROM data directly to VRAM
  without touching g_tiles.

- **hal_sdl2.c**: VRAM writes in pattern table (0x0000-0x17FF) and color
  table (0x2000-0x37FF) route to `g_bg_tiles[]`, ignoring the third.
  Name table writes (0x1800-0x1AFF) route to `g_screen_buf[]`.
  `vdp_render()` now calls `screen_render()` then `vdp_render_sprites()`.
  Removed dead per-third render code. Old border_rgba and sprite pixel
  packing replaced with `g_palette[color_idx]` from SDL_MapRGB.

- **title.c**: `load_title_tiles()` replaces 3 old loaders — loads
  BG1_MAIN (70 tiles, 0x73-0xB8) to all 3 tercios, font A-Z (0x01-0x1C)
  to tercios 1-2, digits (0x1D-0x26) to tercios 1-2, credit digits
  (0x5D-0x66) to tercios 1-2, credit font (0x6E-0x89) to tercios 1-2.
  Removed redundant credit re-load inside cycle loop.

- **CMakeLists.txt**: Added tiledata.c and screen.c.

### Fixed

- **hal_sdl2.c**: Palette packed via `SDL_MapRGB()` instead of hardcoded
  `0xAARRGGBB` literals — fixes pink/magenta tint on SDL_RGBA8888
  little-endian. Also applied to border color and sprite rendering.

- **tiledata.h/c**: Added 2×2 door tile system:
  - `g_door_base[4]` loaded from file offset 0x59F6 (4 tiles forming a
    2×2 door: white frame top, dark blue panel bottom).
  - `g_door_open[2]` loaded from file offset 0x5A36 (open door frame
    upper tiles, bottom 2 = blank).
  - `g_door_variants[6][4]` generated at runtime via
    `tiledata_generate_doors()` — 6 ink colors for the door panel.
  - Renamed old `g_door[1]` → `g_heart[1]` (heart/life tile at file
    0x5A76, was mislabeled as door).

- **main.c**: Init order now includes `tiledata_load_from_rom()` and
  `screen_init()` before `tiles_load_from_rom()`.

### Removed

- Per-third pattern table model abandoned — flat `g_bg_tiles[256][16]`
  with no per-third differentiation. All third-specific writes collapse
  to the same destination.

## 2026-05-18 — Z80 char_to_tile match + per-third font loading

### Fixed

- **title.c / camera.c** (`char_to_tile`): Now matches the Z80 original exactly:
  `chr - 0x30 + 0x5D` for ALL characters ≥ 0x30 (both digits and letters). The
  old formula `chr - 0x30 + 0x1C + tile_base` was incorrect — the Z80's
  `RET NC` after `ADD 0x5D` means the letter path (`SUB 0x41, ADD C`) is ONLY
  reached for chr < 0x30 (punctuation). This means:
  - `'0'..'9'` → VRAM **0x5D..0x66**
  - `'A'..'Z'` → VRAM **0x6E..0x87**

- **title.c**: Added `load_credit_digit_tiles()` — loads digit tile patterns
  from ROM **0x86F6** (same data as ANIM_BG) to VRAM 0x5D-0x66 in thirds 1-2 only.

- **title.c**: Added `load_credit_font_tiles()` — loads font letter patterns
  from ROM **0x8796** (same data as WALLS, 28 tiles) to VRAM 0x6E-0x87 in
  thirds 1-2 only. The two extra tiles (0x8936-0x8956) provide the custom
  `'['` → "(c)" and `'\'` → "?" symbols used in credits.

- **title.c** (`title_screen`): Calls both loading functions after
  `load_title_border_tiles()` so credit text renders correctly in thirds 1-2
  while third 0 retains WALLS data at the same VRAM indices.

- **tiles.c**: Removed embedded `FONT_DIGITS` const arrays (tiles 0x1B, 0x1D-0x26)
  — digits now come from ROM 0x86F6 to the correct Z80-mapped positions 0x5D-0x66.

- **main.c / game.h**: Removed `tiles_load_bios_rom()` — no external
  `msxbios.rom` needed. All tiles come from the game ROM.

### Changed

- **AGENTS.md**: Updated char_to_tile docs, added per-third credit tile map,
  documented digit source (ROM 0x86F6), removed BIOS font references.

## 2026-05-17 — Title screen VRAM fix + BIOS font + char encoding

### Added

- **tiles.c**: `tiles_reload_all()` — reloads all tiles from `g_tiles[]` to all
  3 VRAM thirds (undoes `intro_prepare_vram()` clearing).

- **tiles.c**: `tiles_load_bios_rom()` — loads MSX1 BIOS charset from external
  `msxbios.rom`, mapping letters A-Z to VRAM 0x01-0x1A and digits 0-9 to
  0x1D-0x26 for credit text rendering (`tile_base=0x01`).

- **title.c**: `load_title_border_tiles()` — loads 4 decorative border tiles
  from BG1_MAIN (ROM 0x8056) to VRAM 0x73-0x76 for the logo frame.

### Fixed

- **title.c / camera.c** (`char_to_tile`): Digit formula now correctly adds
  `tile_base`: `chr - 0x30 + 0x1C + tile_base`. The old formula
  `chr - 0x30 + 0x5D` ignored `tile_base`, causing inconsistent VRAM indices
  when `tile_base ≠ 0x41`. Matches Z80 original: `SUB 0x30` → `ADD 0x5D` →
  fall through → `SUB 0x41` → `ADD C`.

- **title.c** (`draw_credit_row`): Changed `tile_base` from `0x73` to `0x01`,
  correct for credit text.

- **title.c** (`title_screen`): Replaced `tiles_reload_walls_and_anim()` with
  `tiles_reload_all()` + `load_title_border_tiles()`. `intro_prepare_vram()`
  clears pattern/color tables for tiles ≥0x80 in third 0, and all tiles in
  thirds 1-2. The old code only restored WALLS+ANIM_BG, leaving title logo
  blocks (0x77-0xB8) and BIOS font tiles (0x01-0x26) cleared in thirds 1-2 —
  causing corrupted title logo and credit text in those screen bands.

- **tiles.c** (`TILE_MAP`): Replaced title block entries (A: 0x9116, B: 0x81A6,
  C: 0x8286) with single 66-tile logo body entry from ROM 0x8096 (tile #5+ of
  the full logo dataset at 0x8056). The entire title logo (70 tiles, 0x73-0xB8)
  is a contiguous block starting at ROM 0x8056, not three separate blocks.

- **tiles.c** (`tiles_reload_walls_and_anim`): Loop count corrected from 28 to
  26 (was writing indices 0x59-0x74, clobbering wall variant tiles 0x73-0x74).

- **main.c**: Moved `tiles_load_bios_rom()` after `tiles_load_from_rom()` so
  the BIOS font properly overwrites the game charset at indices 0x01-0x02
  instead of being overwritten by it.

### Changed

- **AGENTS.md**: Updated with gotchas on `char_to_tile` digit formula,
  credit `tile_base`, title border tile loading, and init order.
  Removed `vram_tiles.c` and `vram_tiles.h` references.

## 2025-05-16 — Tile loading rewrite

### Changed

- **tiles.c**: Replaced hardcoded `VRAM_TILES[185][16]` array (from openMSX VRAM dumps)
  with runtime loading from the game ROM file. A lookup table maps each VRAM index to
  its ROM address. Unmapped tiles fall back to BLANK.

- **room.c (load_tileset)**: Fixed tile data reading to use raw 16-byte interleaved
  format from ROM directly, instead of incorrect pointer-indirection through a
  non-existent pointer table.

### Fixed

- **tiles.c (TILE_MAP)**: Corrected title block B ROM address from `0x9926` to `0x81A6`
  (verified against title-screen VRAM dump).

- **tiles.c (TILE_MAP)**: Added missing gameplay wall tiles `0x73-0x74` at `0x89C6`
  and `0x76` at `0x8976` (these sit after the 26-tile WALLS block and were previously
  unmapped, rendering as BLANK).

### Removed

- **vram_tiles.c**: Deleted (duplicate VRAM dump data, was already commented out
  in CMakeLists.txt).

### Notes

- All tiles in ROM are raw 16-byte interleaved MSX format (8 pattern bytes +
  8 color bytes). NOT compressed as previously assumed.
- ROM descriptor table: 0x7BC0-0x7BC6 store full 16-bit addresses; 0x7BC8+ use
  compact format (context-dependent hi byte).
- **All 185 tiles now mapped** to verified ROM addresses (19-entry TILE_MAP).
- Title-screen tile blocks: A (0x77-0x87 @ 0x9116), B (0x88-0x95 @ 0x81A6),
  C (0x96-0xB8 @ 0x8286), decorative borders use BG1_MAIN (0x8056) at 0x73-0x76.
- Gameplay wall tiles: main WALLS block 0x59-0x72 @ 0x8796 (26 tiles), plus
  0x73-0x74 @ 0x89C6, 0x75-0x76 @ 0x8966 (not contiguous with main block).
