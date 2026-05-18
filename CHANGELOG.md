# Changelog

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
