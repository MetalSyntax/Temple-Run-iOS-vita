// Parches de este juego. Cada parche se agrega con evidencia (log/dump + desensamblado) y su
// dirección del Mach-O.
#include <stddef.h>

#include "imports.h"
#include "log.h"
#include "macho.h"
#include "patch.h"

// std::filebuf dentro de ofstream/fstream: en Darwin mbstate_t va alineado a 8, así que el GCC 4.2
// de Apple pone el filebuf en +8; en VitaSDK queda en +4 (ofstream = vptr + filebuf). El juego
// llama open()/close() con ofs+8 (Mach-O 0x5f314) y close() lee el buffer como vptr -> PC basura.
// En ifstream (vptr + _M_gcount) ambos dan +8. Se corrige `this` mirando dónde está el vptr real.
extern void *filebuf_vtable[] __asm__("_ZTVSt13basic_filebufIcSt11char_traitsIcEE");
extern void *filebuf_open(void *fb, const char *name, int mode) __asm__(
    "_ZNSt13basic_filebufIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode");
extern void *filebuf_close(void *fb) __asm__("_ZNSt13basic_filebufIcSt11char_traitsIcEE5closeEv");

static void *fix_filebuf(char *fb) {
    void *vptr = &filebuf_vtable[2]; // vptr apunta tras offset_to_top y typeinfo
    if (*(void **)fb != vptr && *(void **)(fb - 4) == vptr)
        return fb - 4;
    return fb;
}

static void *ios_filebuf_open(char *fb, const char *name, int mode) {
    void *r = filebuf_open(fix_filebuf(fb), name, mode);
    return r ? fb : NULL; // devuelve `this` tal como lo pasó el juego
}

static void *ios_filebuf_close(char *fb) {
    return filebuf_close(fix_filebuf(fb)) ? fb : NULL;
}

const import_entry patch_imports[] = {
    {"__ZNSt13basic_filebufIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode", (uintptr_t)ios_filebuf_open, 0},
    {"__ZNSt13basic_filebufIcSt11char_traitsIcEE5closeEv", (uintptr_t)ios_filebuf_close, 0},
};
TABLE_END(patch)

void patch_init(void) {}
