// The system table: what DeckEmu knows how to run, and which libretro core runs it.
// Adding a system is one row here plus the core dll in cores\ -- nothing else.
#ifndef SYSTEMS_H
#define SYSTEMS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  const char* name;    // display name, also the save/state folder
  const char* core;    // dll name, looked for in cores\ beside the exe
  const char* exts;    // "|z64|n64|v64|" lowercase, pipe delimited
  const char* aliases; // folder names that mean this system, normalised (see normalise())
  const char* const (*options)[2]; // core options to pin, NULL for none
  int optionCount;
  const int* padMap; // 16 SDL_CONTROLLER_BUTTON_* -> retro id, NULL for the standard map
  float aspect;      // 0 to take whatever the core reports
} System;

const System* systems(int* count);
const System* systemByName(const char* name);

// Physical pad -> libretro id, indexed by SDL_CONTROLLER_BUTTON_* plus 15/16 for the
// analog triggers; -1 means the button does nothing. NULL system gets the standard map.
#define PADMAP_LEN 17
#define PAD_LT 15
#define PAD_RT 16
const int* systemPadMap(const System* s);

// Which system a file belongs to. The folder it sits in decides first -- half the
// library is .zip/.7z/.chd/.bin, which name no system at all -- and the extension
// only breaks the tie when it is unique to one system.
const System* systemForPath(const char* path);
const System* systemForExt(const char* ext); // NULL if unknown or shared by several

// lowercased extension without the dot, "" if none
void pathExt(const char* path, char* out, size_t n);
bool isArchiveExt(const char* ext); // zip/7z/rar: contents decide, not the wrapper
bool isRomPath(const char* path);   // worth showing in the library at all

#endif
