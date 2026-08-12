#include "systems.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "libretro.h"

// The default is the map every other frontend uses: the face buttons go by position, so
// an Xbox pad lands on a SNES pad the right way up.
// clang-format off
static const int PAD_STANDARD[PADMAP_LEN] = {
    RETRO_DEVICE_ID_JOYPAD_B,     // A
    RETRO_DEVICE_ID_JOYPAD_A,     // B
    RETRO_DEVICE_ID_JOYPAD_Y,     // X
    RETRO_DEVICE_ID_JOYPAD_X,     // Y
    RETRO_DEVICE_ID_JOYPAD_SELECT,// BACK
    -1,                           // GUIDE
    RETRO_DEVICE_ID_JOYPAD_START, // START
    -1, -1,                       // LEFTSTICK, RIGHTSTICK (menu, handled in main.c)
    RETRO_DEVICE_ID_JOYPAD_L,     // LEFTSHOULDER
    RETRO_DEVICE_ID_JOYPAD_R,     // RIGHTSHOULDER
    RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
    RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
    RETRO_DEVICE_ID_JOYPAD_L2,    // left trigger
    RETRO_DEVICE_ID_JOYPAD_R2,    // right trigger
};

// The N64 core remaps libretro to the N64 pad on a fixed table of its own (B->A, Y->B,
// A->C-down, X->C-up, L->C-left, R->C-right, L2->Z, SELECT->L, R2->R). So the face
// buttons already land right; only the shoulders and Z need moving, and either trigger
// is Z because that is the button N64 games lean on hardest.
static const int PAD_N64[PADMAP_LEN] = {
    RETRO_DEVICE_ID_JOYPAD_B,      // A     -> N64 A
    RETRO_DEVICE_ID_JOYPAD_A,      // B     -> C-down
    RETRO_DEVICE_ID_JOYPAD_Y,      // X     -> N64 B
    RETRO_DEVICE_ID_JOYPAD_X,      // Y     -> C-up
    -1, -1,
    RETRO_DEVICE_ID_JOYPAD_START,
    -1, -1,
    RETRO_DEVICE_ID_JOYPAD_SELECT, // LB    -> N64 L
    RETRO_DEVICE_ID_JOYPAD_R2,     // RB    -> N64 R
    RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
    RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
    RETRO_DEVICE_ID_JOYPAD_L2,     // LT    -> Z
    RETRO_DEVICE_ID_JOYPAD_L2,     // RT    -> Z
};
// clang-format on

// Only what we want different from the core's own defaults; everything else is answered
// from the defaults the core declares (see core.c).
static const char* const OPT_N64[][2] = {
    {"mupen64plus-cpucore", "dynamic_recompiler"}, // the interpreters are unplayable here
    {"mupen64plus-rdp-plugin", "gliden64"},        // the Vulkan default wants a context we lack
    {"mupen64plus-43screensize", "640x480"},       // battery-sane on a 7" panel
    {"mupen64plus-169screensize", "960x540"},
    {"mupen64plus-ThreadedRenderer", "False"}, // we hand the core exactly one GL context
    // GLideN64 writes its shader cache out during unload by querying GL. Against a
    // context it does not like, those queries come back as garbage and it writes a
    // multi-gigabyte file -- 3.9GB here -- which the next launch tries to read back.
    // Off entirely: the cost is a few seconds of shader compilation on first boot.
    {"mupen64plus-EnableShadersStorage", "False"},
};
static const char* const OPT_PSP[][2] = {
    {"ppsspp_internal_resolution", "480x272"}, // 1x: an APU is not going to do better
    {"ppsspp_frameskip", "1"},
};
static const char* const OPT_DC[][2] = {
    {"reicast_internal_resolution", "640x480"},
};
static const char* const OPT_PSX[][2] = {
    {"swanstation_GPU_Renderer", "Software"}, // hw renderers want a context per backend
};
static const char* const OPT_MAME[][2] = {
    {"mame_softlists_enable", "enabled"},  // MESS side: computers boot from software lists
    {"mame_softlists_auto_media", "enabled"},
    {"mame_boot_from_cli", "enabled"},
};
#define OPT(x) x, (int) (sizeof(x) / sizeof(x[0]))
#define NOOPT NULL, 0

// Extensions listed here must be ones that *identify* the system. Shared ones
// (bin/iso/cue/chd/rom/zip) are deliberately left out of most rows: they resolve by
// folder name instead, which is how the library is actually organised.
static const System SYS[] = {
    // ponytail: nestopia and stella2014 over the more accurate mesen and stella. Both of
    // those refused every rom on this shelf out of the box; these two take them all and
    // are lighter on the battery. Revisit if a game turns out to need the accuracy.
    {"Nintendo NES", "nestopia_libretro.dll", "|nes|fds|unf|unif|",
     "|nes|nintendones|famicom|nintendofamicom|nintendoentertainmentsystem|", NOOPT, NULL, 0},
    {"Super Nintendo", "snes9x_libretro.dll", "|smc|sfc|fig|swc|bs|",
     "|snes|supernintendo|nintendosnes|superfamicom|", NOOPT, NULL, 0},
    {"Nintendo 64", "mupen64plus_next_libretro.dll", "|z64|n64|v64|",
     "|n64|nintendo64|", OPT(OPT_N64), PAD_N64, 4.0f / 3.0f},
    {"Game Boy", "gambatte_libretro.dll", "|gb|dmg|",
     "|gameboy|nintendogameboy|gb|", NOOPT, NULL, 0},
    {"Game Boy Color", "gambatte_libretro.dll", "|gbc|",
     "|gameboycolor|nintendogameboycolor|gbc|", NOOPT, NULL, 0},
    {"Game Boy Advance", "mgba_libretro.dll", "|gba|srl|",
     "|gba|gameboyadvance|nintendogameboyadvance|", NOOPT, NULL, 0},
    {"Virtual Boy", "mednafen_vb_libretro.dll", "|vb|vboy|",
     "|virtualboy|nintendovirtualboy|", NOOPT, NULL, 0},
    {"Sega Master System", "genesis_plus_gx_libretro.dll", "|sms|",
     "|mastersystem|segamastersystem|sms|", NOOPT, NULL, 0},
    {"Sega Game Gear", "genesis_plus_gx_libretro.dll", "|gg|",
     "|gamegear|segagamegear|gg|", NOOPT, NULL, 0},
    {"Sega Genesis", "genesis_plus_gx_libretro.dll", "|md|gen|smd|",
     "|genesis|segagenesis|megadrive|segamegadrive|segamegadrivegenesis|", NOOPT, NULL, 0},
    {"Sega CD", "genesis_plus_gx_libretro.dll", "",
     "|segacd|megacd|segamegacd|", NOOPT, NULL, 0},
    {"Sega 32X", "picodrive_libretro.dll", "|32x|",
     "|32x|sega32x|", NOOPT, NULL, 0},
    {"Sega Saturn", "mednafen_saturn_libretro.dll", "",
     "|saturn|segasaturn|", NOOPT, NULL, 0},
    {"Dreamcast", "flycast_libretro.dll", "|gdi|cdi|",
     "|dreamcast|dc|segadreamcast|", OPT(OPT_DC), NULL, 0},
    {"PlayStation", "swanstation_libretro.dll", "|pbp|m3u|",
     "|psx|playstation|sonyplaystation|ps1|", OPT(OPT_PSX), NULL, 0},
    {"PSP", "ppsspp_libretro.dll", "|cso|",
     "|psp|sonypsp|playstationportable|", OPT(OPT_PSP), NULL, 0},
    {"Nintendo DS", "melonds_libretro.dll", "|nds|",
     "|ds|nintendods|nds|", NOOPT, NULL, 0},
    {"Nintendo 3DS", "citra_libretro.dll", "|3ds|cia|cci|cxi|",
     "|3ds|nintendo3ds|", NOOPT, NULL, 0},
    {"TurboGrafx-16", "mednafen_pce_libretro.dll", "|pce|",
     "|pcengine|turbografx|turbografx16|necturbografx16|tg16|", NOOPT, NULL, 0},
    {"SuperGrafx", "mednafen_supergrafx_libretro.dll", "|sgx|",
     "|supergrafx|necsupergrafx|", NOOPT, NULL, 0},
    {"3DO", "opera_libretro.dll", "",
     "|3do|panasonic3do|panasonic3do1|", NOOPT, NULL, 0},
    {"Atari 2600", "stella2014_libretro.dll", "|a26|",
     "|atari2600|2600|", NOOPT, NULL, 0},
    {"Atari 5200", "a5200_libretro.dll", "|a52|",
     "|atari5200|5200|", NOOPT, NULL, 0},
    {"Atari 7800", "prosystem_libretro.dll", "|a78|",
     "|atari7800|7800|", NOOPT, NULL, 0},
    {"Atari 8-bit", "atari800_libretro.dll", "|atr|xex|atx|",
     "|atari8bit|atari8bittosec|atari800|", NOOPT, NULL, 0},
    {"Atari Lynx", "handy_libretro.dll", "|lnx|",
     "|lynx|atarilynx|", NOOPT, NULL, 0},
    {"Atari Jaguar", "virtualjaguar_libretro.dll", "|j64|jag|",
     "|jaguar|atarijaguar|", NOOPT, NULL, 0},
    {"WonderSwan", "mednafen_wswan_libretro.dll", "|ws|wsc|",
     "|wonderswan|bandaiwonderswan|bandaiwonderswancolor|", NOOPT, NULL, 0},
    {"Neo Geo Pocket", "mednafen_ngp_libretro.dll", "|ngp|ngc|",
     "|neogeopocket|ngp|", NOOPT, NULL, 0},
    {"Commodore 64", "vice_x64_libretro.dll", "|d64|t64|prg|crt|tap|",
     "|c64|commodore64|", NOOPT, NULL, 0},
    {"Amiga", "puae_libretro.dll", "|adf|dms|ipf|hdf|uae|lha|",
     "|amiga|commodoreamiga|", NOOPT, NULL, 0},
    {"MSX", "bluemsx_libretro.dll", "|dsk|mx1|mx2|col|",
     "|msx|colecovision|", NOOPT, NULL, 0},
    {"DOS", "dosbox_pure_libretro.dll", "|conf|dosz|",
     "|dos|msdos|pcdos|", NOOPT, NULL, 0},
    {"Arcade", "fbneo_libretro.dll", "",
     "|arcade|arcadefightcade|fbneo|neogeo|snkneogeo|", NOOPT, NULL, 0, true},
    // MAME is the MESS end of the deal: the machines nothing else emulates -- Apple II,
    // the Macs, the TI, the odd Sega board -- all live behind this one core.
    {"MAME", "mame_libretro.dll", "",
     "|mame|mamechd|appleii|apple2|applemacintosh|macintosh|ti|computers|segamodel2|segamodel3|"
     "pioneerlaseractive|segachanneldata|", OPT(OPT_MAME), NULL, 0, true},
};
#define SYS_COUNT ((int) (sizeof(SYS) / sizeof(SYS[0])))

const int* systemPadMap(const System* s) {
  return (s != NULL && s->padMap != NULL) ? s->padMap : PAD_STANDARD;
}

const System* systems(int* count) {
  if(count) *count = SYS_COUNT;
  return SYS;
}

const System* systemByName(const char* name) {
  for(int i = 0; i < SYS_COUNT; i++) {
    if(_stricmp(SYS[i].name, name) == 0) return &SYS[i];
  }
  return NULL;
}

void pathExt(const char* path, char* out, size_t n) {
  const char* dot = strrchr(path, '.');
  const char* slash = strrchr(path, '\\');
  out[0] = '\0';
  if(dot == NULL || (slash != NULL && dot < slash)) return;
  size_t i = 0;
  for(const char* p = dot + 1; *p != '\0' && i + 1 < n; p++) out[i++] = (char) tolower((unsigned char) *p);
  out[i] = '\0';
}

// "|zip|7z|..." membership, with the pipes doing the whole-word matching for us
static bool inList(const char* list, const char* ext) {
  if(list == NULL || list[0] == '\0' || ext[0] == '\0') return false;
  char needle[24];
  if(strlen(ext) + 3 > sizeof(needle)) return false;
  snprintf(needle, sizeof(needle), "|%s|", ext);
  return strstr(list, needle) != NULL;
}

bool isArchiveExt(const char* ext) {
  return inList("|zip|7z|rar|", ext);
}

// Disc images and raw dumps: real content, but they name no system on their own.
static bool isSharedExt(const char* ext) {
  return inList("|bin|iso|cue|chd|img|ccd|mdf|toc|rom|gdi|cdi|dsk|", ext);
}

const System* systemForExt(const char* ext) {
  const System* found = NULL;
  for(int i = 0; i < SYS_COUNT; i++) {
    if(inList(SYS[i].exts, ext)) {
      if(found != NULL && strcmp(found->core, SYS[i].core) != 0) return NULL; // two cores claim it
      if(found == NULL) found = &SYS[i];
    }
  }
  return found;
}

// Folder names as written on disk vary wildly ("Nintendo SNES", "Super_Nintendo",
// "snes"); strip everything that is not a letter or digit and they collapse together.
static void normalise(const char* s, char* out, size_t n) {
  size_t i = 0;
  for(; *s != '\0' && i + 1 < n; s++) {
    if(isalnum((unsigned char) *s)) out[i++] = (char) tolower((unsigned char) *s);
  }
  out[i] = '\0';
}

static const System* systemForFolder(const char* folder) {
  char norm[128], needle[132];
  normalise(folder, norm, sizeof(norm));
  if(norm[0] == '\0') return NULL;
  snprintf(needle, sizeof(needle), "|%s|", norm);
  for(int i = 0; i < SYS_COUNT; i++) {
    if(strstr(SYS[i].aliases, needle) != NULL) return &SYS[i];
  }
  return NULL;
}

const System* systemForPath(const char* path) {
  // Deepest folder first: "...\No-Intro\Super_Nintendo\S\game.7z" must find the SNES
  // before it walks up as far as the No-Intro set itself.
  const char* end = strrchr(path, '\\');
  while(end != NULL && end > path) {
    const char* start = end - 1;
    while(start > path && *(start - 1) != '\\') start--;
    char folder[128];
    size_t len = (size_t) (end - start);
    if(len >= sizeof(folder)) len = sizeof(folder) - 1;
    memcpy(folder, start, len);
    folder[len] = '\0';
    const System* s = systemForFolder(folder);
    if(s != NULL) return s;
    if(start == path) break;
    end = start - 1;
  }
  char ext[24];
  pathExt(path, ext, sizeof(ext));
  return systemForExt(ext);
}

bool isRomPath(const char* path) {
  char ext[24];
  pathExt(path, ext, sizeof(ext));
  if(ext[0] == '\0') return false;
  if(isArchiveExt(ext) || isSharedExt(ext)) return systemForPath(path) != NULL;
  return systemForExt(ext) != NULL;
}
