// Menú de opciones de video (SELECT+START), mismo diseño que Block-Breaker-3 / Ferrari-GT-Evolution-vita: panel con la fuente 8x8
// de PSPSDK abajo de la pantalla, el juego en pausa detrás (gles_present no vuelve hasta cerrar) para ver el efecto
// en vivo. Se guarda en DATA_PATH options.txt, editable a mano:
//     mode=centered     (centered | full | tate | tatefull)
//     filter=sharp      (bilinear | sharp)
//     msaa=0            (0 | 2 | 4; se aplica al reiniciar)
//     accel=on          (on = sensor de movimiento | off = solo stick izquierdo)
#include <psp2/ctrl.h>
#include <stdio.h>
#include <string.h>
#include <vitaGL.h>

#include "gles.h"
#include "hle.h"
#include "log.h"
#include "options.h"

#ifndef PORT_2D
#define PORT_2D 0 // 1 = juego 2D, habilita el filtro "pixel" (vecino más cercano)
#endif
#define OPTIONS_PATH DATA_PATH "options.txt"

extern const unsigned char font8x8[];
extern volatile unsigned ml_alive; // uikit.c

int opt_filter = FILTER_SHARP, opt_msaa = 0, opt_msaa_active = 0, opt_accel = 1;
static int open_req;

static const char *filter_names[] = {"bilinear", "sharp", "pixel"};
static const char *filter_labels[] = {"BILINEAR (SMOOTH)", "SHARP BILINEAR", "PIXEL (NEAREST)"};
// índice = port_rotation (gles.c)
extern int port_rotation; // modo de pantalla: 0 centrado, 1 completa, 2 tate, 3 tate completa
static const char *mode_names[] = {"centered", "full", "tate", "tatefull"};
static const char *mode_labels[] = {"CENTERED", "FULL SCREEN", "TATE (ROTATED 90)", "TATE FULL SCREEN"};

void options_load(void) {
    FILE *f = fopen(OPTIONS_PATH, "r");
    if (f) {
        char line[64], s[16];
        int v;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "filter=%15s", s) == 1) {
                for (int i = 0; i < 3; i++)
                    if (!strcmp(s, filter_names[i]))
                        opt_filter = i;
            } else if (sscanf(line, "mode=%15s", s) == 1) {
                for (int i = 0; i < 4; i++)
                    if (!strcmp(s, mode_names[i]))
                        port_rotation = i;
            } else if (sscanf(line, "msaa=%d", &v) == 1 && (v == 0 || v == 2 || v == 4))
                opt_msaa = v;
            else if (sscanf(line, "accel=%15s", s) == 1)
                opt_accel = strcmp(s, "off") != 0;
        }
        fclose(f);
    }
    if (opt_filter == FILTER_PIXEL && !PORT_2D)
        opt_filter = FILTER_SHARP;
    opt_msaa_active = opt_msaa;
    l_info("options.txt: modo %s, filtro %s, MSAA %dx, acelerómetro %s", mode_names[port_rotation],
           filter_names[opt_filter], opt_msaa, opt_accel ? "sensor" : "stick");
}

static void options_save(void) {
    FILE *f = fopen(OPTIONS_PATH, "w");
    if (!f)
        return;
    fprintf(f, "mode=%s\nfilter=%s\nmsaa=%d\naccel=%s\n", mode_names[port_rotation], filter_names[opt_filter],
            opt_msaa, opt_accel ? "on" : "off");
    fclose(f);
}

int options_msaa_mode(void) {
    return opt_msaa == 4 ? SCE_GXM_MULTISAMPLE_4X : opt_msaa == 2 ? SCE_GXM_MULTISAMPLE_2X : SCE_GXM_MULTISAMPLE_NONE;
}

void options_open(void) { open_req = 1; }
int options_pending(void) { return open_req; }

// ---------------------------------------------------------------- panel

enum { ROW_MODE, ROW_FILTER, ROW_MSAA, ROW_ACCEL, ROWS };
#define PW 480
#define PH 128
static uint32_t px[PW * PH]; // ABGR (GL_RGBA/UNSIGNED_BYTE en little endian)
static GLuint tex;
static int cursor;

#define C_BG 0xFF201008
#define C_SEL 0xFF603818
#define C_TEXT 0xFFE0E0E0
#define C_DIM 0xFF909090
#define C_HEAD 0xFF40C0FF

static void fill(int y, int h, uint32_t c) {
    for (int i = y * PW; i < (y + h) * PW; i++)
        px[i] = c;
}
static void text(int x, int y, uint32_t c, const char *s) {
    for (; *s && x + 8 <= PW; s++, x += 8)
        for (int r = 0; r < 8; r++) {
            unsigned char bits = font8x8[(unsigned char)*s * 8 + r];
            for (int col = 0; col < 8; col++)
                if (bits & (0x80 >> col))
                    px[(y + r) * PW + x + col] = c;
        }
}

static void render(void) {
    static const char *labels[ROWS] = {"Screen mode", "Screen filter", "Anti-aliasing (MSAA)", "Tilt (accelerometer)"};
    char val[48];
    fill(0, PH, C_BG);
    text(16, 8, C_HEAD, "VIDEO OPTIONS");
    for (int r = 0; r < ROWS; r++) {
        int y = 28 + r * 16;
        if (r == cursor)
            fill(y - 4, 16, C_SEL);
        text(16, y, C_TEXT, labels[r]);
        if (r == ROW_MODE)
            strcpy(val, mode_labels[port_rotation]);
        else if (r == ROW_FILTER)
            strcpy(val, filter_labels[opt_filter]);
        else if (r == ROW_ACCEL)
            strcpy(val, opt_accel ? "MOTION SENSOR" : "LEFT STICK ONLY");
        else
            snprintf(val, sizeof(val), "%s%s", opt_msaa == 4 ? "4X" : opt_msaa == 2 ? "2X" : "OFF",
                     opt_msaa != opt_msaa_active ? " (RESTART)" : "");
        text(208, y, C_TEXT, val);
    }
    text(16, 100, C_DIM, "LEFT/RIGHT or X change   O save & close");
    text(16, 112, C_DIM, cursor == ROW_ACCEL ? "Left stick always overrides the sensor" : opt_msaa != opt_msaa_active ? "MSAA applies after restarting the game"
                                                    : "MSAA costs GPU time on heavy 3D scenes");
}

static void change(int row, int d) {
    if (row == ROW_MODE) {
        port_rotation = (port_rotation + d + 4) % 4;
    } else if (row == ROW_FILTER) {
        int n = PORT_2D ? 3 : 2;
        opt_filter = (opt_filter + d + n) % n;
    } else if (row == ROW_ACCEL) {
        opt_accel = !opt_accel;
    } else {
        static const int v[] = {0, 2, 4};
        opt_msaa = v[(opt_msaa / 2 + d + 3) % 3];
    }
}

static void draw_panel(void) {
    if (!tex) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    render();
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, PW, PH, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    // x2 en la franja de abajo (960x224); la proyección del present es 0..960 x 0..544 con y hacia abajo
    static const float xy[] = {0, 544 - PH * 2, 960, 544 - PH * 2, 0, 544, 960, 544};
    static const float uv[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glVertexPointer(2, GL_FLOAT, 0, xy);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static uint32_t read_pad(void) {
    SceCtrlData c;
    sceCtrlPeekBufferPositive(0, &c, 1);
    return c.buttons;
}

void options_run(void (*draw_game)(void)) {
    open_req = 0;
    uint32_t prev = read_pad(); // SELECT+START siguen apretados
    l_info("menú de opciones abierto");
    for (;;) {
        uint32_t b = read_pad(), p = b & ~prev;
        prev = b;
        if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_START))
            break;
        if (p & SCE_CTRL_UP)
            cursor = (cursor + ROWS - 1) % ROWS;
        if (p & SCE_CTRL_DOWN)
            cursor = (cursor + 1) % ROWS;
        if (p & (SCE_CTRL_RIGHT | SCE_CTRL_CROSS))
            change(cursor, 1);
        if (p & SCE_CTRL_LEFT)
            change(cursor, -1);
        ml_alive++; // watchdog: el juego queda en pausa dentro de present
        draw_panel();
        vglSwapBuffers(GL_FALSE);
        draw_game(); // modo/filtro nuevos en vivo
    }
    options_save();
    l_info("opciones: modo %s, filtro %s, MSAA %dx (activo %dx), acelerómetro %s", mode_names[port_rotation],
           filter_names[opt_filter], opt_msaa, opt_msaa_active, opt_accel ? "sensor" : "stick");
    while (read_pad()) { // no devolver el botón de cierre al juego
        vglSwapBuffers(GL_FALSE);
        draw_game();
    }
}
