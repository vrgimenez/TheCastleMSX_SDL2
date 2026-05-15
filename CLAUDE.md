# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Port of **The Castle** (ASCII Corporation, 1986) from MSX/Z80 to C99 + SDL2. The source of truth is the Z80 disassembly in `the_castle_disasm.asm` (25,796 lines). All C code must preserve the original function call order from the disassembly — do not reorganize logic or restructure subsystems arbitrarily.

The ROM (`The_Castle_-_ASCII__1986___GoodMSX___356_.rom`, 32 KB) stores tile data in compressed form. Physical VRAM dumps from OpenMSX are used in `vram_tiles.c` to bypass this compression. More VRAM dumps will be needed as new rooms, enemies, and items are implemented.

The full prior development conversation is in `ClaudeChat.md` — consult it when context on a specific subsystem's reverse-engineering is needed.

## Build

**Requirements:** CMake ≥ 3.16, C99 compiler, SDL2 dev libraries.

```bash
# Linux
sudo apt install build-essential cmake libsdl2-dev

# macOS
brew install cmake sdl2

# Build (all platforms)
cmake -B build
cmake --build build

# Run (ROM is auto-copied to build dir)
./build/the_castle
```

**Windows (MSVC):**
```powershell
cmake -B build -DSDL2_DIR="C:\SDL2\cmake"
cmake --build build --config Release
```

**Build options:**
```bash
cmake -B build -DPAL_TIMING=ON          # 50 Hz PAL instead of 60 Hz NTSC
cmake -B build -DENABLE_ASAN=ON         # AddressSanitizer (debug)
cmake -B build -DCMAKE_BUILD_TYPE=Debug # full debug symbols
```

Default build type is `RelWithDebInfo` (-O2 + debug symbols), recommended for porting work.

## Architecture

```
the_castle.c   — Player physics, BCD scoring, main game_loop()
tiles.c        — ROM → VRAM tile loading (sub_4D52, sub_64AB, sub_6CD9)
enemies.c      — AI: rollers (horizontal), bats (diagonal), sprite draw
particles.c    — Spark trails on roller contact, bat trap animations
doors.c        — Doors, switches, pushable blocks, spikes, collectibles
room.c         — Room script interpreter, room loading, transitions
music.c        — PSG AY-3-8910 sequencer (note bytecode + ISR-style tick)
camera.c       — Scroll, room trigger detection, HUD render
title.c        — Animated title screen (spiral reveal + credits scroll)
vram_tiles.c   — Pre-extracted tile patterns from emulator VRAM dumps
hal_sdl2.c     — SDL2 HAL: TMS9918A software render + AY-3-8910 synthesis
hal.h          — Platform-agnostic HAL contract (VDP, PSG, input, timing)
game.h         — Shared game state (extern globals) + inter-module API
main.c         — Entry point: ROM load, subsystem init, main loop
```

### Module communication

All modules share state through `game.h` extern globals and call the HAL exclusively through `hal.h`. No module includes another module's `.c` file.

Key globals (defined in `main.c`, declared extern in `game.h`):
- `g_player_col`, `g_player_row` — tile-based player position
- `g_map[0x400]` — 20×30 collision/tile map (bits encode tile ID + collision type)
- `g_state_flags` — frame counter; `& 0x03` = 4-frame animation index
- Object tables: `g_objects[16]`, `g_bat_slots[8]`, `g_collectibles[16]`, `g_exit_doors[16]` — all fixed-stride arrays iterated per frame

### HAL interface

`hal.h` is the only platform dependency visible to game modules. To port to a new platform, implement all functions in `hal.h` in a new `hal_<platform>.c` and add it to `CMakeLists.txt`.

HAL covers: `hal_vdp_write_vram()`, `hal_vdp_write_reg()`, `hal_psg_write()`, `hal_psg_read()`, `hal_joystick_read()`, `hal_key_pressed()`, `hal_wait_vsync()`, `hal_delay()`.

### VDP emulation (hal_sdl2.c)

TMS9918A Screen 2 (Graphics II) in software:
- 16 KB VRAM: pattern table 0x0000–0x17FF, color table 0x2000–0x37FF, name table 0x1800–0x1AFF
- Software render to 256×192 RGBA framebuffer, exact 16-color TMS9918A palette
- Up to 32 sprites (16×16), 4-per-scanline limit enforced

### PSG synthesis (hal_sdl2.c)

AY-3-8910 emulated at 44100 Hz, 512-sample buffers:
- Square wave per channel (A, B, C), 17-bit LFSR noise
- Logarithmic volume table matching AY spec
- Envelope generator (attack/decay/hold/alternate modes)

### Tile loading

ROM tiles are compressed — direct decompression from ROM is not yet implemented. Current strategy: `vram_tiles.c` holds patterns extracted from physical VRAM dumps (OpenMSX). When new rooms or sprites are needed, dump VRAM from OpenMSX and add the relevant tile patterns to `vram_tiles.c`. The `tiles.c` module handles routing between ROM data and VRAM dump data.

### ROM layout

- 32 KB cartridge mapped at 0x4000–0xBFFF
- Magic header: 0x41 0x42 ("AB")
- Music data: compact bytecode (note + duration flag)
- Room scripts: text-based with control codes

## Disassembly reference

`the_castle_disasm.asm` labels: `sub_XXXX` for subroutines, `BIOS_CHPUT` / `BIOS_WRTPSG` etc. for MSX BIOS calls, identified Work RAM variables. When implementing a C function, find the corresponding `sub_XXXX` label and follow its call sequence exactly.

## Controls

| Key | MSX joystick |
|-----|-------------|
| Arrows / WASD | Direction |
| Z / Space / Ctrl | Fire 1 |
| X | Fire 2 |
| Escape | Quit |
