# The Castle (ASCII 1986) — MSX→C Port: Estado Técnico

## Arquitectura

Port de ROM Z80 a C99 + SDL2. La ROM original (32KB, ORG 0x4000) se carga en runtime y se usa para datos (música, scripts de sala, tiles comprimidos). La lógica del juego está completamente reescrita en C.

```
main.c          — Entry point, definición de globals, main_loop()
game.h          — Extern declarations de todos los globals compartidos
the_castle.c    — game_init(), game_reset_level(), game_loop(), update_player()
tiles.c         — Cargador de tiles: VRAM_TILES[] → VDP; tiles_load_vram_dump()
vram_tiles.c    — 185 tiles (0x00-0xB8) extraídos de Physical VRAM dumps (openMSX)
enemies.c       — sub_6F5C, update_roller(), update_bat(), sprite draw system
particles.c     — sub_61F5, sub_735F, sub_7279; sistema de partículas (16 slots)
doors.c         — update_doors(), switch, bloques, pinchos, coleccionables, llaves
room.c          — Carga de salas, intérprete de scripts bytecode (sub_5053/55F6)
music.c         — ISR de música (sub_75D4); 2 canales PSG, 96 notas, envelope
camera.c        — sub_623C, scroll_update(), render_background(), triggers de sala
title.c         — sub_4A4A: logo animado, créditos scrolling, ciclo de 3 demos
hal.h           — Interface HAL (VDP, PSG, input, vsync)
hal_sdl2.c      — Implementación SDL2: TMS9918A Screen2, AY-3-8910, teclado
CMakeLists.txt  — Build multiplataforma (Linux/macOS/Windows)
```

## MSX Hardware Portado

**VDP TMS9918A**: VRAM 16KB emulada en RAM del host. Render por software de Screen 2 (Graphics II): 32×24 tiles, paleta TMS9918A exacta (16 colores), sprites 16×16 con límite de 4 por scanline, early clock bit, magnificación. Escritura vía `hal_vdp_write_vram()`.

**PSG AY-3-8910**: Síntesis por software en callback SDL2. 2 canales de tono (A=melodía con envelope, B=bajo con vol fijo), LFSR 17 bits para ruido en canal C. Registros R0-R13 completos incluyendo envelope generator (R11/R12/R13). Tabla de 96 notas (8 octavas). Mixer inicializado con R7=0xB8 (igual que BIOS GICINI).

**Input**: Teclado mapeado a GTSTCK/GTTRIG del MSX. Cursores/WASD = dirección, Z/Space/Ctrl = fire.

**Timing**: VBlank a 60Hz (NTSC) o 50Hz (PAL). `music_isr_tick()` se llama desde `hal_wait_vsync()` emulando la ISR real del TMS9918A.

## Subsistemas del Juego Portados

| Subsistema | Sub-rutinas Z80 | Estado |
|---|---|---|
| Carga de tiles | sub_4D52, sub_64AB, sub_6CD9 | ✅ usa dumps de VRAM real |
| Tiles dinámicos por sala | tiles_load_vram_dump() | ✅ carga .bin de openMSX |
| Animación de tiles | sub_6265 | ✅ cicla ANIM_BG cada 4 frames |
| Reproductor de música | sub_75D4, sub_7615, sub_765C | ✅ ISR exacta |
| SFX (chispa, llave, muerte) | sub_76BE | ✅ 3 canales de SFX |
| Movimiento del jugador | sub_40BB, sub_41F6 | ✅ con sub-pixel y transición |
| Colisión de mapa | sub_4566, sub_49D4 | ✅ |
| Enemigos roller | sub_710B | ✅ física horizontal + rebote |
| Enemigos bat | sub_719D, sub_7279 | ✅ diagonal + estado trampa |
| Sprites draw system | sub_6A7C, sub_6AEF, sub_6EE1 | ✅ |
| Partículas/chispas | sub_61F5, sub_735F | ✅ 16 slots |
| Puertas animadas | sub_72CA, sub_72DD | ✅ |
| Switches | sub_7326, sub_7341 | ✅ modifica mapa de colisión |
| Bloques empujables | sub_4601, sub_4609 | ✅ fases caída+deslizamiento |
| Pinchos retráctiles | sub_47B8, sub_7494 | ✅ |
| Coleccionables/llaves | sub_4820, sub_43BF | ✅ 5 tipos |
| Puertas de salida | sub_4499, sub_74E9, sub_7510 | ✅ |
| Carga de salas | sub_5053, sub_5382 | ✅ |
| Scripts de sala (bytecode) | sub_55F6 | ✅ intérprete completo |
| Transición entre salas | sub_5084/94/A7/BA | ✅ BCD navigation |
| Render de mapa (10×10) | sub_63BB | ✅ |
| Triggers de sala | sub_5B96 | ✅ 8 tipos |
| Cámara/timers | sub_623C | ✅ |
| Pantalla de título | sub_4A4A | ✅ logo + créditos + demo |
| Sistema de score BCD | sub_5D87, sub_5DC0 | ✅ |

## Bugs Conocidos

**1. Main loop incorrecto** (crítico)
El loop original en 0x4016 es:
```
restart: CALL sub_6383   ; reset keyframe queue
         CALL sub_4D52   ; reset level state
inner:   CALL sub_4A4A   ; título+juego (retorna con carry si abortado)
         CALL sub_4D52   ; reset level state
         JR C, restart   ; si carry → restart completo
         CALL sub_4029   ; limpiar aux state
         JR inner        ; siguiente ciclo (juego ya comenzó dentro de 4A4A)
```
El port actual llama `title_screen()` luego `room_load_initial()` separados, rompiendo el flujo original donde el juego comienza **dentro** de `sub_4A4A`. Crashea al presionar fire y arranca directo en sala sin título.

**2. Tiles por sala incompletos**
Solo se tienen dumps de VRAM de 2 pantallas (título + sala 1). Las demás salas del castillo usan tiles que no están en `vram_tiles.c`. Se necesitan más dumps de openMSX (`debug save vram sala_N.bin`) al visitar cada sala.

**3. sub_4029 sin implementar**
Limpia variables de transposición y tempo (0xEAF1/F2/F4/F5). Sin esto el estado de música puede quedar corrupto entre ciclos de título.

**4. sub_5327 / sub_4B13 (curtain wipe)**
El efecto de cortina entre ciclos del título no está completamente sincronizado con el timing original.

**5. Scroll/cámara (sub_5B96) parcial**
`scroll_update()` en camera.c accede a `g_exit_doors[]` via `doors_find_trigger()` pero la tabla de salidas no se inicializa desde los datos de sala de la ROM — solo desde `doors_init()` que la deja vacía.

**6. room_load_initial() llama sub_51D9 incompleto**
La carga de la sala inicial no carga el script de objetos (0xEB08) ni el script de fondo (0xEB05). El intérprete de scripts `room_script_tick()` funciona pero no tiene datos que procesar.

## Para Compilar

```bash
sudo apt install libsdl2-dev cmake
cmake -B build && cmake --build build
cp The_Castle*.rom build/the_castle.rom
cp The_Castle_PhVRAM_Screen_1_Play.bin build/vram_play.bin
./build/the_castle
```
