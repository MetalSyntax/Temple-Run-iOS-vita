# Temple Run — Progreso del Port (iOS Mach-O)

Documentación completa y arquitectura en `PORTING_PLAN.md`.

## Resumen de Estado

- **Juego:** Temple Run (v1.5)
- **Plataforma Original:** iOS (Mach-O ARMv7 32-bit)
- **Motor:** Imangi Studios Custom 3D Engine (C++ / OpenGL ES 1.1 & 2.0)
- **Title ID:** `PSVTEMRUN`
- **Slug:** `templerun`
- **Build Actual:** `build/templerun.vpk` (1.0 MB) & `build/eboot.bin` (955 KB) compilados exitosamente.
- **Estado:** Listo para despliegue y pruebas directas en PS Vita física.

---

## Fases del Port

### Phase 1: Repo and tools ✅
- [x] IPA extraído (`templerun_extract/Payload/TempleRun.app`), symlink `game_app` funcional.
- [x] Binario descifrado verificado (`cryptid 0`).
- [x] `rebase.bin` generado con 8616 palabras de relocalización (`docs/macho_report.txt`).
- [x] Scripts de compilación y empaquetado `build.sh` y `CMakeLists.txt` operativos.

### Phase 2: Decompilation and inventory ✅
- [x] Dumps generados en `decompiled/macho/`:
  - `frameworks.txt`: `OpenGLES`, `OpenAL`, `AVFoundation`, `AudioToolbox`, `UIKit`, `Foundation`, `QuartzCore`, `CoreGraphics`, etc.
  - `objc_classes.txt`: `MainAppDelegate`, `MainViewController`, `EAGLView`, `Preferences`, etc.
  - `symbols.txt`: 81 clases ObjC y tabla completa de símbolos C/C++.
  - `imports.txt`: 579 imports únicos clasificados.
- [x] Funciones OpenGL ES 1.1 mapeadas a vitaGL: `glDrawElements`, `glAlphaFuncx`, `glClearColorx`, `glColorPointer`, `glCullFace`, `glDepthRangex`, `glFrontFace`, `glGetError`, `glHint`, `glMaterialx`, `glMaterialxv`, `glNormalPointer`.

### Phase 3: Real engine analysis ✅
- [x] Motor identificado: **Imangi Studios Custom 3D Engine**:
  - Clases C++: `ImangiPreferences` (`savePreferences`, `isTutorialEnabled`, `areFriendMarkersEnabled`).
  - Ciclo de vida: `_main` → `-[MainAppDelegate application:didFinishLaunchingWithOptions:]` → `MainViewController` (`startGame`, `loadSounds`, `loadCharacterSounds`).
  - Renderizado: `EAGLView` (`setupOpenGL`, `createFramebuffer`, `mainEventLoop`, `drawView`, `layoutSubviews`).
  - Audio: OpenAL, AudioQueue, `AVAudioPlayer` para música (`BackgroundMusic.mp3`) y SFX.
  - Entrada: Táctil (swipes para saltar, deslizarse y girar) e inclinación/acelerómetro para dirección.

### Phase 4: Loader and ObjC bootstrap ✅
- [x] `macho_load()` mapea segmentos en `MACHO_SLIDE = 0x98000000`, procesa `rebase.bin` y enlaza símbolos indirectos.
- [x] Soporte para `LC_MAIN` y `LC_UNIXTHREAD` integrado en `macho.c` (`macho_entry()`).
- [x] Trampolines `ml_align_call` en `asm.S` para realinear la pila de 4 a 8 bytes de forma segura.
- [x] Protección de símbolos de datos (`is_data_symbol`): asigna bloques de memoria cero (`make_unimpl_data`) a símbolos `_OBJC_CLASS_$_*`, `_OBJC_METACLASS_$_*`, `_kCF*` y RTTI vtables.
- [x] Shims POSIX y runtime C++ en `source/patch.c`: `open`, `read`, `write`, `lseek`, `fstat`, `remove`, `getenv`, `setenv`, `printf`, `fprintf`, `operator new`, `operator delete`.
- [x] Gestor de logs rotativos (`ux0:data/templerun/logs/templerun_NNN.log`).

### Phase 5: Graphics (Build ✅, Prueba de hardware en progreso)
- [x] `vendor/vitaGL` compilado en modo `SOFTFP_ABI=1` con redirección de viewport float a VFP (`sceGxmSetViewport_sfp`).
- [x] Inicialización con `vglInitExtended` en resolución 960x544.
- [x] 12 llamadas adicionales de GLES1 vinculadas en `source/gles.c`.
- [ ] Verificación en pantalla del renderizado del templo 3D y el corredor.

### Phase 6: Touch ✅
- [x] Capa de entrada en `source/input.c` con escalado y traslación de coordenadas táctiles frontales a espacio lógico.
- [ ] Verificación de interacción táctil y gestos de swipe.

### Phase 7: Physical controls ✅
- [x] Esquema de mapeo diseñado:
  - Stick Analógico Izquierdo (LX) / Cruceta Izq-Der: dirección / inclinación.
  - Cruceta Arriba / ✕ (Cruz): Salto (swipe arriba).
  - Cruceta Abajo / ○ (Círculo): Deslizarse / Barrida (swipe abajo).
  - □ (Cuadrado) / Botón L: Girar 90° a la izquierda (swipe izquierda).
  - △ (Triángulo) / Botón R: Girar 90° a la derecha (swipe derecha).
  - START: Pausa / Menú.
- [ ] Ajuste fino de respuesta de botones en hardware físico.

### Phase 8: Audio
- [x] Rutas de OpenAL, AudioQueue y MP3 identificadas.
- [x] Enrutamiento a `sceAudioOut` en `source/coreaudio.c`.
- [ ] Prueba de música y sonido de monedas en consola.

### Phase 9: Videos / FMV
- [x] Símbolos de MediaPlayer registrados.
- [ ] Salto seguro en caso de ausencia de vídeos.

### Phase 10: Assets, data, language and saves ✅
- [x] Datos de la app redirigidos a `ux0:data/templerun/TempleRun.app/`.
- [x] Partidas guardadas y configuración a `ux0:data/templerun/saves/`.
- [x] Detección de idioma del sistema mediante `sceAppUtilSystemParamGetInt`.
- [ ] Validación de guardado persistente de monedas y récords.

### Phase 11: Performance ✅
- [x] Overclock configurado por defecto en `main.c` (CPU 444 MHz, GPU 222 MHz).
- [ ] Monitoreo de tasa de cuadros en hardware real.

### Phase 12: Configurator and options
- [ ] Configuración persistente opcional para ajustes visuales y controles.

### Phase 13: Trophies
- [ ] Mapeo de logros GameKit a `sceNpTrophy`.

### Phase 14: LiveArea and VPK ✅
- [x] Configuración de `param.sfo` con TITLEID `PSVTEMRUN`.
- [x] Assets de LiveArea listos.
- [x] Generación confirmada de `eboot.bin` y paquete `templerun.vpk`.

### Phase 15: Real-hardware testing and release (Siguiente paso)
- [ ] Desplegar datos: `psvita-toolkit deploy --data`
- [ ] Desplegar VPK: `psvita-toolkit deploy`
- [ ] Ejecutar en consola PS Vita y revisar `templerun_001.log`.
