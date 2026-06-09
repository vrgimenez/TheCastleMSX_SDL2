/*
 * THE CASTLE — HAL (Hardware Abstraction Layer) public interface
 * ==============================================================
 * Este header es el único punto de contacto entre la lógica del juego
 * (the_castle.c) y cualquier implementación de plataforma (hal_sdl2.c,
 * hal_wasm.c, hal_null.c, …).
 *
 * Agregar una nueva plataforma = implementar todas las funciones de abajo.
 */

#pragma once
#ifndef CASTLE_HAL_H
#define CASTLE_HAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * CICLO DE VIDA
 * ========================================================================== */

/**
 * hal_init() — Inicializa toda la plataforma.
 * @param pal_timing  true=50Hz (PAL), false=60Hz (NTSC)
 * @return true si todo fue bien, false si algo falló.
 */
bool hal_init(bool pal_timing);

/**
 * hal_quit() — Libera todos los recursos de plataforma.
 */
void hal_quit(void);

/**
 * hal_is_running() — Consulta si el usuario no ha pedido cerrar.
 * A diferencia de hal_poll_events(), no procesa eventos. Útil para
 * comprobar en loops que no pueden consumir el evento en ese momento.
 */
bool hal_is_running(void);

/**
 * hal_poll_events() — Procesa eventos del sistema (input, ventana, etc.).
 * Debe llamarse una vez por frame, antes de leer el joystick.
 * @return false si el usuario pidió cerrar la aplicación.
 */
bool hal_poll_events(void);

/* ==========================================================================
 * VDP — Video Display Processor (TMS9918A)
 * ========================================================================== */

/** Escribe un registro VDP (R0..R7). */
void    hal_vdp_write_reg(uint8_t reg, uint8_t val);

/** Escribe un byte en VRAM. */
void    hal_vdp_write_vram(uint16_t addr, uint8_t val);

/** Lee un byte de VRAM. */
uint8_t hal_vdp_read_vram(uint16_t addr);

/** Rellena un bloque de VRAM con un valor (equivale a FILVRM). */
void    hal_vdp_fill_vram(uint16_t addr, uint8_t val, uint16_t count);

/** Copia datos de RAM a VRAM (equivale a LDIRVM). */
void    hal_vdp_copy_to_vram(uint16_t dst, const uint8_t *src, uint16_t count);

/** Copia datos de VRAM a RAM (equivale a LDIRMV). */
void    hal_vdp_copy_from_vram(uint16_t src, uint8_t *dst, uint16_t count);

/** Configura el VDP en modo Screen 2 / Graphics II (equivale a INIGRP). */
void    hal_vdp_init_screen2(void);

/** Apaga la pantalla (equivale a DISSCR). */
void    hal_vdp_disable_screen(void);

/** Borra todos los sprites (equivale a CLRSPR). */
void    hal_vdp_clear_sprites(void);

/**
 * hal_vdp_present() — Renderiza el estado actual de VRAM y lo muestra.
 * Llamado automáticamente por hal_wait_vsync(), pero puede llamarse
 * manualmente si se necesita un flush inmediato.
 */
void    hal_vdp_present(void);

/* ==========================================================================
 * PSG — Programmable Sound Generator (AY-3-8910)
 * ========================================================================== */

/** Escribe un registro PSG (R0..R15) — equivale a BIOS WRTPSG. */
void    hal_psg_write(uint8_t reg, uint8_t val);

/** Lee un registro PSG — equivale a BIOS RDPSG. */
uint8_t hal_psg_read(uint8_t reg);

/* ==========================================================================
 * INPUT
 * ========================================================================== */

/**
 * hal_joystick_read() — Lee la dirección del joystick.
 * @param port  0 = joystick 1, 1 = joystick 2
 * @return Dirección (0-8) igual que BIOS GTSTCK:
 *         0=nada, 1=↑, 2=↑→, 3=→, 4=↓→, 5=↓, 6=↓←, 7=←, 8=↑←
 */
uint8_t hal_joystick_read(uint8_t port);

/**
 * hal_key_pressed() — ¿Está pulsado el botón de acción (fire/space)?
 * Equivale a BIOS GTTRIG para el botón 1 del joystick 1.
 */
bool    hal_key_pressed(void);

/**
 * hal_read_special_key() — Detecta pulsación única de teclas especiales.
 * @return 0=nada, 1=F1 (suicidio), 2=F2 (game over)
 * El evento se consume al leer (retorna 0 en sucesivas llamadas hasta
 * que se suelte y vuelva a presionar la tecla).
 */
uint8_t hal_read_special_key(void);

/**
 * hal_is_ctrl_held() — ¿Ctrl está siendo presionado?
 * En el original MSX, Ctrl mantenido duplica la velocidad.
 */
bool    hal_is_ctrl_held(void);

/**
 * hal_is_graph_held() — ¿GRAPH/Alt está presionado?
 * En MSX original: Ctrl+GRAPH = triple/quad velocidad.
 * En PC: se mapea a Alt (LALT/RALT) como proxy.
 */
bool    hal_is_graph_held(void);

/**
 * hal_read_wasd_dir() — Detecta pulsación única de WASD (teleport extra).
 * @return 0=nada, 1=W, 2=A, 3=S, 4=D
 * El evento se consume al leer.
 */
uint8_t hal_read_wasd_dir(void);

/* ==========================================================================
 * TIMING
 * ========================================================================== */

/**
 * hal_wait_vsync() — Espera al VBlank y presenta el frame.
 * El juego llama a esta función al final de cada frame.
 * La implementación debe: renderizar VRAM → mostrar → esperar hasta
 * completar el periodo de frame (16.67 ms a 60Hz / 20 ms a 50Hz).
 */
void    hal_wait_vsync(void);

/**
 * hal_delay() — Espera N frames completos (útil para pausas en cutscenes).
 */
void    hal_delay(uint8_t frames);

#ifdef __cplusplus
}
#endif

#endif /* CASTLE_HAL_H */
