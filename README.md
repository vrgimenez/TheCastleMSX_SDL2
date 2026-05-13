# The Castle — MSX → C Port

Port del juego **The Castle** (ASCII, 1986) de MSX a C puro con SDL2.

## Estado del port

| Módulo | Estado | Notas |
|---|---|---|
| Desensamblado Z80 completo | ✅ | 25.796 líneas, todas las instrucciones |
| HAL SDL2 (VDP, PSG, input, vsync) | ✅ | Multiplataforma |
| `init_system()` | ✅ | |
| `reset_level_state()` | ✅ | |
| `game_loop()` esqueleto | ✅ | |
| `update_player()` + colisión | ✅ | |
| `render_map()` | ✅ | stub, falta cargar tiles desde ROM |
| `music_tick()` / PSG | ✅ | formato de datos decodificado |
| `score_add()` / BCD | ✅ | |
| `update_enemies()` | 🚧 | sub_6F5C pendiente |
| `update_doors()` | 🚧 | sub_442D pendiente |
| Carga de tiles desde ROM | 🚧 | sub_4D0F (LDIRVM) pendiente |
| Camera / scroll | 🚧 | sub_623C pendiente |
| Pantalla de título | 🚧 | sub_4A4A pendiente |

## Dependencias

### Linux
```bash
sudo apt install build-essential cmake libsdl2-dev
```

### macOS
```bash
brew install cmake sdl2
```

### Windows
1. Instalar [CMake](https://cmake.org/download/)
2. Descargar [SDL2-devel-2.x.x-VC.zip](https://libsdl.org/download-2.0.php)
3. Extraer SDL2 y pasar la ruta al cmake:
```powershell
cmake -B build -DSDL2_DIR="C:\SDL2\cmake"
cmake --build build --config Release
```

## Compilar y correr

```bash
# Clonar / preparar los archivos
mkdir build && cd build
cmake ..
make -j$(nproc)
./the_castle
```

### Opciones de build

```bash
# PAL (50Hz) en vez de NTSC (60Hz)
cmake .. -DPAL_TIMING=ON

# Debug con AddressSanitizer
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
```

## Controles

| Teclado | Joystick MSX |
|---|---|
| Cursores / WASD | Dirección |
| Z / Space / Ctrl | Fire 1 |
| X | Fire 2 |
| Escape | Salir |

## Arquitectura del código

```
the_castle.c   — Lógica pura del juego (sin dependencias de plataforma)
hal.h          — Interface HAL (lo único que ve the_castle.c)
hal_sdl2.c     — Implementación SDL2 (VDP, PSG, input, timing)
CMakeLists.txt — Build system multiplataforma
```

### Agregar una nueva plataforma

1. Crear `hal_miplatforma.c`
2. Implementar todas las funciones declaradas en `hal.h`
3. Agregar las fuentes al `CMakeLists.txt`

### Diseño del VDP emulado

El TMS9918A en modo Screen 2 (Graphics II) se emula con:
- **VRAM**: 16KB en RAM del host
- **Render**: por software a un framebuffer RGBA de 256×192
- **Paleta**: 16 colores TMS9918A exactos
- **Sprites**: hasta 32 sprites de 16×16, límite de 4 por scanline

### Síntesis PSG

El AY-3-8910 se sintetiza con:
- Onda cuadrada por canal (A, B, C)
- LFSR de 17 bits para el canal de ruido
- Tabla de volúmenes logarítmica estándar del AY
- Buffer de audio de 512 samples @ 44100 Hz

## Archivos del disasm

`the_castle_disasm.asm` — Desensamblado completo con:
- Labels para todas las subrutinas (`sub_XXXX`)
- Anotaciones de llamadas al BIOS MSX (`BIOS_CHPUT`, `BIOS_WRTPSG`, etc.)
- Variables del Work RAM del MSX identificadas
- Contador de llamadas por subrutina
