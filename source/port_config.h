#pragma once
// Configuración propia de este juego (el resto de source/ es común a los ports iOS de la familia).
#define PORT_NAME "Temple Run"
#define PORT_SLUG "templerun"
#define PORT_EXEC "TempleRun"           // v1.5 armv6, cryptid 0
#define PORT_ROTATION 0                 // Info.plist: solo vertical (Portrait / UpsideDown)
#define PORT_STRETCH 0
#define PORT_SYSTEM_VERSION "4.3.3"     // MinimumOSVersion 3.0
#define PORT_DEVICE_MACHINE "iPhone2,1"
#define GLES_POOL_MB 16
// Inclinación lateral (monedas / esquivar a los lados): el stick izquierdo inclina el teléfono.
#define PORT_ACCEL_FROM_STICK 1
#define PORT_FONT "Cheboyga.ttf"       // sin font.ttf, coretext usa la fuente del bundle (FontLabel/ZFont)
#define PORT_SCREEN_W 320
#define PORT_SCREEN_H 480
#define PORT_SCREEN_SCALE 1.0f
