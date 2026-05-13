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