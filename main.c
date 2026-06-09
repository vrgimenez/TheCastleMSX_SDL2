/*
 * THE CASTLE (ASCII, 1986) — main.c
 * ==================================
 * Punto de entrada del port. Responsabilidades:
 *
 *   1. Cargar la ROM desde disco
 *   2. Inicializar la HAL SDL2
 *   3. Cargar tiles desde la ROM a la VRAM emulada
 *   4. Inicializar todos los subsistemas del juego
 *   5. Correr el loop principal (title → game → title ...)
 *   6. Manejar cierre limpio
 *
 * Uso:
 *   ./the_castle [ruta/a/the_castle.rom]
 *   (por defecto busca "the_castle.rom" en el directorio actual)
 *
 * Compile (Linux):
 *   gcc -std=c99 -Wall -O2 \
 *       main.c the_castle.c tiles.c enemies.c particles.c doors.c hal_sdl2.c \
 *       $(sdl2-config --cflags --libs) -lm -o the_castle
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

#include "hal.h"
#include "game.h"
#include "screen.h"

/* ==========================================================================
 * CONSTANTES
 * ========================================================================== */

#define ROM_SIZE_EXPECTED  32768u          /* 32 KB — ROM estándar MSX       */
#define ROM_DEFAULT_PATH   "the_castle.rom"

/* ==========================================================================
 * VARIABLES GLOBALES DEL JUEGO
 * (Definiciones — las declaraciones extern están en game.h)
 * ========================================================================== */

uint8_t g_state_flags    = 0;
uint8_t g_anim_frame     = 0;
uint8_t g_facing         = 0;
uint8_t g_player_speed   = 0;
uint8_t g_transition     = 0;
uint8_t g_game_over      = 0;
uint8_t g_room_exit      = 0;
uint8_t g_restart_flag   = 0;
uint8_t g_intro_active   = 0;
uint8_t g_enemies_active = 0;
uint8_t g_player_col     = 0;
uint8_t g_player_row     = 0;
uint8_t g_player_x       = 0;
uint8_t g_player_y       = 0;
uint8_t g_lives          = 0;
uint8_t g_room_number    = 0;
uint8_t g_score[3]       = {0, 0, 0};
uint8_t g_hiscore[3]     = {0, 0, 0};
uint8_t g_map[0x400]     = {0};

const uint8_t *g_rom      = NULL;
uint32_t       g_rom_size = 0;

/* ==========================================================================
 * STUBS de doors.c (update_roller_by_pos, update_bat_by_slot)
 *
 * doors.c necesita llamar a funciones de enemies.c para los rollers
 * y murciélagos que aparecen en la tabla de coleccionables.
 * Aquí las implementamos como wrappers que delegan a enemies.c.
 *
 * En una versión completa se refactorizaría enemies.c para exponer
 * estas funciones directamente.
 * ========================================================================== */
void update_roller_by_pos(uint8_t col, uint8_t row, uint8_t move_flags)
{
    /*
     * Equivale a llamar sub_710B con un slot sintético.
     * Por ahora: solo dibujar el tile en la posición dada.
     * TODO: instanciar un EnemySlot temporal y llamar a la lógica real.
     */
    (void)move_flags;
    /* Tile del roller = 0x34 en la name table */
    uint16_t addr = (uint16_t)(0x1800u + (uint16_t)row * 32u + col + 1u);
    hal_vdp_write_vram(addr, 0x34u);
}

void update_bat_by_slot(uint8_t col, uint8_t row, uint8_t move_flags)
{
    /*
     * Equivale a llamar sub_719D con un slot sintético.
     * TODO: instanciar un EnemySlot temporal y llamar a la lógica real.
     */
    (void)move_flags;
    uint16_t addr = (uint16_t)(0x1800u + (uint16_t)row * 32u + col + 1u);
    hal_vdp_write_vram(addr, 0x36u);
}

/* ==========================================================================
 * CARGA DE ROM
 * ========================================================================== */

static uint8_t *load_rom(const char *path, uint32_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: no se puede abrir la ROM '%s'\n", path);
        fprintf(stderr, "  Copia the_castle.rom al directorio de trabajo\n"
                        "  o pasa la ruta como primer argumento.\n");
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz != (long)ROM_SIZE_EXPECTED) {
        fprintf(stderr, "Advertencia: la ROM tiene %ld bytes (se esperaban %u)\n",
                sz, ROM_SIZE_EXPECTED);
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fprintf(stderr, "Error: sin memoria para cargar la ROM (%ld bytes)\n", sz);
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    if (read != (size_t)sz) {
        fprintf(stderr, "Error: lectura incompleta de la ROM\n");
        free(buf);
        return NULL;
    }

    /* Verificar magic MSX: primeros 2 bytes deben ser 'A' 'B' (0x41 0x42) */
    if (buf[0] != 0x41 || buf[1] != 0x42) {
        fprintf(stderr, "Advertencia: magic bytes incorrectos (0x%02X 0x%02X, se esperaba 0x41 0x42)\n",
                buf[0], buf[1]);
        fprintf(stderr, "  El archivo puede no ser una ROM MSX válida.\n");
    }

    *size_out = (uint32_t)sz;
    printf("ROM cargada: '%s' (%u bytes)\n", path, *size_out);
    printf("  Entry point INIT: 0x%04X\n",
           (unsigned)(buf[2] | (buf[3] << 8)));

    return buf;
}

/* ==========================================================================
 * GAME FRAME (sub_4064) — Una iteración del bucle de juego por frame
 *
 * Estructura fiel al código original en 0x4064:
 *
 *   sub_4064:
 *     CALL sub_5D5D         → check title_mode flag (g_state_flags bit 0)
 *     [if not title mode: reset anim_frame, facing]
 *     CALL sub_6383         → reset keyframe queue
 *     LD A,1 → (0xEAE8)    → enemies_active = 1
 *     CALL sub_5128         → music tick + VSync
 *     XOR A → (0xEAE8)     → enemies_active = 0
 *     CALL 0x62D8           → render background + triggers
 *     CALL sub_5B96         → scroll update
 *     check g_restart_flag  → if set, return
 *     CALL sub_442D         → update doors
 *     CALL sub_434A         → update collectibles
 *     CALL sub_40BB         → update player
 *     CALL sub_6F5C         → update enemies
 *     CALL sub_4406         → update traps
 *     CALL sub_438D         → check key pickup
 *     CALL sub_4499         → check door exit
 *     CALL sub_5A2D         → update HUD
 *     check g_game_over     → if set, return
 *     check g_room_exit     → CALL sub_5053 → if NZ, return
 *     g_state_flags++
 *     CALL 0x623C           → camera/particles update
 *     JR sub_4064
 * ========================================================================== */
void game_frame(void)
{
    /* sub_5D5D: check title/demo mode bit */
    bool title_mode = (g_state_flags & 0x01u) != 0;
    if (!title_mode) {
        g_anim_frame = 0;
        g_facing     = 0;
    }

    /* sub_6383: reset keyframe queue */
    memset(g_keyframe_queue, 0xFFu, sizeof(g_keyframe_queue));

    /* enemies_active toggle around poll/music (sub_5128) */
    g_enemies_active = 1;
    if (!hal_poll_events()) { g_game_over = 1; return; }
    g_enemies_active = 0;

    /* F1: suicidio (pierde una vida y game over) / F2: perder todas las vidas */
    {
        uint8_t sk = hal_read_special_key();
        if (sk == 1) {
            if (g_lives > 0u) g_lives--;
            g_game_over = 1;
        } else if (sk == 2) {
            g_lives = 0;
            g_game_over = 1;
        }
    }

    /* WASD teleport (extra del port, solo en modo juego activo) */
    if (g_room_exit == 0u && !(g_state_flags & 0x01u)) {
        static const uint8_t WASD_TO_EXIT[5] = { 0, 1, 7, 5, 3 };
        /* W→EXIT_UP (1), A→EXIT_LEFT (7), S→EXIT_DOWN (5), D→EXIT_RIGHT (3) */
        uint8_t wasd = hal_read_wasd_dir();
        if (wasd != 0u) {
            uint8_t hi = (g_room_x >> 4) & 0x0Fu;
            uint8_t lo = g_room_x & 0x0Fu;
            bool valid = false;
            switch (wasd) {
                case 1: valid = (hi > 0);      break; /* W: arriba */
                case 2: valid = (lo > 0);      break; /* A: izquierda */
                case 3: valid = (hi < 9);      break; /* S: abajo */
                case 4: valid = (lo < 9);      break; /* D: derecha */
            }
            if (valid) g_room_exit = WASD_TO_EXIT[wasd];
        }
    }

    /* sub_62D8: render background with trigger processing */
    render_background();

    /* sub_5B96: scroll/trigger update */
    scroll_update();

    /* check restart flag (g_restart_flag) */
    if (g_restart_flag) return;

    /* update_doors (sub_442D) */
    update_doors();

    /* update_collectibles (sub_434A) */
    update_collectibles();

    /* game_loop (sub_40BB: player movement + camera) */
    game_loop();

    /* update_enemies (sub_6F5C + sub_6F27) */
    update_enemies();

    /* update_traps (sub_4406) */
    update_traps();

    /* check_key_pickup (sub_438D) */
    check_key_pickup();

    /* check_door_exit (sub_4499) */
    check_door_exit();

    /* update HUD (sub_5A2D) */
    draw_hud();

    /* check game over */
    if (g_game_over) return;

    /* check room exit → sub_5053 */
    if (g_room_exit) {
        room_transition();
        if (g_room_exit) return;
    }

    /* increment frame counter */
    g_state_flags++;

    /* camera + particles update (sub_623C) */
    camera_update();
    update_particles();
}

/* ==========================================================================
 * LOOP PRINCIPAL (sub_401C)
 *
 * Estructura fiel al código original en 0x4016:
 *
 *   restart_title:
 *     reset_keyframe_queue()    ; sub_6383
 *     game_reset_level()        ; sub_4D52
 *   inner_loop:
 *     run_title_or_game()       ; sub_4A4A  — maneja title + demo + juego
 *     game_reset_level()        ; sub_4D52
 *     if aborted → restart_title
 *     clear_aux_state()         ; sub_4029
 *     goto inner_loop
 *
 * sub_4A4A (title_screen) NO retorna hasta que el jugador muere (game over).
 * Internamente maneja: 3 ciclos de título → demo mode → juego real.
 * ========================================================================== */
static void main_loop(void)
{
    while (true) {
        /* sub_4A4A: title_screen maneja título + demo + juego completo */
        title_screen();

        /* Si el usuario cerró la ventana durante title_screen */
        if (!hal_poll_events()) break;

        /* sub_4D52: reset de nivel tras game over */
        game_reset_level();

        /* sub_4029: clear aux state */
        music_set_tempo(0u, 0u);
        music_set_transpose(0u, 0u);
    }
}

/* ==========================================================================
 * ENTRY POINT
 * ========================================================================== */
int main(int argc, char *argv[])
{
    const char *rom_path = (argc > 1) ? argv[1] : ROM_DEFAULT_PATH;

    /* --- 1. Cargar ROM --- */
    uint8_t  *rom_buf  = NULL;
    uint32_t  rom_size = 0;

    rom_buf = load_rom(rom_path, &rom_size);
    if (!rom_buf) return 1;

    /* --- 2. Inicializar HAL SDL2 --- */
#ifdef CASTLE_PAL_TIMING
    bool pal = true;
#else
    bool pal = false;
#endif
    if (!hal_init(pal)) {
        fprintf(stderr, "Error: no se pudo inicializar SDL2\n");
        free(rom_buf);
        return 1;
    }

    /* --- 3. Cargar overlay tiles desde ROM --- */


    /* --- 4. Inicializar screen buffer --- */
    screen_init();

    /* --- 5. Cargar tiles ROM → VRAM (escribe g_bg_tiles via HAL) --- */
    tiles_load_from_rom(rom_buf, rom_size);

    /* --- 6. Inicializar subsistemas --- */
    game_init();
    enemies_init();
    particles_init();
    doors_init();
    music_init();
    camera_init();

    printf("Iniciando The Castle...\n");
    printf("Controles: Cursores = mover, Space = saltar, Esc = salir\n");
    printf("          F1=suicidio F2=game over Ctrl=2x Ctrl+Alt=4x\n");
    printf("          WASD=teletransporte entre salas\n");

    /* --- 5. Loop principal --- */
    main_loop();

    /* --- 6. Cierre limpio --- */
    hal_quit();
    free(rom_buf);

    printf("Hasta la próxima.\n");
    return 0;
}
