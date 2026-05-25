cmake_minimum_required(VERSION 3.16)
project(TheCastle C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# =============================================================================
# Opciones de build
# =============================================================================
option(PAL_TIMING "Usar timing PAL (50Hz) en vez de NTSC (60Hz)" OFF)
option(ENABLE_ASAN "Activar AddressSanitizer (solo desarrollo)" OFF)

# =============================================================================
# Fuentes del juego (lógica pura, sin dependencias de plataforma)
# =============================================================================
set(GAME_SOURCES
    main.c          # Punto de entrada: carga ROM, inicializa HAL y subsistemas
    the_castle.c    # Loop principal, jugador, movimiento, puntuación
#   vram_tiles.c    # Datos de tiles extraídos de Physical VRAM dump (openMSX)
    tiles.c         # Carga de tiles ROM → VRAM (sub_4D52 / sub_64AB / sub_6CD9)
    enemies.c       # IA de enemigos: roller, bat, sprite draw (sub_6F5C..sub_719D)
    particles.c     # Chispas, partículas, trampa murciélago (sub_61F5 / sub_7279)
    doors.c         # Puertas, switches, bloques, pinchos, coleccionables (sub_442D)
    room.c          # Carga de salas, scripts de sala, transiciones (sub_5053)
    music.c         # Reproductor PSG AY-3-8910, ISR de música (sub_75D4 / sub_7769)
    camera.c        # Cámara, scroll, triggers de sala, render de mapa (sub_623C)
    title.c         # Pantalla de título completa con animación (sub_4A4A)
    tiledata.c      # Arrays de tiles cargados desde ROM
    screen.c        # Screen buffer + renderer desde arrays planos
)

# =============================================================================
# HAL SDL2
# =============================================================================
set(HAL_SDL2_SOURCES
    hal_sdl2.c      # VDP TMS9918A + PSG AY-3-8910 + input + vsync via SDL2
)

# =============================================================================
# Buscar SDL2
# =============================================================================
# Método preferido: paquete CMake nativo de SDL2 (SDL2 >= 2.0.12)
find_package(SDL2 QUIET)

if(NOT SDL2_FOUND)
    # Fallback: buscar con pkg-config (Linux/macOS con brew/apt)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 REQUIRED sdl2)
    endif()
endif()

if(NOT SDL2_FOUND AND NOT SDL2_LIBRARIES)
    message(FATAL_ERROR
        "SDL2 no encontrado.\n"
        "  Linux:   sudo apt install libsdl2-dev\n"
        "  macOS:   brew install sdl2\n"
        "  Windows: descarga SDL2-devel desde https://libsdl.org/\n"
        "           y setea SDL2_DIR al directorio de los .cmake\n"
    )
endif()

# =============================================================================
# Executable
# =============================================================================
add_executable(the_castle
    ${GAME_SOURCES}
    ${HAL_SDL2_SOURCES}
)

# Nota: main.c está en GAME_SOURCES porque define las variables globales
# del juego (declaradas extern en game.h). El resto de módulos las usan
# via game.h sin necesitar incluir main.c directamente.

# Includes
target_include_directories(the_castle PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# SDL2: soporte para ambas formas de linkeo (target vs variables legacy)
if(TARGET SDL2::SDL2)
    target_link_libraries(the_castle PRIVATE SDL2::SDL2main SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(the_castle PRIVATE SDL2::SDL2main SDL2::SDL2-static)
else()
    # pkg-config o variables legacy
    target_include_directories(the_castle PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(the_castle PRIVATE ${SDL2_LIBRARIES})
    target_compile_options(the_castle PRIVATE ${SDL2_CFLAGS_OTHER})
endif()

# math (necesario para síntesis PSG)
target_link_libraries(the_castle PRIVATE m)

# =============================================================================
# Definiciones de compile
# =============================================================================
if(PAL_TIMING)
    target_compile_definitions(the_castle PRIVATE CASTLE_PAL_TIMING=1)
endif()

# =============================================================================
# Flags de compilación por plataforma
# =============================================================================
if(MSVC)
    # Windows / Visual Studio
    target_compile_options(the_castle PRIVATE
        /W4             # nivel de warnings alto
        /WX-            # warnings no son errores (hay stubs pendientes)
        /wd4996         # deshabilitar "función deprecated" (sprintf, etc.)
    )
    # En Windows SDL2 necesita SDL2main que redefine main()
    if(TARGET SDL2::SDL2main)
        target_link_libraries(the_castle PRIVATE SDL2::SDL2main)
    endif()
    # Subsystem Windows para evitar la ventana de consola (release)
    set_target_properties(the_castle PROPERTIES
        WIN32_EXECUTABLE $<IF:$<CONFIG:Debug>,FALSE,TRUE>
    )
else()
    # GCC / Clang (Linux, macOS, MinGW)
    target_compile_options(the_castle PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter   # hay muchos stubs con params sin usar
        -Wno-unused-function    # funciones forward-declaradas aún sin implementar
    )

    if(ENABLE_ASAN)
        target_compile_options(the_castle PRIVATE -fsanitize=address -g)
        target_link_options(the_castle PRIVATE -fsanitize=address)
    endif()
endif()

# =============================================================================
# Optimización por config
# =============================================================================
# RelWithDebInfo es el modo recomendado para desarrollo del port:
# tiene símbolos de debug + optimización moderada (-O2)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
        "Tipo de build: Debug | Release | RelWithDebInfo | MinSizeRel" FORCE)
    message(STATUS "CMAKE_BUILD_TYPE no definido, usando RelWithDebInfo")
endif()

# =============================================================================
# Copiar la ROM al directorio de build (necesaria en runtime para cargar tiles)
# =============================================================================
set(ROM_FILE "${CMAKE_CURRENT_SOURCE_DIR}/The_Castle_-_ASCII__1986___GoodMSX___356_.rom")
if(EXISTS "${ROM_FILE}")
    configure_file("${ROM_FILE}"
        "${CMAKE_CURRENT_BINARY_DIR}/the_castle.rom" COPYONLY)
    message(STATUS "ROM copiada al directorio de build")
else()
    message(WARNING
        "ROM no encontrada en ${ROM_FILE}\n"
        "Ponla en el directorio del proyecto o ajusta la ruta en CMakeLists.txt"
    )
endif()

# =============================================================================
# Instrucciones post-build
# =============================================================================
message(STATUS "")
message(STATUS "========================================")
message(STATUS "  The Castle — MSX Port")
message(STATUS "========================================")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  PAL timing : ${PAL_TIMING}")
message(STATUS "  ASan       : ${ENABLE_ASAN}")
message(STATUS "")
message(STATUS "  Módulos: ${GAME_SOURCES} ${HAL_SDL2_SOURCES}")
message(STATUS "")
message(STATUS "  Para compilar:")
message(STATUS "    cmake -B build && cmake --build build")
message(STATUS "")
message(STATUS "  Para correr:")
message(STATUS "    ./build/the_castle [ruta/a/the_castle.rom]")
message(STATUS "")
message(STATUS "  Controles:")
message(STATUS "    Cursores/WASD = mover, Z/Space/Ctrl = acción, Esc = salir")
message(STATUS "========================================")
message(STATUS "")

# =============================================================================
# Instalación (opcional, para empaquetado)
# =============================================================================
install(TARGETS the_castle RUNTIME DESTINATION bin)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/the_castle.rom"
    DESTINATION share/the_castle
    OPTIONAL)
