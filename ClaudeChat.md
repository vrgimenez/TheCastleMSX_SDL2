¡The Castle de ASCII (1986)! Un clásico de MSX. Vamos a analizarlo.No hay herramientas de Z80 instaladas y sin red no puedo instalarlas. Escribiré un desensamblador Z80 en Python directamente.Funciona perfecto. Ahora generemos el archivo completo y también un análisis de rutinas:¡Listo! Aquí tienes el desensamblado completo de **The Castle (ASCII, 1986)** — 25.796 líneas de Z80.

Lo que incluye el archivo:

**Cabecera identificada:**
- Magic: `AB` (ROM MSX estándar)
- Entry point INIT: `0x4010` — aquí empieza la ejecución
- Tamaño: 32KB (ROM sin mapper)

**Anotaciones automáticas:**
- Todas las subrutinas etiquetadas como `sub_XXXX` con cuántas veces son llamadas
- Las llamadas al BIOS del MSX anotadas (ej: `CALL BIOS_INITXT`, `CALL BIOS_WRTPSG`)
- Las variables del Work RAM del MSX identificadas

**Subrutinas más importantes por frecuencia de llamada:**
| Dirección | Llamadas | Probable función |
|-----------|----------|-----------------|
| `0x6A7C` | 43x | Render/dibujo de sprites |
| `0x5128` | 30x | Posicionamiento VDP |
| `0x6EE1` | 22x | Lógica de movimiento |
| `0x5D5D` | 21x | Input/teclado |
| `0x64AB` | 20x | Colisiones o mapa |

**Próximo paso para portar a C:** ¿quieres que empiece analizando una subrutina específica (como el loop principal en `0x4010`) y la transcribo a C equivalente?

Vamos a analizar el loop principal y las subrutinas clave para empezar la traducción a C.Tengo suficiente contexto. Ahora generamos la traducción a C:Ahora tengo suficiente para escribir una traducción C robusta y estructurada:Compila perfecto (solo warnings de stubs pendientes). Copiamos a outputs:714 líneas de C, compila sin errores. Esto es lo que se tradujo y el estado actual del port:

**Completamente traducido a C:**

- `init_system()` — toda la inicialización de hardware (VDP, PSG, VRAM, variables de RAM)
- `reset_level_state()` — reset del nivel con los mismos valores que el original (`sub_4D52`)
- `game_loop()` — el loop principal frame-a-frame con la misma secuencia de llamadas
- `update_player()` — movimiento con tabla de direcciones, colisión, sub-píxel, animación
- `collision_check()` — lectura del mapa de tiles
- `render_map()` — volcado de tiles al VDP
- `music_tick()` — reproductor de música PSG (formato de datos decodificado)
- `score_add()` / `hiscore_check()` / `score_display()` — puntuación en BCD
- `put_tile()` / `draw_sprite()` — escritura a VRAM via HAL

**Pendiente de decodificar (marcado con TODO):**
- `update_enemies()` — `sub_6F5C` (lógica de IA de los enemigos)
- `update_doors()` — `sub_442D`
- `camera_update()` — `sub_623C`
- La HAL completa (hay que implementarla para SDL, libretro, etc.)

¿Continuamos con los enemigos (`sub_6F5C` / `sub_6A7C`) o prefieres primero implementar la HAL para SDL2 y tener algo que compile y corra en PC?

> _primero quiero la hal para sdl2 (o es mejor sdl3)_

Buena pregunta. SDL3 es el futuro pero SDL2 tiene mucho más soporte ahora mismo.Q: ¿En qué plataforma vas a desarrollar?
A: Linux

Para Linux, **SDL2** es lo más práctico hoy — está en todos los repos (`apt install libsdl2-dev`), tiene drivers de audio maduros, y el ecosistema de herramientas (debuggers, profilers) está todo probado con SDL2. SDL3 todavía no está en los repos de muchas distros.

Vamos con SDL2 entonces:Tengo lo que necesito. La HAL es sustancial — vamos a escribirla bien:

> Q: ¿En qué plataforma vas a desarrollar? \
> _A: Multiplataforma_

Bien, multiplataforma. SDL2 sigue siendo la mejor elección — corre en Linux, Windows, macOS, y hasta en consolas (Switch, PS4) con ports oficiales. La HAL que estamos escribiendo ya es multiplataforma por diseño.

Lo que sí hay que hacer bien es el **sistema de build**. Vamos a agregar un `CMakeLists.txt` que detecta la plataforma automáticamente, y el header `hal.h` que faltaba:Ahí están los cuatro archivos. Lo que entregamos:

**`hal.h`** — la interface pública. `the_castle.c` solo ve este header, nunca SDL directamente. Eso significa que en el futuro podés escribir `hal_wasm.c` para WebAssembly, `hal_switch.c` para Switch, o `hal_null.c` para tests unitarios sin tocar una línea del juego.

**`hal_sdl2.c`** — la implementación completa en ~600 líneas:
- **VDP**: VRAM de 16KB, renderizado por software del TMS9918A en Screen 2 con la paleta de colores exacta, sprites con límite de 4 por scanline y soporte de early clock bit y magnificación
- **PSG**: síntesis por software del AY-3-8910 con onda cuadrada por canal, LFSR de 17 bits para el canal de ruido, y tabla de volúmenes logarítmica
- **Input**: teclado mapeado a GTSTCK/GTTRIG del MSX (cursores+WASD, Z/Space/Ctrl para fire)
- **Vsync**: timing de 60Hz o 50Hz seleccionable con `-DPAL_TIMING=ON`

**`CMakeLists.txt`** — detecta SDL2 por tres métodos distintos (CMake nativo, pkg-config, variables legacy) y da mensajes de error claros si no lo encuentra.

Para arrancar en Linux:
```bash
sudo apt install libsdl2-dev cmake
cmake -B build && cmake --build build
./build/the_castle
```

El próximo paso lógico sería decodificar `sub_6F5C` (los enemigos) o `sub_4D0F` (la carga de tiles desde ROM al VRAM), que es lo que haría aparecer algo en pantalla. ¿Por cuál seguimos?

> _decodificame `sub_6F5C` (los enemigos)_

Vamos a leer bien esa subrutina y todo lo que llama:Tengo todo lo necesario. Ahora puedo decodificar correctamente:Compila sin errores. Acá está lo que decodifiqué y lo que encontré:

**Lo que hace `sub_6F5C` realmente** (que no era obvio hasta leer todo el árbol de llamadas):

No es solo "mover enemigos" — es el **dispatcher del sprite del jugador** en la name table del VDP. Selecciona qué frame de animación mostrar mirando 2 bits del contador de frames global (`g_state_flags & 0x03`). Los enemigos tienen sus propias rutinas (`sub_710B` para rollers, `sub_719D` para murciélagos) que son llamadas desde el loop de slots.

**Hallazgos clave del desensamblado:**

- **Dos tipos de enemigo identificados**: `0x34` (roller horizontal) y `0x36` (murciélago diagonal). Probablemente hay más — algunos slots tienen verificaciones de `CP 0x34` / `CP 0x36` pero el juego tiene al menos 4 tipos distintos según las tablas de tiles.
- **Arquitectura de tercios de pantalla**: el VDP se divide en 3 franjas de 8 filas. Cada tercio tiene su propio buffer de animación (`0xEA66`, `0xEA67`, `0xEA68`) y tabla de tiles en VRAM. Esto es una optimización clásica del TMS9918A en Screen 2.
- **`sub_6A7C` (43×)** no es un `WRTVRM` directo — es un lookup en la tabla del tercio correcto que selecciona el tile ID y luego lo escribe. Es el corazón del renderer.
- **El jugador empuja enemigos**: cuando un bloque/roller tiene el bit 4 del move_flags activo, modifica `g_player_col` directamente (lo arrastra).

**Pendiente en `enemies.c`**: `sub_61F5` (efecto de partícula al pisar roller) y `sub_7279` (trampa especial del murciélago) — ¿seguimos con esos o pasamos a cargar los tiles desde la ROM?

> _sigue con esos por favor_

Vamos a leer esas dos subrutinas completas:Tengo todo. Los datos en `0x7780+` son claramente datos de tiles (no código). Ahora escribo la traducción:Suficiente. Ahora tengo el cuadro completo. Escribo la traducción:Cero errores. Copiamos y entregamos:Compila limpio. Lo que encontré en estas dos rutinas:

**`sub_61F5` — Efecto de chispa del roller:**
El roller solo genera chispa si tiene movimiento horizontal activo (`bit 0` de `move_flags`). En modo título hay una comprobación adicional: el tile adyacente izquierdo debe tener el `bit 4` activo (indica suelo real, no espacio decorativo). La chispa se spawnea en el primer slot libre de una tabla de 16 partículas con stride=5 en `0xE43E`. La dirección (`0x04`=derecha / `0x0C`=izquierda) sigue la dirección del roller.

**`sub_735F` — Spawn de partícula individual:**
Las partículas son sprites de 4 tiles en forma de zigzag vertical (2 tiles arriba + 2 tiles 3 filas más abajo). `sub_474E` sondea los tiles adyacentes para decidir si la partícula puede seguir moviéndose o debe pararse. En modo título solo dibuja sin mover, y cuando `V_ACTIVE` se apaga borra el sprite y libera el slot.

**`sub_7279` — Trampa del murciélago:**
El murciélago entra en estado "trampa" (`type=0x36` en `anim_id`) cuando se queda sin espacio vertical. Anima un ciclo de 4 fases controlado por `g_state_flags & 0x3F` (los 6 bits bajos del contador de frames = ciclo de 64 frames):
- Fases 1-2: sprite estático en posición `row+1`
- Fase 3: sprite de 2 tiles alternando entre `D=4` y `D=8` en función del bit 0 (parpadeo rápido)
- Fase 4: borra el sprite y avanza `col+1` para el siguiente ciclo

**Descubrimiento importante:** hay un segundo mapa en `0xE496` (stride=30) distinto del que estábamos usando (`0xE000`, stride=20). El primero es el **mapa de colisión** (bits 4-5 codifican tipo de tile), el segundo probablemente es el **mapa visual**. Son dos capas separadas.

¿Seguimos con la carga de tiles desde la ROM, o prefieres decodificar las puertas y switches (`sub_442D`) para tener el juego más completo?

> _Sigue con los tiles_

Vamos a leer las rutinas de carga de tiles:Ahora tengo todo lo necesario. El formato es claro: cada tileset es un bloque de punteros de 2 bytes a datos de tile de 16 bytes (8 pares patrón/color). Escribo el loader:

> _ahora resta decodificar las puertas y switches_

Vamos a leer todo el árbol de sub_442D:Tengo todo lo necesario. Ahora escribo la traducción:Cero errores. Lo que quedó claro de este análisis:

**Arquitectura de objetos interactivos** — el juego tiene **cuatro tablas de RAM separadas**, todas con stride fijo, que se iteran cada frame con el mismo patrón `HL=base, DE=stride, B=count, DJNZ`:

| Tabla | Base RAM | Slots | Stride | Contenido |
|-------|----------|-------|--------|-----------|
| `g_objects` | `0xE43E` | 16 | 5 | Puertas, switches, bloques, pinchos |
| `g_exit_doors` | `0xE3D6` | 16 | 4 | Puertas de salida de habitación |
| `g_collectibles` | `0xE386` | 16 | 5 | Llaves, power-ups, rollers |
| `g_bat_slots` | `0xE416` | 8 | 5 | Murciélagos y llaves especiales |

**Hallazgos importantes:**

- **Dos pasadas por frame**: todos los dispatchers hacen exactamente dos pasadas sobre sus tablas. La primera actualiza estado, la segunda dibuja. Esto garantiza que no haya flickering por escribir en VRAM en medio del update.

- **Bloques empujables** (tiles `0x1C`/`0x1D`) son físicamente simulados: tienen una fase de caída por gravedad (bit 3 del state = 0) y una fase de empuje horizontal (bit 3 = 1). El jugador se mueve con el bloque si está encima — `g_player_col` se modifica directamente.

- **Switch global** (bit 5 de `g_state_flags`): cuando está activo, el switch no dibuja una animación sino que **reescribe el mapa de colisión** en `g_tilemap` con `0x10` (suelo). Hay una segunda capa del mapa en el offset `+0x258` que se limpia al mismo tiempo.

- **Animaciones de puerta** en ROM (`0x77F2`, `0x7802`): son tablas de `B×4` bytes donde cada fila de 4 bytes contiene los 4 tile IDs que forman una fila horizontal de la puerta/switch. Los frames se indexan multiplicando el frame counter por 4.

- **Pinchos** (`0x1F`) se actualizan en la misma tabla que puertas y bloques, pero tienen su propia lógica de rebote vertical con dos sub-pasadas — la primera mueve, la segunda dibuja los dos frames de animación (`D=0x00` bajado, `D=0x02` subido).

El port ahora tiene traducidos todos los sistemas principales. ¿Seguimos con el scroll/cámara (`sub_623C`), el sistema de música (`sub_7769`), o prefieres hacer un `main.c` que una todo y compile con SDL2?

