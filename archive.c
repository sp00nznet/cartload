#include "archive.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "systems.h"

static char sevenZip[MAX_PATH];
static bool searched;

static const char* find7z(void) {
  if(searched) return sevenZip[0] != '\0' ? sevenZip : NULL;
  searched = true;
  // next to the exe first, so a portable 7za.exe beside Cartload wins over whatever is installed
  char* base = SDL_GetBasePath();
  const char* candidates[] = {"%s7za.exe", "%s7z.exe", NULL};
  for(int i = 0; candidates[i] != NULL; i++) {
    snprintf(sevenZip, sizeof(sevenZip), candidates[i], base ? base : ".\\");
    if(GetFileAttributesA(sevenZip) != INVALID_FILE_ATTRIBUTES) {
      SDL_free(base);
      return sevenZip;
    }
  }
  SDL_free(base);
  const char* env[] = {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)", NULL};
  for(int i = 0; env[i] != NULL; i++) {
    const char* dir = getenv(env[i]);
    if(dir == NULL) continue;
    snprintf(sevenZip, sizeof(sevenZip), "%s\\7-Zip\\7z.exe", dir);
    if(GetFileAttributesA(sevenZip) != INVALID_FILE_ATTRIBUTES) return sevenZip;
  }
  if(SearchPathA(NULL, "7z.exe", NULL, sizeof(sevenZip), sevenZip, NULL) > 0) return sevenZip;
  sevenZip[0] = '\0';
  return NULL;
}

bool archiveReady(void) {
  return find7z() != NULL;
}

// Run 7z with stdout on a pipe and collect everything it writes. `out` is grown as
// needed and is the caller's to free; pass NULL to run purely for the exit code.
static bool run7z(const char* args, uint8_t** out, size_t* outSize, char* err, size_t errLen) {
  const char* exe = find7z();
  if(exe == NULL) {
    snprintf(err, errLen, "7-Zip is not installed, so archives can't be opened");
    return false;
  }
  char cmd[2048];
  // a truncated command line is a different command: it would run 7-Zip against half a
  // path, so say so instead
  if(snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe, args) >= (int) sizeof(cmd)) {
    snprintf(err, errLen, "that path is too long for a 7-Zip command line");
    return false;
  }
  HANDLE readEnd = NULL, writeEnd = NULL;
  SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
  if(!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 20)) {
    snprintf(err, errLen, "could not open a pipe to 7-Zip");
    return false;
  }
  SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0); // only the child keeps the write end

  STARTUPINFOA si;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = writeEnd;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  si.hStdInput = NULL;
  PROCESS_INFORMATION pi;
  memset(&pi, 0, sizeof(pi));
  if(!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
    CloseHandle(readEnd);
    CloseHandle(writeEnd);
    snprintf(err, errLen, "could not run 7-Zip");
    return false;
  }
  CloseHandle(writeEnd); // ours must go, or the read below never sees end of file

  size_t cap = out != NULL ? (1 << 20) : 0, size = 0;
  uint8_t* buf = cap > 0 ? malloc(cap) : NULL;
  bool oom = (cap > 0 && buf == NULL);
  for(;;) {
    if(size == cap && !oom) {
      size_t want = cap * 2;
      uint8_t* bigger = realloc(buf, want);
      if(bigger == NULL) {
        oom = true;
      } else {
        buf = bigger;
        cap = want;
      }
    }
    char sink[65536];
    DWORD got = 0;
    void* dst = oom || buf == NULL ? sink : buf + size;
    DWORD room = oom || buf == NULL ? (DWORD) sizeof(sink) : (DWORD) (cap - size);
    if(!ReadFile(readEnd, dst, room, &got, NULL) || got == 0) break;
    if(!oom && buf != NULL) size += got; // on OOM keep draining, or 7z blocks forever
  }
  CloseHandle(readEnd);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  if(oom) {
    free(buf);
    snprintf(err, errLen, "not enough memory to unpack that");
    return false;
  }
  if(code != 0) { // 1 is a warning, 2+ is a real failure
    free(buf);
    snprintf(err, errLen, code == 1 ? "7-Zip could not read part of that archive" : "that archive is damaged");
    return false;
  }
  if(out != NULL) {
    *out = buf;
    *outSize = size;
  } else {
    free(buf);
  }
  return true;
}

bool archiveBestEntry(const char* archive, char* entry, size_t n) {
  char args[1200], err[256];
  snprintf(args, sizeof(args), "l -ba -slt -- \"%s\"", archive);
  uint8_t* listing = NULL;
  size_t size = 0;
  if(!run7z(args, &listing, &size, err, sizeof(err))) return false;

  // -slt prints one "Path = ...\n ... Size = ...\n" block per entry
  char bestRom[512] = {0}, bestAny[512] = {0}, current[512] = {0};
  long long bestRomSize = -1, bestAnySize = -1;
  bool found = false;
  for(char *line = (char*) listing, *end = (char*) listing + size; line < end;) {
    char* nl = memchr(line, '\n', (size_t) (end - line));
    size_t len = nl != NULL ? (size_t) (nl - line) : (size_t) (end - line);
    while(len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) len--;
    if(len > 7 && strncmp(line, "Path = ", 7) == 0) {
      size_t copy = len - 7 < sizeof(current) - 1 ? len - 7 : sizeof(current) - 1;
      memcpy(current, line + 7, copy);
      current[copy] = '\0';
    } else if(len > 7 && strncmp(line, "Size = ", 7) == 0 && current[0] != '\0') {
      long long bytes = atoll(line + 7);
      char ext[24];
      pathExt(current, ext, sizeof(ext));
      found = true;
      if(bytes > bestAnySize) {
        bestAnySize = bytes;
        snprintf(bestAny, sizeof(bestAny), "%s", current);
      }
      // a rom we recognise beats a bigger file we don't -- archives carry readmes,
      // box art and the odd nested manual
      if(bytes > bestRomSize && !isArchiveExt(ext) && systemForExt(ext) != NULL) {
        bestRomSize = bytes;
        snprintf(bestRom, sizeof(bestRom), "%s", current);
      }
      current[0] = '\0';
    }
    line = nl != NULL ? nl + 1 : end;
  }
  free(listing);
  if(!found) return false;
  snprintf(entry, n, "%s", bestRom[0] != '\0' ? bestRom : bestAny);
  return entry[0] != '\0';
}

// 7-Zip reads * and ? in a name as wildcards, so an entry containing either has to be
// left off the command line. Nearly every rom archive holds one file anyway.
static bool quotable(const char* entry) {
  return strchr(entry, '*') == NULL && strchr(entry, '?') == NULL && strchr(entry, '"') == NULL;
}

uint8_t* archiveExtract(const char* archive, const char* entry, size_t* sizeOut, char* err, size_t errLen) {
  char args[2000];
  if(quotable(entry)) {
    snprintf(args, sizeof(args), "e -so -- \"%s\" \"%s\"", archive, entry);
  } else {
    snprintf(args, sizeof(args), "e -so -- \"%s\"", archive); // all of it; single-entry archives only
  }
  uint8_t* data = NULL;
  size_t size = 0;
  if(!run7z(args, &data, &size, err, errLen)) return NULL;
  if(size == 0) {
    free(data);
    snprintf(err, errLen, "that archive unpacked to nothing");
    return NULL;
  }
  *sizeOut = size;
  return data;
}

bool archiveExtractToFile(const char* archive, const char* entry, const char* dir, char* out, size_t n, char* err,
                          size_t errLen) {
  const char* leaf = strrchr(entry, '/');
  const char* back = strrchr(entry, '\\');
  if(back != NULL && (leaf == NULL || back > leaf)) leaf = back;
  leaf = leaf != NULL ? leaf + 1 : entry;
  snprintf(out, n, "%s\\%s", dir, leaf);
  // already unpacked from an earlier launch of the same game: reuse it, these are big
  if(GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return true;

  CreateDirectoryA(dir, NULL);
  char args[2200];
  if(quotable(entry)) {
    snprintf(args, sizeof(args), "e -y -o\"%s\" -- \"%s\" \"%s\"", dir, archive, entry);
  } else {
    snprintf(args, sizeof(args), "e -y -o\"%s\" -- \"%s\"", dir, archive);
  }
  if(!run7z(args, NULL, NULL, err, errLen)) return false;
  if(GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) {
    snprintf(err, errLen, "7-Zip did not produce %s", leaf);
    return false;
  }
  return true;
}

void archiveCleanup(const char* dir) {
  char pattern[MAX_PATH];
  snprintf(pattern, sizeof(pattern), "%s\\*", dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if(h == INVALID_HANDLE_VALUE) return;
  do {
    if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    char full[MAX_PATH];
    snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
    DeleteFileA(full);
  } while(FindNextFileA(h, &fd));
  FindClose(h);
}
