// Half the library is .zip/.7z/.rar, so unwrapping one is not an optional extra.
//
// ponytail: this shells out to 7-Zip rather than linking a decompressor. One small
// module covers zip, 7z and rar -- zlib would have covered deflate-in-zip only, and
// 10k of the roms here are .7z. If 7z.exe ever stops being a given, swap the two
// run7z() calls for libarchive; nothing above this file would notice.
#ifndef ARCHIVE_H
#define ARCHIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool archiveReady(void); // 7z.exe was found; false means archives cannot be opened

// The one file inside worth running: a known rom extension if there is one, else the
// biggest entry. False if the archive is unreadable or holds nothing usable.
bool archiveBestEntry(const char* archive, char* entry, size_t n);

// Contents of `entry` in a malloc'd buffer, for cores that take the rom in memory.
uint8_t* archiveExtract(const char* archive, const char* entry, size_t* sizeOut, char* err, size_t errLen);

// Same, but written to `dir` and the path handed back, for cores that need_fullpath.
bool archiveExtractToFile(const char* archive, const char* entry, const char* dir, char* out, size_t n, char* err,
                          size_t errLen);

void archiveCleanup(const char* dir); // delete what archiveExtractToFile() left behind

#endif
