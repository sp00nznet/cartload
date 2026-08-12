// DeckEmu - touch + gamepad frontend for a shelf of libretro cores, aimed at Windows
// handhelds. Systems screen -> library (tap/drag to pick a rom) -> game -> in-game menu.
//
// One frontend, many machines: systems.c says which core runs what, core.c loads that
// core when a rom is picked, archive.c unwraps the .zip/.7z the rom usually arrives in.
// Descended from SnesDeck and N64Deck, which were this shape with one system each.

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <sys/stat.h>

#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL_syswm.h>

#include "archive.h"
#include "core.h"
#include "libretro.h"
#include "systems.h"

// A full shelf runs to six figures of files (54k MAME zips alone here), so the list is
// heap grown rather than a fixed array -- a small library still costs a small array.
#define MAX_ROMS 250000
#define PATHLEN 512
#define ROMPATH 400
#ifndef STATUS_HEAP_CORRUPTION
#define STATUS_HEAP_CORRUPTION 0xC0000374L
#endif
#define PADMAP_LEN 17
#define PAD_LT 15
#define PAD_RT 16

typedef struct {
  char path[ROMPATH];
  char name[160];      // display name, tags trimmed unless that would collide
  const System* sys;   // which machine, and so which core
  SDL_Texture* tex;    // lazily rendered name, invalidated on layout change
  int tw, th;
} Rom;

enum { MODE_SYSTEMS, MODE_LIBRARY, MODE_GAME, MODE_MENU };

static const char* MENU_ITEMS[] = {"Resume", "Save state", "Load state", "Reset", "Library", "Quit"};
#define MENU_COUNT 6

static struct {
  SDL_Window* window;
  SDL_Renderer* ren;
  SDL_Texture* frame; // the core's output, resized whenever the core changes resolution
  int frameW, frameH;

  bool loaded;
  char prefPath[PATHLEN];
  char romDir[PATHLEN];
  char statePath[PATHLEN];
  char toast[128];
  uint32_t toastUntil;

  Rom* roms; // grown by the scan worker; only it touches this while scanning
  int romCount, romCap;
  bool reachable; // last scan could open the rom folder at all

  // the library, filtered to one machine. view[] indexes roms[]; NULL filter is all of it.
  int* view;
  int viewCount, viewCap;
  const System* filter;
  struct {
    const System* sys; // NULL for the "All systems" row
    int count;
  } present[64];
  int presentCount;

  const System* openSys; // whose core is loaded right now, NULL for none
  char coreDir[PATHLEN], tmpDir[PATHLEN];
  // the list screens share one set of scroll physics; this is where the other one waits
  float sysScroll, libScroll;
  int sysSel, libSel;
  // scanning runs on a worker thread: an offline share blocks for ~20s and the ui
  // must stay alive. Only the worker touches g.roms while scanning is true.
  SDL_Thread* scanThread;
  SDL_atomic_t scanDone;
  SDL_atomic_t dirReady; // g.romDir is settled, safe for the main thread to display
  bool scanning;
  bool scanIsStartup;
  char scanArg[PATHLEN];
  int sel;
  int menuSel;
  int mode;
  bool running;
  bool fullscreen;

  // touch scrolling
  float scroll, vel;
  bool dragging;
  bool scrubbing; // dragging the right-edge strip rather than the list
  int dragDist;
  uint32_t pendingTap; // single tap in-game waits out the double-tap window, then opens the menu

  // layout, rebuilt on resize
  int W, H, headerH, rowH, footerH, listY, listH;
  TTF_Font *fTitle, *fRow, *fSmall;
} g;

static void startScan(bool isStartup, const char* arg);

static const SDL_Color COL_TEXT = {232, 236, 245, 255};
static const SDL_Color COL_DIM = {136, 145, 168, 255};
static const SDL_Color COL_ACCENT = {255, 255, 255, 255};

static void fillRect(int x, int y, int w, int h, int r, int gr, int b, int a) {
  SDL_Rect rc = {x, y, w, h};
  SDL_SetRenderDrawBlendMode(g.ren, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(g.ren, r, gr, b, a);
  SDL_RenderFillRect(g.ren, &rc);
}

static SDL_Texture* textTex(TTF_Font* f, const char* s, SDL_Color c, int* w, int* h) {
  SDL_Surface* sur = TTF_RenderUTF8_Blended(f, s, c);
  if(sur == NULL) return NULL;
  SDL_Texture* t = SDL_CreateTextureFromSurface(g.ren, sur);
  if(w) *w = sur->w;
  if(h) *h = sur->h;
  SDL_FreeSurface(sur);
  return t;
}

// anchor: 0 left, 1 center, 2 right
static void drawText(TTF_Font* f, const char* s, int x, int y, SDL_Color c, int anchor) {
  int w = 0, h = 0;
  SDL_Texture* t = textTex(f, s, c, &w, &h);
  if(t == NULL) return;
  if(anchor == 1) x -= w / 2;
  else if(anchor == 2) x -= w;
  SDL_Rect d = {x, y, w, h};
  SDL_RenderCopy(g.ren, t, NULL, &d);
  SDL_DestroyTexture(t);
}

// painted before anything that can block for a long time -- a network share that is
// down costs ~20s in FindFirstFile, and a black window reads as a crash
static void drawStatus(const char* line1, const char* line2) {
  fillRect(0, 0, g.W, g.H, 18, 20, 28, 255);
  fillRect(0, 0, g.W, g.headerH, 26, 30, 44, 255);
  drawText(g.fTitle, "DeckEmu", g.W / 24, (g.headerH - g.H / 13) / 2, COL_TEXT, 0);
  drawText(g.fRow, line1, g.W / 2, g.H / 2 - g.H / 16, COL_TEXT, 1);
  drawText(g.fSmall, line2, g.W / 2, g.H / 2 + g.H / 40, COL_DIM, 1);
}

static void toast(const char* msg) {
  snprintf(g.toast, sizeof(g.toast), "%s", msg);
  g.toastUntil = SDL_GetTicks() + 1800;
}

// --- files -----------------------------------------------------------------

static uint8_t* readFile(const char* name, int* length) {
  FILE* f = fopen(name, "rb");
  if(f == NULL) return NULL;
  fseek(f, 0, SEEK_END);
  int size = ftell(f);
  rewind(f);
  uint8_t* buffer = malloc(size);
  if(buffer == NULL || fread(buffer, size, 1, f) != 1) {
    free(buffer);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *length = size;
  return buffer;
}

// Machine first, then title: the library is browsed one system at a time, and this puts
// each system's roms in one contiguous run for the filter to slice out.
static int cmpRom(const void* a, const void* b) {
  const Rom* x = a;
  const Rom* y = b;
  int bySys = strcmp(x->sys->name, y->sys->name);
  return bySys != 0 ? bySys : _stricmp(x->name, y->name);
}

// filename without directory or extension -- what save states are keyed on
static void baseName(const char* path, char* out, size_t n) {
  const char* b = strrchr(path, '\\');
  snprintf(out, n, "%s", b ? b + 1 : path);
  char* dot = strrchr(out, '.');
  if(dot) *dot = '\0';
}

static void initRom(Rom* r, const char* path) {
  snprintf(r->path, sizeof(r->path), "%s", path);
  r->sys = systemForPath(path);
  baseName(path, r->name, sizeof(r->name));
  // ponytail: trim the "(USA) [!]" dump tags, they eat the row on a 7" screen.
  // display only -- saves stay keyed on the full filename so regions don't collide.
  char* tag = strstr(r->name, " (");
  if(tag == NULL) tag = strstr(r->name, " [");
  if(tag != NULL && tag != r->name) *tag = '\0';
  r->tex = NULL;
}

// worker thread: room for one more rom, doubling as it goes
static bool growRoms(void) {
  if(g.romCount < g.romCap) return true;
  int want = g.romCap > 0 ? g.romCap * 2 : 1024;
  if(want > MAX_ROMS) want = MAX_ROMS;
  if(want <= g.romCap) return false;
  Rom* bigger = realloc(g.roms, (size_t) want * sizeof(Rom));
  if(bigger == NULL) return false;
  g.roms = bigger;
  g.romCap = want;
  return true;
}

// returns false if the directory itself could not be opened (offline share, bad path)
static bool scanDir(const char* dir, int depth) {
  // 6 deep: sets nest as set\system\letter\game here, and a couple more never hurt
  if(depth > 6 || g.romCount >= MAX_ROMS) return true;
  char pattern[PATHLEN];
  snprintf(pattern, sizeof(pattern), "%s\\*", dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if(h == INVALID_HANDLE_VALUE) return false;
  do {
    if(fd.cFileName[0] == '.') continue;
    char full[PATHLEN];
    snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
    if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) scanDir(full, depth + 1);
    else if(isRomPath(full) && growRoms()) initRom(&g.roms[g.romCount++], full);
  } while(FindNextFileA(h, &fd) && g.romCount < MAX_ROMS);
  FindClose(h);
  return true;
}

// main thread only: textures belong to the renderer
static void clearRoms(void) {
  for(int i = 0; i < g.romCount; i++) {
    if(g.roms[i].tex) SDL_DestroyTexture(g.roms[i].tex);
    g.roms[i].tex = NULL;
  }
  g.romCount = 0;
  g.viewCount = 0;
  g.presentCount = 0;
  g.sel = 0;
  g.scroll = 0;
  g.vel = 0;
  g.sysSel = g.libSel = 0;
  g.sysScroll = g.libScroll = 0;
}

// main thread: which machines the scan actually turned up, "All" first
static void buildPresent(void) {
  g.presentCount = 0;
  g.present[g.presentCount].sys = NULL;
  g.present[g.presentCount++].count = g.romCount;
  for(int i = 0; i < g.romCount && g.presentCount < (int) (sizeof(g.present) / sizeof(g.present[0]));) {
    int j = i;
    while(j < g.romCount && g.roms[j].sys == g.roms[i].sys) j++; // sorted by system already
    g.present[g.presentCount].sys = g.roms[i].sys;
    g.present[g.presentCount++].count = j - i;
    i = j;
  }
}

// main thread: the rows the library screen shows, given the current filter
static void rebuildView(void) {
  if(g.viewCap < g.romCount) {
    int* bigger = realloc(g.view, (size_t) (g.romCount > 0 ? g.romCount : 1) * sizeof(int));
    if(bigger == NULL) return;
    g.view = bigger;
    g.viewCap = g.romCount;
  }
  g.viewCount = 0;
  for(int i = 0; i < g.romCount; i++) {
    if(g.filter == NULL || g.roms[i].sys == g.filter) g.view[g.viewCount++] = i;
  }
}

// the row list the current screen is showing
static int rowCount(void) {
  return g.mode == MODE_SYSTEMS ? g.presentCount : g.viewCount;
}

static bool isListMode(void) {
  return g.mode == MODE_SYSTEMS || g.mode == MODE_LIBRARY;
}

// worker thread: clearRoms() has already run, so no textures exist to free here
static void scanRoms(void) {
  SDL_AtomicSet(&g.dirReady, 1); // g.romDir is final for this scan, publish it
  g.romCount = 0;
  g.reachable = scanDir(g.romDir, 0);
  qsort(g.roms, g.romCount, sizeof(Rom), cmpRom);
  // trimming "(USA) [!]" off names makes every dump of a game look identical. Where that
  // happened, put the full filename back so the versions can be told apart.
  for(int i = 0; i < g.romCount;) {
    int j = i + 1;
    while(j < g.romCount && g.roms[j].sys == g.roms[i].sys && _stricmp(g.roms[i].name, g.roms[j].name) == 0) j++;
    if(j - i > 1) {
      for(int k = i; k < j; k++) baseName(g.roms[k].path, g.roms[k].name, sizeof(g.roms[k].name));
    }
    i = j;
  }
  qsort(g.roms, g.romCount, sizeof(Rom), cmpRom);
}

static void configPath(char* out, size_t n) {
  snprintf(out, n, "%sromdir.txt", g.prefPath);
}

// remembers the folder only; the caller decides how to kick off the scan
static void setRomDir(const char* dir) {
  snprintf(g.romDir, sizeof(g.romDir), "%s", dir);
  size_t l = strlen(g.romDir);
  while(l > 3 && (g.romDir[l - 1] == '\\' || g.romDir[l - 1] == '/')) g.romDir[--l] = '\0';
  char cfg[PATHLEN];
  configPath(cfg, sizeof(cfg));
  FILE* f = fopen(cfg, "wb");
  if(f) {
    fputs(g.romDir, f);
    fclose(f);
  }
}

// native folder picker: touch-friendly and already on every Windows box
static bool pickFolder(char* out, size_t n) {
  bool ok = false;
  HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  HWND parent = NULL;
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  if(SDL_GetWindowWMInfo(g.window, &info)) parent = info.info.win.window; // keep it modal, not lost behind fullscreen
  IFileOpenDialog* dlg = NULL;
  if(SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_ALL, &IID_IFileOpenDialog, (void**) &dlg))) {
    DWORD opts = 0;
    IFileOpenDialog_GetOptions(dlg, &opts);
    IFileOpenDialog_SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IFileOpenDialog_SetTitle(dlg, L"Pick your rom folder");
    if(SUCCEEDED(IFileOpenDialog_Show(dlg, parent))) { // fails plainly on cancel
      IShellItem* item = NULL;
      if(SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item))) {
        PWSTR wide = NULL;
        if(SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &wide))) {
          // rest of the app is on the -A apis, so convert to the same codepage
          ok = WideCharToMultiByte(CP_ACP, 0, wide, -1, out, (int) n, NULL, NULL) > 0;
          CoTaskMemFree(wide);
        }
        IShellItem_Release(item);
      }
    }
    IFileOpenDialog_Release(dlg);
  }
  if(SUCCEEDED(hrCo)) CoUninitialize(); // SDL may already have put us in an apartment
  return ok;
}

static void chooseFolder(void) {
  char dir[PATHLEN];
  if(g.scanning) return;
  if(!pickFolder(dir, sizeof(dir))) return; // cancelled
  setRomDir(dir);
  startScan(false, NULL);
}

static bool isDir(const char* path) {
  DWORD a = GetFileAttributesA(path);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void resolveRomDir(const char* target) {
  if(target != NULL && isDir(target)) {
    setRomDir(target);
    scanRoms();
    return;
  }
  char cfg[PATHLEN];
  configPath(cfg, sizeof(cfg));
  FILE* f = fopen(cfg, "rb");
  if(f) {
    char line[PATHLEN] = {0};
    size_t n = fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    while(n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
    char* start = line;
    // a hand-edited config saved as utf8 gets a BOM, which FindFirstFile rejects as a bad name
    if((unsigned char) start[0] == 0xEF && (unsigned char) start[1] == 0xBB && (unsigned char) start[2] == 0xBF) start += 3;
    while(*start == ' ' || *start == '\t') start++;
    if(start[0] != '\0') {
      // keep the chosen library even when it is unreachable right now -- a network
      // drive that is not up yet must not silently demote us to the local folder
      snprintf(g.romDir, sizeof(g.romDir), "%s", start);
      scanRoms();
      return;
    }
  }
  char* base = SDL_GetBasePath();
  snprintf(g.romDir, sizeof(g.romDir), "%sroms", base ? base : ".\\");
  SDL_free(base);
  scanRoms();
}

static int SDLCALL scanWorker(void* data) {
  (void) data;
  if(g.scanIsStartup) {
    resolveRomDir(g.scanArg[0] != '\0' ? g.scanArg : NULL);
  } else {
    scanRoms();
  }
  SDL_AtomicSet(&g.scanDone, 1);
  return 0;
}

// startup passes the command line target; later scans just re-read g.romDir
static void startScan(bool isStartup, const char* arg) {
  if(g.scanning) return;
  clearRoms();
  g.scanIsStartup = isStartup;
  snprintf(g.scanArg, sizeof(g.scanArg), "%s", arg != NULL ? arg : "");
  SDL_AtomicSet(&g.scanDone, 0);
  SDL_AtomicSet(&g.dirReady, isStartup ? 0 : 1); // a rescan already knows its folder
  g.scanning = true;
  g.scanThread = SDL_CreateThread(scanWorker, "romscan", NULL);
  if(g.scanThread == NULL) { // no thread? scan inline, a frozen ui beats no library
    scanWorker(NULL);
    g.scanning = false;
  }
}

// --- rom loading -----------------------------------------------------------

static void closeRom(void) {
  if(!g.loaded) return;
  coreUnloadGame(); // the core flushes the cartridge save itself
  g.loaded = false;
}

// the core dlls ship separately: in cores\ beside the exe, or loose next to it
static bool findCoreDll(const char* name, char* out, size_t n) {
  char* base = SDL_GetBasePath();
  const char* where[] = {"%scores\\%s", "%s%s"};
  for(int i = 0; i < 2; i++) {
    snprintf(out, n, where[i], base ? base : ".\\", name);
    if(GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) {
      SDL_free(base);
      return true;
    }
  }
  SDL_free(base);
  return false;
}

// Load the core this system needs, unless it is already the one we have. Cores are not
// cheap to swap -- each brings its own GL context up and down -- so staying put while
// the player picks another game for the same machine is worth the bookkeeping.
static bool ensureCore(const System* sys, char* err, size_t errLen) {
  if(g.openSys == sys && coreIsOpen()) return true;
  char dll[PATHLEN];
  if(!findCoreDll(sys->core, dll, sizeof(dll))) {
    snprintf(err, errLen, "%s needs %s in cores\\", sys->name, sys->core);
    g.openSys = NULL;
    return false;
  }
  char saveDir[PATHLEN], systemDir[PATHLEN];
  snprintf(systemDir, sizeof(systemDir), "%ssystem", g.prefPath); // bios, shared by every core
  snprintf(saveDir, sizeof(saveDir), "%ssaves\\%s", g.prefPath, sys->name);
  mkdir(saveDir);
  g.openSys = NULL;
  if(!coreOpen(dll, g.window, systemDir, saveDir, sys->options, sys->optionCount, err, errLen)) return false;
  g.openSys = sys;
  return true;
}

static bool loadRom(const Rom* rom) {
  char err[256] = {0};
  closeRom();
  const System* sys = rom != NULL ? rom->sys : NULL;
  if(sys == NULL) {
    toast("Don't know what machine that one is for");
    return false;
  }
  if(!ensureCore(sys, err, sizeof(err))) {
    toast(err);
    return false;
  }

  // Most of the shelf is packed. The core takes the rom in memory, unless it is one of
  // the disc or computer cores that insists on reading the file itself -- those get a
  // real path, so the archive is unpacked into tmp\ and left there for next time.
  char ext[24], content[PATHLEN];
  uint8_t* data = NULL;
  size_t size = 0;
  snprintf(content, sizeof(content), "%s", rom->path);
  pathExt(rom->path, ext, sizeof(ext));
  if(isArchiveExt(ext)) {
    char entry[512];
    if(!archiveBestEntry(rom->path, entry, sizeof(entry))) {
      toast(archiveReady() ? "Nothing runnable inside that archive" : "7-Zip is missing, can't open archives");
      return false;
    }
    if(coreNeedFullpath()) {
      if(!archiveExtractToFile(rom->path, entry, g.tmpDir, content, sizeof(content), err, sizeof(err))) {
        toast(err);
        return false;
      }
    } else {
      data = archiveExtract(rom->path, entry, &size, err, sizeof(err));
      if(data == NULL) {
        toast(err);
        return false;
      }
      // The core still gets a path, and plenty of them read the extension off it to
      // work out what they were handed -- stella flatly refuses a ".7z". RetroArch's
      // convention for content it unpacked is archive#entry, which leaves the real
      // extension last, so cores that only look at the name are happy.
      const char* leaf = strrchr(entry, '/');
      snprintf(content, sizeof(content), "%s#%s", rom->path, leaf ? leaf + 1 : entry);
    }
  }

  bool ok = coreLoadGame(content, data, size, err, sizeof(err));
  free(data);
  if(!ok) {
    toast(err[0] ? err : "Could not load that rom");
    return false;
  }

  char saveName[192], stateDir[PATHLEN];
  baseName(rom->path, saveName, sizeof(saveName));
  snprintf(stateDir, sizeof(stateDir), "%sstates\\%s", g.prefPath, sys->name);
  mkdir(stateDir);
  snprintf(g.statePath, sizeof(g.statePath), "%s\\%s.state", stateDir, saveName);
  g.loaded = true;
  char title[256];
  snprintf(title, sizeof(title), "DeckEmu - %s", rom->name);
  SDL_SetWindowTitle(g.window, title);
  g.mode = MODE_GAME;
  return true;
}

static void saveState(void) {
  int size = coreStateSize();
  uint8_t* data = size > 0 ? malloc(size) : NULL;
  if(data == NULL || !coreSaveState(data, size)) {
    free(data);
    toast("Could not make a state");
    return;
  }
  FILE* f = fopen(g.statePath, "wb");
  if(f) {
    fwrite(data, size, 1, f);
    fclose(f);
    toast("State saved");
  } else {
    toast("Could not save state");
  }
  free(data);
}

static void loadState(void) {
  int size = 0;
  uint8_t* data = readFile(g.statePath, &size);
  if(data == NULL) {
    toast("No state saved yet");
    return;
  }
  toast(coreLoadState(data, size) ? "State loaded" : "State file invalid");
  free(data);
}

// --- layout ----------------------------------------------------------------

static void layout(void) {
  SDL_GetRendererOutputSize(g.ren, &g.W, &g.H);
  g.rowH = g.H / 9;
  if(g.rowH < 64) g.rowH = 64; // touch target floor
  g.headerH = g.H / 8;
  g.footerH = g.H / 13;
  g.listY = g.headerH;
  g.listH = g.H - g.headerH - g.footerH;
  if(g.fTitle) TTF_CloseFont(g.fTitle);
  if(g.fRow) TTF_CloseFont(g.fRow);
  if(g.fSmall) TTF_CloseFont(g.fSmall);
  const char* fonts[] = {"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf", "font.ttf"};
  g.fTitle = g.fRow = g.fSmall = NULL;
  for(int i = 0; i < 3 && g.fRow == NULL; i++) {
    g.fTitle = TTF_OpenFont(fonts[i], g.H / 17);
    g.fRow = TTF_OpenFont(fonts[i], g.H / 23);
    g.fSmall = TTF_OpenFont(fonts[i], g.H / 36);
  }
  for(int i = 0; i < g.romCount; i++) {
    if(g.roms[i].tex) {
      SDL_DestroyTexture(g.roms[i].tex);
      g.roms[i].tex = NULL;
    }
  }
}

// the rom behind row i of the library, or NULL if that row is not one
static Rom* rowRom(int i) {
  if(g.mode == MODE_SYSTEMS || i < 0 || i >= g.viewCount) return NULL;
  return &g.roms[g.view[i]];
}

static float maxScroll(void) {
  float m = (float) (rowCount() * g.rowH - g.listH);
  return m > 0 ? m : 0;
}

static void scrollToSel(void) {
  float top = (float) (g.sel * g.rowH);
  float bottom = top + g.rowH - g.listH;
  if(g.scroll > top) g.scroll = top;
  if(g.scroll < bottom) g.scroll = bottom;
  if(g.scroll > maxScroll()) g.scroll = maxScroll();
  if(g.scroll < 0) g.scroll = 0;
  g.vel = 0;
}

static void moveSel(int delta) {
  if(rowCount() == 0) return;
  g.sel += delta;
  if(g.sel < 0) g.sel = 0;
  if(g.sel >= rowCount()) g.sel = rowCount() - 1;
  scrollToSel();
}

// Each list screen keeps its own place, so backing out of a system and into another
// does not dump you at the top of a library you were halfway down.
static void switchMode(int mode) {
  if(g.mode == MODE_SYSTEMS) {
    g.sysSel = g.sel;
    g.sysScroll = g.scroll;
  } else if(g.mode == MODE_LIBRARY) {
    g.libSel = g.sel;
    g.libScroll = g.scroll;
  }
  g.mode = mode;
  g.vel = 0;
  if(mode == MODE_SYSTEMS) {
    g.sel = g.sysSel;
    g.scroll = g.sysScroll;
  } else if(mode == MODE_LIBRARY) {
    g.sel = g.libSel;
    g.scroll = g.libScroll;
  }
  if(g.sel >= rowCount()) g.sel = rowCount() > 0 ? rowCount() - 1 : 0;
  if(g.scroll > maxScroll()) g.scroll = maxScroll();
}

// picking a machine on the systems screen
static void chooseSystem(int i) {
  if(i < 0 || i >= g.presentCount) return;
  g.filter = g.present[i].sys;
  rebuildView();
  g.libSel = 0;
  g.libScroll = 0;
  switchMode(MODE_LIBRARY);
}

// --- drawing ---------------------------------------------------------------

static bool hit(SDL_Rect r, int x, int y) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static SDL_Rect folderButton(void) {
  int w = g.W / 6, h = g.headerH * 3 / 5;
  SDL_Rect r = {g.W - w - g.W / 40, (g.headerH - h) / 2, w, h};
  return r;
}

// touch strip down the right edge: drag it to cross the whole library at once.
// Dragging rows only moves a screenful at a time, which is useless at 4000 roms.
static SDL_Rect scrollStrip(void) {
  int w = g.W / 22;
  if(w < 48) w = 48; // stay thumb-sized
  SDL_Rect r = {g.W - w, g.listY, w, g.listH};
  return r;
}

static void scrubTo(int y) {
  float t = (float) (y - g.listY) / (float) g.listH;
  if(t < 0.0f) t = 0.0f;
  if(t > 1.0f) t = 1.0f;
  g.scroll = t * maxScroll();
  g.vel = 0;
  g.sel = (int) (g.scroll / g.rowH); // keep the controller cursor where the eye is
  if(g.sel >= rowCount()) g.sel = rowCount() - 1;
  if(g.sel < 0) g.sel = 0;
}

static int rowLetter(int i) {
  Rom* r = rowRom(i);
  const char* s = r != NULL ? r->name : (g.present[i].sys ? g.present[i].sys->name : "All");
  return toupper((unsigned char) s[0]);
}

// shoulder buttons skip a letter at a time, the gamepad equivalent of the strip
static void jumpLetter(int dir) {
  if(rowCount() == 0) return;
  int i = g.sel;
  int cur = rowLetter(i);
  while(i + dir >= 0 && i + dir < rowCount()) {
    i += dir;
    if(rowLetter(i) != cur) break;
  }
  if(dir < 0) { // land on the first entry of that letter, not the last
    int c = rowLetter(i);
    while(i > 0 && rowLetter(i - 1) == c) i--;
  }
  g.sel = i;
  scrollToSel();
}

// Both list screens are the same list: the machines the scan found, or the games on one
// of them. Only where the row text comes from differs.
static void drawList(void) {
  bool sysScreen = (g.mode == MODE_SYSTEMS);
  int rows = rowCount();
  fillRect(0, 0, g.W, g.H, 18, 20, 28, 255);
  fillRect(0, 0, g.W, g.headerH, 26, 30, 44, 255);
  drawText(g.fTitle, sysScreen ? "DeckEmu" : (g.filter ? g.filter->name : "All systems"), g.W / 24,
           (g.headerH - g.H / 13) / 2, COL_TEXT, 0);
  SDL_Rect fb = folderButton();
  fillRect(fb.x, fb.y, fb.w, fb.h, 44, 50, 70, 255);
  drawText(g.fSmall, sysScreen ? "Rom folder" : "Systems", fb.x + fb.w / 2, fb.y + (fb.h - g.H / 30) / 2, COL_TEXT, 1);
  char sub[64];
  if(sysScreen) snprintf(sub, sizeof(sub), "%d system%s", rows - 1, rows == 2 ? "" : "s");
  else snprintf(sub, sizeof(sub), "%d game%s", rows, rows == 1 ? "" : "s");
  drawText(g.fSmall, sub, fb.x - g.W / 60, (g.headerH - g.H / 30) / 2, COL_DIM, 2);

  SDL_Rect clip = {0, g.listY, g.W, g.listH};
  SDL_RenderSetClipRect(g.ren, &clip);
  if(rows == 0) {
    drawText(g.fRow, g.reachable ? "No roms found" : "Can't reach that folder", g.W / 2,
             g.listY + g.listH / 2 - g.H / 12, COL_TEXT, 1);
    drawText(g.fSmall, g.romDir, g.W / 2, g.listY + g.listH / 2, COL_DIM, 1);
    drawText(g.fSmall,
             g.reachable ? "Tap \"Rom folder\" above to pick your library"
                         : "If it's a network drive, check it's connected, then tap here to retry",
             g.W / 2, g.listY + g.listH / 2 + g.H / 18, COL_DIM, 1);
  }
  int first = (int) (g.scroll / g.rowH);
  if(first < 0) first = 0;
  for(int i = first; i < rows; i++) {
    int y = g.listY + i * g.rowH - (int) g.scroll;
    if(y > g.listY + g.listH) break;
    bool selected = (i == g.sel);
    if(selected) {
      fillRect(0, y, g.W, g.rowH - 2, 60, 110, 200, 255);
      fillRect(0, y, g.W / 120, g.rowH - 2, 120, 190, 255, 255);
    } else if(i % 2 == 0) {
      fillRect(0, y, g.W, g.rowH - 2, 26, 30, 44, 130);
    }
    // clamp into the list: an unclamped row clip let names scroll over the header
    int top = y < g.listY ? g.listY : y;
    int bot = y + g.rowH > g.listY + g.listH ? g.listY + g.listH : y + g.rowH;
    SDL_Rect rowClip = {0, top, g.W - g.W / 20, bot - top};
    SDL_RenderSetClipRect(g.ren, &rowClip);
    Rom* r = rowRom(i);
    if(r != NULL) { // games are cached: there can be a hundred thousand of them
      if(r->tex == NULL) r->tex = textTex(g.fRow, r->name, COL_TEXT, &r->tw, &r->th);
      if(r->tex != NULL) {
        SDL_SetTextureColorMod(r->tex, selected ? 255 : 232, selected ? 255 : 236, selected ? 255 : 245);
        SDL_Rect d = {g.W / 20, y + (g.rowH - r->th) / 2, r->tw, r->th};
        SDL_RenderCopy(g.ren, r->tex, NULL, &d);
      }
    } else { // a couple of dozen machines, drawn straight
      const System* s = g.present[i].sys;
      char count[32];
      snprintf(count, sizeof(count), "%d", g.present[i].count);
      drawText(g.fRow, s ? s->name : "All systems", g.W / 20, y + (g.rowH - g.H / 20) / 2,
               selected ? COL_ACCENT : COL_TEXT, 0);
      drawText(g.fSmall, count, g.W - g.W / 12, y + (g.rowH - g.H / 30) / 2, COL_DIM, 2);
    }
    SDL_RenderSetClipRect(g.ren, &clip);
  }
  SDL_RenderSetClipRect(g.ren, NULL);
  // scroll strip: visible so it reads as grabbable, thumb shows where you are
  if(maxScroll() > 0) {
    SDL_Rect s = scrollStrip();
    fillRect(s.x, s.y, s.w, s.h, 26, 30, 44, g.scrubbing ? 220 : 120);
    int barH = (int) ((float) g.listH / (rows * g.rowH) * g.listH);
    if(barH < 40) barH = 40;
    int barY = s.y + (int) (g.scroll / maxScroll() * (g.listH - barH));
    int barW = s.w / 3;
    fillRect(s.x + (s.w - barW) / 2, barY, barW, barH, 120, 190, 255, g.scrubbing ? 255 : 180);
  }
  // big letter while scrubbing, so flying through 50000 roms is aimable
  if(g.scrubbing && rows > 0) {
    int idx = (int) (g.scroll / g.rowH);
    if(idx >= rows) idx = rows - 1;
    if(idx < 0) idx = 0;
    char letter[2] = {(char) rowLetter(idx), '\0'};
    int box = g.H / 4;
    fillRect((g.W - box) / 2, (g.H - box) / 2, box, box, 40, 60, 100, 235);
    drawText(g.fTitle, letter, g.W / 2, (g.H - g.H / 13) / 2, COL_TEXT, 1);
  }
  // a library this size would otherwise keep a texture per row it has ever drawn
  if(!sysScreen && rows > 500) {
    int visible = g.listH / g.rowH + 2;
    int lo = first - 60, hi = first + visible + 60;
    for(int i = 0; i < rows; i++) {
      Rom* r = &g.roms[g.view[i]];
      if((i < lo || i > hi) && r->tex != NULL) {
        SDL_DestroyTexture(r->tex);
        r->tex = NULL;
      }
    }
  }

  fillRect(0, g.H - g.footerH, g.W, g.footerH, 26, 30, 44, 255);
  drawText(g.fSmall,
           sysScreen ? "Tap or A: open      Y: rom folder      B: quit      drag to scroll"
                     : "Tap or A: play      B: systems      LB/RB: jump a letter      drag to scroll",
           g.W / 2, g.H - g.footerH + (g.footerH - g.H / 30) / 2, COL_DIM, 1);
}

// the core picks its own output size, and changes it mid-game on some titles
static void updateFrame(void) {
  int w = 0, h = 0;
  const void* px = corePixels(&w, &h);
  if(px == NULL) return;
  if(g.frame == NULL || w != g.frameW || h != g.frameH) {
    if(g.frame) SDL_DestroyTexture(g.frame);
    g.frame = SDL_CreateTexture(g.ren, corePixelFormat(), SDL_TEXTUREACCESS_STREAMING, w, h);
    g.frameW = w;
    g.frameH = h;
    if(g.frame == NULL) return;
  }
  SDL_UpdateTexture(g.frame, NULL, px, w * (SDL_BYTESPERPIXEL(corePixelFormat())));
}

static void drawGame(void) {
  SDL_SetRenderDrawColor(g.ren, 0, 0, 0, 255);
  SDL_RenderClear(g.ren);
  if(g.frame == NULL) return;
  // the aspect the core asks for, not the one it renders at: a 640x480 N64 frame is
  // still 4:3, and a Game Boy's 160x144 is not square either
  float aspect = g.openSys != NULL && g.openSys->aspect > 0.1f ? g.openSys->aspect : coreAspect();
  int w = g.W, h = (int) (g.W / aspect);
  if(h > g.H) {
    h = g.H;
    w = (int) (g.H * aspect);
  }
  SDL_Rect dst = {(g.W - w) / 2, (g.H - h) / 2, w, h};
  // GL hands frames back bottom-up; flipping in the blit beats flipping 1.2MB on the cpu
  SDL_RenderCopyEx(g.ren, g.frame, NULL, &dst, 0.0, NULL,
                   coreFlipped() ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
}

static SDL_Rect menuButtonRect(int i) {
  int bw = g.W / 3, bh = g.rowH;
  if(bw < g.W / 2 - g.W / 8) bw = g.W / 2 - g.W / 8;
  int totalH = MENU_COUNT * (bh + g.H / 60);
  SDL_Rect r = {(g.W - bw) / 2, (g.H - totalH) / 2 + i * (bh + g.H / 60), bw, bh};
  return r;
}

static SDL_Rect gameMenuButton(void) {
  int w = g.W / 9, h = g.H / 16;
  SDL_Rect r = {g.W - w - g.W / 60, g.H / 60, w, h};
  return r;
}

static void drawMenu(void) {
  drawGame();
  fillRect(0, 0, g.W, g.H, 10, 12, 18, 215);
  for(int i = 0; i < MENU_COUNT; i++) {
    SDL_Rect r = menuButtonRect(i);
    bool sel = (i == g.menuSel);
    fillRect(r.x, r.y, r.w, r.h, sel ? 60 : 32, sel ? 110 : 36, sel ? 200 : 52, 255);
    drawText(g.fRow, MENU_ITEMS[i], r.x + r.w / 2, r.y + (r.h - g.H / 20) / 2, COL_TEXT, 1);
  }
}

// --- input -----------------------------------------------------------------

// physical pad -> libretro id, through whichever map the running system asked for
static int padToCore(int button) {
  if(button < 0 || button >= PADMAP_LEN) return -1;
  return systemPadMap(g.openSys)[button];
}

static int keyToCore(int key) {
  switch(key) {
    case SDLK_z: return RETRO_DEVICE_ID_JOYPAD_B;
    case SDLK_x: return RETRO_DEVICE_ID_JOYPAD_A;
    case SDLK_a: return RETRO_DEVICE_ID_JOYPAD_Y;
    case SDLK_s: return RETRO_DEVICE_ID_JOYPAD_X;
    case SDLK_q: return RETRO_DEVICE_ID_JOYPAD_L;
    case SDLK_w: return RETRO_DEVICE_ID_JOYPAD_R;
    case SDLK_e: return RETRO_DEVICE_ID_JOYPAD_L2;
    case SDLK_r: return RETRO_DEVICE_ID_JOYPAD_R2;
    case SDLK_RSHIFT: return RETRO_DEVICE_ID_JOYPAD_SELECT;
    case SDLK_RETURN: return RETRO_DEVICE_ID_JOYPAD_START;
    case SDLK_UP: return RETRO_DEVICE_ID_JOYPAD_UP;
    case SDLK_DOWN: return RETRO_DEVICE_ID_JOYPAD_DOWN;
    case SDLK_LEFT: return RETRO_DEVICE_ID_JOYPAD_LEFT;
    case SDLK_RIGHT: return RETRO_DEVICE_ID_JOYPAD_RIGHT;
  }
  return -1;
}

static bool isDirId(int id) {
  return id == RETRO_DEVICE_ID_JOYPAD_UP || id == RETRO_DEVICE_ID_JOYPAD_DOWN ||
         id == RETRO_DEVICE_ID_JOYPAD_LEFT || id == RETRO_DEVICE_ID_JOYPAD_RIGHT;
}

static void toggleFullscreen(void) {
  g.fullscreen = !g.fullscreen;
  SDL_SetWindowFullscreen(g.window, g.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void releaseButtons(void) { // don't hold anything down while paused
  memset(corePad, 0, sizeof(corePad));
  coreStick[0] = coreStick[1] = coreCStick[0] = coreCStick[1] = 0;
}

static void openMenu(void) {
  g.mode = MODE_MENU;
  g.menuSel = 0;
  releaseButtons();
}

static void backToLibrary(void) {
  closeRom();
  switchMode(MODE_LIBRARY);
  SDL_SetWindowTitle(g.window, "DeckEmu");
}

static void activateMenu(int item) {
  switch(item) {
    case 0: g.mode = MODE_GAME; break;
    case 1: saveState(); g.mode = MODE_GAME; break;
    case 2: loadState(); g.mode = MODE_GAME; break;
    case 3: coreReset(); g.mode = MODE_GAME; break;
    case 4: backToLibrary(); break;
    case 5: closeRom(); g.running = false; break; // closeRom first so the save is written
  }
}

// one direction handler for dpad, stick and keyboard, in whichever mode
static void onDirection(int id, bool pressed) {
  if(g.mode == MODE_GAME) {
    corePad[id] = pressed;
    return;
  }
  if(!pressed || g.scanning) return;
  if(g.mode == MODE_LIBRARY || g.mode == MODE_SYSTEMS) {
    if(id == RETRO_DEVICE_ID_JOYPAD_UP) moveSel(-1);
    else if(id == RETRO_DEVICE_ID_JOYPAD_DOWN) moveSel(1);
    else if(id == RETRO_DEVICE_ID_JOYPAD_LEFT) moveSel(-(g.listH / g.rowH));
    else if(id == RETRO_DEVICE_ID_JOYPAD_RIGHT) moveSel(g.listH / g.rowH);
  } else {
    if(id == RETRO_DEVICE_ID_JOYPAD_UP) g.menuSel = (g.menuSel + MENU_COUNT - 1) % MENU_COUNT;
    else if(id == RETRO_DEVICE_ID_JOYPAD_DOWN) g.menuSel = (g.menuSel + 1) % MENU_COUNT;
  }
}

static void onConfirm(void) {
  if(g.scanning) return;
  if(g.mode == MODE_SYSTEMS) {
    chooseSystem(g.sel);
  } else if(g.mode == MODE_LIBRARY) {
    Rom* r = rowRom(g.sel);
    if(r != NULL) loadRom(r);
  } else if(g.mode == MODE_MENU) {
    activateMenu(g.menuSel);
  }
}

static void handleTouch(int x, int y, int clicks) {
  if(g.scanning) return; // the worker owns the list right now
  if(g.mode == MODE_SYSTEMS || g.mode == MODE_LIBRARY) {
    // the header button is the folder picker on the systems screen and the way back
    // to it from a library
    if(hit(folderButton(), x, y)) {
      if(g.mode == MODE_SYSTEMS) chooseFolder();
      else switchMode(MODE_SYSTEMS);
      return;
    }
    if(y < g.listY || y > g.listY + g.listH) return;
    if(g.romCount == 0 && !g.reachable) { // offline share: tapping the message retries
      startScan(false, NULL);
      return;
    }
    int idx = (int) ((y - g.listY + g.scroll) / g.rowH);
    if(idx < 0 || idx >= rowCount()) return;
    g.sel = idx;
    if(g.mode == MODE_SYSTEMS) chooseSystem(idx);
    else loadRom(rowRom(idx));
  } else if(g.mode == MODE_GAME) {
    if(clicks >= 2) { // double tap toggles fullscreen, either way
      g.pendingTap = 0;
      toggleFullscreen();
    } else if(hit(gameMenuButton(), x, y)) {
      openMenu(); // tapping the visible button shouldn't wait
    } else {
      // a tap anywhere else is only a menu tap once it can't still become a double tap
      g.pendingTap = SDL_GetTicks() + GetDoubleClickTime() + 20;
    }
  } else {
    for(int i = 0; i < MENU_COUNT; i++) {
      if(hit(menuButtonRect(i), x, y)) {
        g.menuSel = i;
        activateMenu(i);
        return;
      }
    }
    g.mode = MODE_GAME; // tap outside the buttons closes the menu
  }
}

// --- main ------------------------------------------------------------------

static FILE* selftestLog;

static void SDLCALL selftestLogCb(void* userdata, int category, SDL_LogPriority priority, const char* message) {
  (void) userdata;
  (void) category;
  (void) priority;
  if(selftestLog) fprintf(selftestLog, "%s\n", message);
}

// selftest only: heap corruption is reported by a fail fast, which skips the unhandled
// filter entirely. A first chance vectored handler still sees it, and the backtrace says
// whose code was running.
static LONG CALLBACK selftestVectoredCb(EXCEPTION_POINTERS* ep) {
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  if(selftestLog == NULL || code == 0x406D1388 /* thread name */ || (code & 0x20000000)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if(code != STATUS_HEAP_CORRUPTION && code != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  fprintf(selftestLog, "EXCEPTION 0x%08lx at %p\n", code, ep->ExceptionRecord->ExceptionAddress);
  void* frames[24];
  USHORT n = RtlCaptureStackBackTrace(0, 24, frames, NULL);
  for(USHORT i = 0; i < n; i++) {
    char module[MAX_PATH] = "?";
    MEMORY_BASIC_INFORMATION mbi;
    if(VirtualQuery(frames[i], &mbi, sizeof(mbi)) && mbi.AllocationBase) {
      GetModuleFileNameA((HMODULE) mbi.AllocationBase, module, sizeof(module));
      const char* slash = strrchr(module, '\\');
      if(slash) memmove(module, slash + 1, strlen(slash));
    }
    fprintf(selftestLog, "  #%02d %p %s\n", i, frames[i], module);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// selftest only: name the module that died, since there is no debugger on this box
static LONG WINAPI selftestCrashCb(EXCEPTION_POINTERS* ep) {
  void* at = ep->ExceptionRecord->ExceptionAddress;
  char module[MAX_PATH] = "?";
  MEMORY_BASIC_INFORMATION mbi;
  if(VirtualQuery(at, &mbi, sizeof(mbi)) && mbi.AllocationBase) {
    GetModuleFileNameA((HMODULE) mbi.AllocationBase, module, sizeof(module));
  }
  if(selftestLog) {
    fprintf(selftestLog, "CRASH code 0x%08lx at %p in %s (+0x%llx)\n", ep->ExceptionRecord->ExceptionCode, at, module,
            (unsigned long long) ((uint8_t*) at - (uint8_t*) mbi.AllocationBase));
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

static void fatal(const char* what) {
  char msg[512];
  snprintf(msg, sizeof(msg), "%s: %s", what, SDL_GetError());
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "DeckEmu", msg, NULL);
  exit(1);
}

int main(int argc, char** argv) {
  bool windowed = false;
  // --selftest: report which cores are actually installed, and with a rom given, boot
  // it far enough to prove a frame came back. The only part of this that can be checked
  // without sitting down with a controller.
  bool selftest = false;
  int frames = 120; // --selftest <rom> [frames]: boot far enough in to see something
  const char* target = NULL; // a rom to boot straight into, or a library folder
  for(int i = 1; i < argc; i++) {
    if(strcmp(argv[i], "--windowed") == 0) windowed = true;
    else if(strcmp(argv[i], "--selftest") == 0) windowed = selftest = true;
    else if(target == NULL) target = argv[i];
    else if(selftest && atoi(argv[i]) > 0) frames = atoi(argv[i]);
  }
  if(selftest) { // set up before anything else, so core startup lands in the log too
    selftestLog = fopen("selftest.log", "w");
    setvbuf(selftestLog, NULL, _IONBF, 0); // a crash mid-test must not swallow the log
    SDL_LogSetOutputFunction(selftestLogCb, NULL);
    SetUnhandledExceptionFilter(selftestCrashCb);
    AddVectoredExceptionHandler(1, selftestVectoredCb);
    coreVerbose = true;
  }
  // handheld panels run at 150-200% scaling; without this Windows upscales a small
  // backbuffer and everything goes soft. Must be set before SDL_Init.
  SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
  SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl"); // the core needs a GL window to share
  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) fatal("SDL init failed");
  if(TTF_Init() != 0) fatal("Font init failed");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1"); // non-integer scaling on a 7" panel

  g.fullscreen = !windowed;
  g.window = SDL_CreateWindow("DeckEmu", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1280, 720,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_OPENGL |
                              (g.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
  if(g.window == NULL) fatal("Could not create window");
  g.ren = SDL_CreateRenderer(g.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if(g.ren == NULL) fatal("Could not create renderer");
  SDL_ShowCursor(SDL_DISABLE);

  char* pref = SDL_GetPrefPath("", "DeckEmu");
  snprintf(g.prefPath, sizeof(g.prefPath), "%s", pref ? pref : ".\\");
  SDL_free(pref);
  char saveDir[PATHLEN], systemDir[PATHLEN], stateDir[PATHLEN];
  snprintf(saveDir, sizeof(saveDir), "%ssaves", g.prefPath);
  snprintf(systemDir, sizeof(systemDir), "%ssystem", g.prefPath);
  snprintf(stateDir, sizeof(stateDir), "%sstates", g.prefPath);
  snprintf(g.tmpDir, sizeof(g.tmpDir), "%stmp", g.prefPath);
  mkdir(saveDir);
  mkdir(systemDir);
  mkdir(stateDir);
  mkdir(g.tmpDir);

  if(selftest) {
    // exit 0 pass, 2 no core for that rom, 3 rom would not load, 4 the core drew nothing.
    // With no rom it just inventories the shelf, which is the useful thing after a
    // core download: it says which systems can actually be played right now.
    int count = 0, have = 0;
    const System* all = systems(&count);
    for(int i = 0; i < count; i++) {
      char dll[PATHLEN];
      bool ok = findCoreDll(all[i].core, dll, sizeof(dll));
      have += ok;
      fprintf(selftestLog, "%-20s %-40s %s\n", all[i].name, all[i].core, ok ? "ok" : "MISSING");
    }
    fprintf(selftestLog, "%d/%d cores installed, 7-Zip %s\n", have, count, archiveReady() ? "ok" : "MISSING");
    int rc = 0;
    if(target != NULL && isRomPath(target)) {
      // given a rom, go the whole way: gl context, framebuffer, readback, a lit frame
      Rom direct;
      memset(&direct, 0, sizeof(direct));
      initRom(&direct, target);
      fprintf(selftestLog, "rom: %s -> %s\n", target, direct.sys ? direct.sys->name : "(unknown system)");
      if(!loadRom(&direct)) {
        fprintf(selftestLog, "load failed: %s\n", g.toast);
        rc = 3;
      } else {
        fprintf(selftestLog, "core: %s\n", coreName() ? coreName() : "(none)");
        for(int i = 0; i < frames; i++) {
          if(i % 10 == 0) fprintf(selftestLog, "frame %d\n", i);
          coreRunFrame();
        }
        int w = 0, h = 0;
        const uint8_t* px = corePixels(&w, &h);
        int bpp = SDL_BYTESPERPIXEL(corePixelFormat());
        if(px != NULL && w > 0) { // selftest.bmp, so a black screen can actually be looked at
          size_t row = (size_t) w * bpp;
          uint8_t* upright = malloc(row * h);
          for(int y = 0; y < h; y++) {
            memcpy(upright + (size_t) y * row, px + (size_t) (coreFlipped() ? h - 1 - y : y) * row, row);
          }
          SDL_Surface* s =
              SDL_CreateRGBSurfaceWithFormatFrom(upright, w, h, bpp * 8, (int) row, corePixelFormat());
          SDL_SaveBMP(s, "selftest.bmp");
          SDL_FreeSurface(s);
          free(upright);
        }
        fprintf(selftestLog, "frame %dx%d flipped=%d\n", w, h, (int) coreFlipped());
        if(px != NULL && w > 0) {
          const uint8_t* mid = px + ((size_t) (h / 2) * w + w / 2) * 4;
          fprintf(selftestLog, "corner %02x%02x%02x%02x  centre %02x%02x%02x%02x\n", px[0], px[1], px[2], px[3],
                  mid[0], mid[1], mid[2], mid[3]);
        }
        // a frame with any colour in it. Alpha is skipped: it comes back opaque whether
        // or not the core drew, so counting it would pass a blank screen
        rc = 4;
        for(int i = 0; px != NULL && i < w * h * bpp && rc != 0; i++) {
          if(!(bpp == 4 && i % 4 == 3) && px[i] != 0) rc = 0;
        }
      }
    }
    fprintf(selftestLog, "exit %d\n", rc);
    fclose(selftestLog);
    closeRom();
    coreClose();
    return rc;
  }

  g.mode = MODE_SYSTEMS;
  g.running = true;
  layout();
  startScan(true, target);
  if(target != NULL && isRomPath(target)) {
    Rom direct;
    memset(&direct, 0, sizeof(direct));
    initRom(&direct, target);
    loadRom(&direct);
  }

  uint64_t countFreq = SDL_GetPerformanceFrequency();
  uint64_t lastCount = SDL_GetPerformanceCounter();
  float timeAdder = 0.0f;
  SDL_Event event;

  while(g.running) {
    while(SDL_PollEvent(&event)) {
      switch(event.type) {
        case SDL_QUIT: g.running = false; break;
        case SDL_WINDOWEVENT:
          if(event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) layout();
          break;
        case SDL_CONTROLLERDEVICEADDED:
          SDL_GameControllerOpen(event.cdevice.which);
          break;
        case SDL_CONTROLLERDEVICEREMOVED: {
          SDL_GameController* c = SDL_GameControllerFromInstanceID(event.cdevice.which);
          if(c) SDL_GameControllerClose(c);
          break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
          bool down = (event.type == SDL_CONTROLLERBUTTONDOWN);
          int b = event.cbutton.button;
          if(b == SDL_CONTROLLER_BUTTON_LEFTSTICK || b == SDL_CONTROLLER_BUTTON_GUIDE) {
            if(down && g.mode == MODE_GAME) openMenu();
            else if(down && g.mode == MODE_MENU) g.mode = MODE_GAME;
            break;
          }
          int id = padToCore(b);
          if(isDirId(id)) {
            onDirection(id, down);
          } else if(g.mode == MODE_GAME) {
            if(id >= 0) corePad[id] = down;
          } else if(down) {
            if(b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_START) onConfirm();
            else if(b == SDL_CONTROLLER_BUTTON_Y && g.mode == MODE_SYSTEMS) chooseFolder();
            else if(b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) jumpLetter(-1);
            else if(b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) jumpLetter(1);
            else if(b == SDL_CONTROLLER_BUTTON_B) {
              // B backs out one screen at a time, and quits from the top one
              if(g.mode == MODE_MENU) g.mode = MODE_GAME;
              else if(g.mode == MODE_LIBRARY) switchMode(MODE_SYSTEMS);
              else g.running = false;
            }
          }
          break;
        }
        case SDL_CONTROLLERAXISMOTION: {
          int axis = event.caxis.axis;
          int16_t v = event.caxis.value;
          if(axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
            // held per trigger, because a system can map both to the same button (N64's Z)
            static bool trig[2];
            int t = (axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
            trig[t] = v > 8000;
            const int* map = systemPadMap(g.openSys);
            if(g.mode == MODE_GAME) {
              for(int i = 0; i < 2; i++) {
                int id = map[i == 0 ? PAD_LT : PAD_RT];
                if(id < 0) continue;
                bool other = map[i == 0 ? PAD_RT : PAD_LT] == id && trig[!i];
                corePad[id] = trig[i] || other;
              }
            }
            break;
          }
          if(axis == SDL_CONTROLLER_AXIS_RIGHTX) coreCStick[0] = g.mode == MODE_GAME ? v : 0;
          if(axis == SDL_CONTROLLER_AXIS_RIGHTY) coreCStick[1] = g.mode == MODE_GAME ? v : 0;
          if(axis != SDL_CONTROLLER_AXIS_LEFTX && axis != SDL_CONTROLLER_AXIS_LEFTY) break;
          if(g.mode == MODE_GAME) { // in game the stick is the stick, not a dpad
            coreStick[axis == SDL_CONTROLLER_AXIS_LEFTX ? 0 : 1] = v;
            break;
          }
          static bool held[4]; // up down left right, for menu navigation
          int neg = (axis == SDL_CONTROLLER_AXIS_LEFTY) ? 0 : 2;
          int pos = neg + 1;
          static const int ID[4] = {RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
                                    RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT};
          bool wantNeg = v < -12000, wantPos = v > 12000;
          if(held[neg] != wantNeg) { held[neg] = wantNeg; onDirection(ID[neg], wantNeg); }
          if(held[pos] != wantPos) { held[pos] = wantPos; onDirection(ID[pos], wantPos); }
          break;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
          bool down = (event.type == SDL_KEYDOWN);
          int key = event.key.keysym.sym;
          if(down && key == SDLK_ESCAPE) {
            if(g.mode == MODE_GAME) openMenu();
            else if(g.mode == MODE_MENU) g.mode = MODE_GAME;
            else if(g.mode == MODE_LIBRARY) switchMode(MODE_SYSTEMS);
            else g.running = false;
            break;
          }
          if(down && key == SDLK_F11) {
            toggleFullscreen();
            break;
          }
          if(down && key == SDLK_F5 && g.loaded) { saveState(); break; }
          if(down && key == SDLK_F8 && g.loaded) { loadState(); break; }
          int id = keyToCore(key);
          if(isDirId(id)) {
            onDirection(id, down);
          } else if(g.mode == MODE_GAME) {
            if(id >= 0) corePad[id] = down;
          } else if(down && (key == SDLK_RETURN || key == SDLK_SPACE)) {
            onConfirm();
          }
          break;
        }
        case SDL_MOUSEBUTTONDOWN:
          g.dragging = true;
          g.dragDist = 0;
          g.vel = 0;
          if(isListMode() && !g.scanning && rowCount() > 0 &&
             hit(scrollStrip(), event.button.x, event.button.y)) {
            g.scrubbing = true;
            scrubTo(event.button.y);
          }
          break;
        case SDL_MOUSEMOTION:
          if(g.scrubbing) {
            scrubTo(event.motion.y);
          } else if(g.dragging && isListMode()) {
            g.dragDist += abs(event.motion.yrel) + abs(event.motion.xrel);
            g.scroll -= event.motion.yrel;
            g.vel = (float) -event.motion.yrel;
          }
          break;
        case SDL_MOUSEBUTTONUP:
          g.dragging = false;
          if(g.scrubbing) {
            g.scrubbing = false;
          } else if(g.dragDist < 12) {
            g.vel = 0;
            handleTouch(event.button.x, event.button.y, event.button.clicks);
          } else {
            g.vel *= 1.8f; // let a flick actually travel
          }
          break;
        case SDL_MOUSEWHEEL:
          g.scroll -= event.wheel.y * g.rowH * 0.9f;
          g.vel = 0;
          break;
        case SDL_DROPFILE: {
          char* dropped = event.drop.file;
          if(isDir(dropped)) {
            setRomDir(dropped);
            g.mode = MODE_SYSTEMS;
            startScan(false, NULL);
          } else if(isRomPath(dropped)) {
            Rom direct;
            memset(&direct, 0, sizeof(direct));
            initRom(&direct, dropped);
            loadRom(&direct);
          }
          SDL_free(dropped);
          break;
        }
      }
    }

    if(g.scanning && SDL_AtomicGet(&g.scanDone)) {
      SDL_WaitThread(g.scanThread, NULL);
      g.scanThread = NULL;
      g.scanning = false;
      buildPresent(); // the machines the scan found, and the rows they make
      rebuildView();
      if(!g.scanIsStartup) { // startup speaks for itself, a manual rescan should report
        toast(g.romCount > 0 ? "Library updated" : g.reachable ? "No roms in that folder" : "Still can't reach it");
      }
    }

    if(g.pendingTap != 0 && (g.mode != MODE_GAME || SDL_GetTicks() >= g.pendingTap)) {
      bool fire = (g.mode == MODE_GAME);
      g.pendingTap = 0;
      if(fire) openMenu();
    }

    // ponytail: fixed 60Hz decay, the window is vsynced anyway
    if(!g.dragging && isListMode()) {
      g.scroll += g.vel;
      g.vel *= 0.96f; // longer glide, a 50000 row list needs it
      if(g.vel < 0.4f && g.vel > -0.4f) g.vel = 0;
    }
    if(g.scroll < 0) { g.scroll = 0; g.vel = 0; }
    if(g.scroll > maxScroll()) { g.scroll = maxScroll(); g.vel = 0; }

    uint64_t curCount = SDL_GetPerformanceCounter();
    float seconds = (curCount - lastCount) / (float) countFreq;
    lastCount = curCount;
    timeAdder += seconds;
    if(timeAdder > 0.25f) timeAdder = 0.25f; // don't catch up after a long stall
    if(!g.loaded || g.mode != MODE_GAME) {
      timeAdder = 0.0f;
    } else {
      float frameTime = coreFrameTime();
      // at most two frames per pass: an n64 frame is expensive enough that chasing a
      // backlog just digs the hole deeper
      for(int ran = 0; timeAdder >= frameTime - 0.002f && ran < 2; ran++) {
        timeAdder -= frameTime;
        coreRunFrame();
        updateFrame();
      }
    }

    if(g.scanning && isListMode()) {
      static const char* DOTS[] = {"Looking for roms", "Looking for roms.", "Looking for roms..", "Looking for roms..."};
      drawStatus(DOTS[(SDL_GetTicks() / 400) % 4], SDL_AtomicGet(&g.dirReady) ? g.romDir : g.scanArg);
    } else if(isListMode()) {
      drawList();
    } else if(g.mode == MODE_GAME) {
      drawGame();
      SDL_Rect b = gameMenuButton();
      fillRect(b.x, b.y, b.w, b.h, 20, 24, 34, 150);
      drawText(g.fSmall, "MENU", b.x + b.w / 2, b.y + (b.h - g.H / 30) / 2, COL_DIM, 1);
    } else {
      drawMenu();
    }
    if(g.toast[0] != '\0') {
      if(SDL_GetTicks() < g.toastUntil) {
        int th = g.H / 12;
        fillRect(0, g.H - th, g.W, th, 60, 110, 200, 230);
        drawText(g.fSmall, g.toast, g.W / 2, g.H - th + (th - g.H / 30) / 2, COL_TEXT, 1);
      } else {
        g.toast[0] = '\0';
      }
    }
    SDL_RenderPresent(g.ren);
  }

  // quitting mid-scan would otherwise sit on a share timeout with the window still up
  if(g.scanThread != NULL) {
    SDL_HideWindow(g.window);
    SDL_WaitThread(g.scanThread, NULL); // don't tear down under the worker
  }
  closeRom();
  coreClose();
  archiveCleanup(g.tmpDir); // unpacked disc images: gigabytes, and rebuilt in seconds
  for(int i = 0; i < g.romCount; i++) {
    if(g.roms[i].tex) SDL_DestroyTexture(g.roms[i].tex);
  }
  free(g.roms);
  free(g.view);
  if(g.fTitle) TTF_CloseFont(g.fTitle);
  if(g.fRow) TTF_CloseFont(g.fRow);
  if(g.fSmall) TTF_CloseFont(g.fSmall);
  TTF_Quit();
  if(g.frame) SDL_DestroyTexture(g.frame);
  SDL_DestroyRenderer(g.ren);
  SDL_DestroyWindow(g.window);
  SDL_Quit();
  return 0;
}
