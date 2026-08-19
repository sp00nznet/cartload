// Minimal libretro host. One core at a time, one game, one thread -- enough to drive
// any of the cores in systems.c and hand Cartload a pixel buffer it can blit like any
// texture. Swapping systems means coreClose() then coreOpen() with the next dll.
#ifndef CORE_H
#define CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <SDL.h>

// Input the core reads each frame. Index corePad by RETRO_DEVICE_ID_JOYPAD_*;
// systems.c holds the physical-pad-to-here map, per system.
extern uint8_t corePad[16];
extern int16_t coreStick[2];  // left stick, x/y, -32768..32767
extern int16_t coreCStick[2]; // right stick

// `options` pins core settings we want different from the core's own defaults, as
// {key, value} pairs; everything else is answered from what the core declares.
bool coreOpen(const char* dllPath, SDL_Window* window, const char* systemDir, const char* saveDir,
              const char* const (*options)[2], int optionCount, char* err, size_t errLen);
void coreClose(void);
bool coreIsOpen(void);
const char* coreName(void);  // "mupen64plus-next 2.7", once the dll is open
bool coreNeedFullpath(void); // core reads the file itself; hand it a real path
extern bool coreVerbose;     // pass the core's chatter through, not just its warnings

// `data` may be NULL, in which case the core is given `path` to read for itself.
bool coreLoadGame(const char* path, const void* data, size_t size, char* err, size_t errLen);
void coreUnloadGame(void);
bool coreGameLoaded(void);

void coreRunFrame(void);
float coreFrameTime(void); // seconds per frame, from the rom's tv standard
float coreAspect(void);    // display aspect the core asks for, 4:3 if it says nothing

// Last frame the core produced: tightly packed rows, bottom-up if coreFlipped().
const void* corePixels(int* w, int* h);
bool coreFlipped(void);
Uint32 corePixelFormat(void); // SDL format of the corePixels() rows

int coreStateSize(void);
bool coreSaveState(void* buf, int size);
bool coreLoadState(const void* buf, int size);
void coreReset(void);

#endif
