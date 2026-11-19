#pragma once
// Opciones de video del port (menú SELECT+START, guardadas en DATA_PATH options.txt).

enum { FILTER_BILINEAR, FILTER_SHARP, FILTER_PIXEL };
extern int opt_filter;      // escalado del FBO del juego a la pantalla
extern int opt_msaa;        // 0 = off, 2 = 2x, 4 = 4x (se aplica al reiniciar: vitaGL lo fija en vglInit)
extern int opt_msaa_active; // el valor con que arrancó vitaGL
extern int opt_accel;       // 1 = sensor de movimiento de la Vita (+ stick), 0 = solo stick

void options_load(void);              // antes de vglInitExtended (también fija el modo de pantalla)
int options_msaa_mode(void);          // SceGxmMultisampleMode para vglInitExtended
void options_open(void);              // input.c: SELECT+START
int options_pending(void);
void options_run(void (*draw_game)(void)); // gles_present: bucle modal con el juego en pausa detrás
