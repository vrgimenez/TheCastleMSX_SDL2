# The Castle — MSX to C Port

Port of **The Castle** (ASCII, 1986) from MSX Z80 assembly to pure C99 with SDL2.

## Port Status

 | Module | Status | Notes |
 |--------|--------|-------|
 | Full Z80 disassembly | ✅ | 25,796 lines, all instructions annotated |
 | SDL2 HAL (VDP, PSG, input, vsync) | ✅ | Cross-platform |
 | Room loading + script engine | ✅ | Complete room pipeline with script interpreter |
 | Camera + scroll | ✅ | Viewport, triggers, door transitions, death fade |
 | Title screen | ✅ | Logo animation, credit scroll, curtain wipe, demo mode |
 | Enemy AI (roller, bat) | ✅ | Full movement, patterns, drawing |
 | Doors + collectibles + traps | ✅ | 5 object types, key system, spike traps |
 | Particles + effects | ✅ | Spark, death, trap-bat animations |
 | Music engine + PSG | ✅ | Full note/period table, tempo, SFX volumes — game music fixed |
 | Tile loading from ROM | ✅ | All tiles mapped — flat g_bg_tiles[256][16] |
 | Overlay tile arrays | ✅ | Font, digits, logo, HUD elements from ROM |
 | Screen compositor | ✅ | g_screen_buf[24][32] + g_bg_tiles renderer |
 | Player movement + collision | ✅ | Complete with map collision |
 | Score + BCD | ✅ | Score add, hi-score, display (DAA simplification noted) |
 | `sub_6EE1` sprite attr setter | ✅ | Identified as sprite-positioning, not name-table write |
 | `game_loop()` / `the_castle.c` | 🚧 | Skeleton functional; several subroutines simplified vs original |
 | `update_roller_by_pos()` | 🚧 | Stub — draws tile, no enemy slot instantiation |
 | `update_bat_by_slot()` | 🚧 | Stub — draws tile, no enemy slot instantiation |

## Dependencies

### Linux
```bash
sudo apt install build-essential cmake libsdl2-dev
```

### macOS
```bash
brew install cmake sdl2
```

### Windows
1. Install [CMake](https://cmake.org/download/)
2. Download [SDL2-devel-2.x.x-VC.zip](https://libsdl.org/download-2.0.php)
3. Extract SDL2 and pass the path to cmake:
```powershell
cmake -B build -DSDL2_DIR="C:\SDL2\cmake"
cmake --build build --config Release
```

## Build & Run

```bash
cmake -B build
cmake --build build
build/the_castle [path/to/the_castle.rom]
```

The original 32 KB game ROM is required at runtime — it provides music data, room scripts, and tile descriptors.

### Build Options

```bash
# PAL (50 Hz) instead of NTSC (60 Hz)
cmake -B build -DPAL_TIMING=ON

# Debug build with AddressSanitizer
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
```

## Controls

| Keyboard | MSX Joystick |
|----------|-------------|
| Arrows / WASD | Direction |
| Z / Space / Ctrl | Fire 1 |
| X | Fire 2 |
| Escape | Quit |

## Code Architecture

```
the_castle.c   — Core game logic (platform-independent)
room.c         — Room loading, script execution, tile reload
camera.c       — Viewport, scroll, triggers, border drawing
title.c        — Title screen, logo, credits, demo
enemies.c      — Enemy AI (roller, bat)
doors.c        — Doors, collectibles, keys, traps, push blocks
particles.c    — Particle effects (sparks, death, trap-bat)
music.c        — PSG music engine
tiles.c        — Tile loading from ROM via lookup table (g_tiles[256][16])
tiledata.c     — Overlay tile arrays (font, digits, logo, HUD) loaded from ROM
screen.c       — Screen buffer (g_screen_buf) + background tiles (g_bg_tiles) + renderer
main.c         — Entry point, main loop, ROM loading
hal.h          — HAL interface (pure header)
hal_sdl2.c     — SDL2 implementation (VDP, PSG, input, timing)
CMakeLists.txt — Cross-platform build system
```

### Adding a New Platform

1. Write `hal_<platform>.c`
2. Implement all functions declared in `hal.h`
3. Add source files to `CMakeLists.txt`

### Emulated VDP Design

The TMS9918A in Screen 2 (Graphics II) mode is emulated via flat arrays,
NOT a per-third VRAM model:

- **Background tiles**: `g_bg_tiles[256][16]` — 256 tiles, 16 bytes each
  (interleaved pattern/color), all screen thirds share the same tile data.
- **Name table**: `g_screen_buf[24][32]` — 24 rows × 32 cols of tile indices.
- **Render**: Software compositing into a 256×192 RGBA framebuffer.
- **Palette**: `g_palette[16]` packed at runtime via `SDL_MapRGB()` —
  platform-safe regardless of endianness.
- **Overlay arrays**: Font, digits, logo, and HUD elements rendered directly
  from ROM-loaded tile arrays (not through the name table).
- **Sprites**: Up to 32 16×16 sprites, 4-per-scanline limit (read from
  legacy `vram[]` for sprite attr/pattern data).

### PSG Synthesis

The AY-3-8910 is synthesized with:
- Square wave per channel (A, B, C)
- 17-bit LFSR for noise channel
- Standard AY logarithmic volume table
- 512-sample audio buffer @ 44100 Hz

## Disassembly

`the_castle_disasm.asm` — Complete annotated disassembly with:
- Labels for all subroutines (`sub_XXXX`)
- MSX BIOS call annotations (`BIOS_CHPUT`, `BIOS_WRTPSG`, etc.)
- Identified MSX Work RAM variables
- Per-subroutine call counters

## Key Gotchas

- **ROM required at runtime** — provides all game data (music, rooms, tiles)
- **`music_isr_tick()` lives in `hal_wait_vsync()`**, not in the game loop — mirrors the MSX VBlank ISR
- **Two map layers**: `g_map[0x400]` (20×30 collision) and `g_tilemap[]` (30×30 visual)
- **BCD room coords**: `g_room_x` stores BCD (hi-nibble = row, lo-nibble = column)
- **Flat tile architecture**: No per-third pattern table — `g_bg_tiles[256][16]` shared across all screen thirds
- **`sub_6EE1`/`sub_6EAE` is a SPRITE attribute setter**, not a name-table writer — sets sprite `B` at pixel position `(X=H, Y=L)` via `CALL 0x004D` (BIOS `WRTVRM`). Port calls it `put_tile(col, row, tile)` as a semantic translation (SDL2 has no sprite hardware).
- **Init order matters**: `hal_init` → `tiles_load_from_rom` → `game_init` → `enemies_init` → `particles_init` → `doors_init` → `music_init` → `camera_init` → `main_loop`
- **Tiles are NOT compressed** — all verified tiles in ROM are raw 16-byte interleaved (pattern + color)
