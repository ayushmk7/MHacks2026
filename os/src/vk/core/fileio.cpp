// src/vk/core/fileio.cpp
// The firmware implementation of vk::fileio::ops: POSIX stdio on FS_ROOT + path (upstream mounts
// LittleFS there). The host tests do not link this file; their shim defines `ops` itself.
#ifndef VK_HOST_TEST

#include "fileio.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../config.h"   // FS_ROOT

namespace vk::fileio {
namespace {

constexpr size_t MAX_PATH_BYTES = 128;

// FS_ROOT + path. Refuses a path that is empty, not absolute, or too long.
bool fullPath(const char *path, char out[MAX_PATH_BYTES]) {
  if (path == nullptr || path[0] != '/') return false;
  const int written = snprintf(out, MAX_PATH_BYTES, "%s%s", FS_ROOT, path);
  return written > 0 && (size_t)written < MAX_PATH_BYTES;
}

bool fwExists(const char *path) {
  char full[MAX_PATH_BYTES];
  if (!fullPath(path, full)) return false;
  struct stat info;
  return stat(full, &info) == 0;
}

long fwSize(const char *path) {
  char full[MAX_PATH_BYTES];
  if (!fullPath(path, full)) return -1;
  struct stat info;
  if (stat(full, &info) != 0) return -1;
  return (long)info.st_size;
}

bool fwRead(const char *path, size_t offset, uint8_t *out, size_t len) {
  char full[MAX_PATH_BYTES];
  if (!fullPath(path, full)) return false;
  FILE *file = fopen(full, "rb");
  if (file == nullptr) return false;
  bool ok = fseek(file, (long)offset, SEEK_SET) == 0;
  if (ok && len > 0) ok = fread(out, 1, len, file) == len;
  fclose(file);
  return ok;
}

// Writes `len` bytes at `offset` of a file opened with `mode`. fclose() is checked: it is where a
// full filesystem shows up.
bool writeWith(const char *path, const char *mode, size_t offset, const uint8_t *data, size_t len) {
  char full[MAX_PATH_BYTES];
  if (!fullPath(path, full)) return false;
  FILE *file = fopen(full, mode);
  if (file == nullptr) return false;
  bool ok = offset == 0 || fseek(file, (long)offset, SEEK_SET) == 0;
  if (ok && len > 0) ok = fwrite(data, 1, len, file) == len;
  if (fclose(file) != 0) ok = false;
  return ok;
}

bool fwWriteAll(const char *path, const uint8_t *data, size_t len) {
  return writeWith(path, "wb", 0, data, len);
}

// The file must already exist: "r+b" does not create it.
bool fwWriteAt(const char *path, size_t offset, const uint8_t *data, size_t len) {
  return writeWith(path, "r+b", offset, data, len);
}

bool fwRename(const char *from, const char *to) {
  char fullFrom[MAX_PATH_BYTES];
  char fullTo[MAX_PATH_BYTES];
  if (!fullPath(from, fullFrom) || !fullPath(to, fullTo)) return false;
  return rename(fullFrom, fullTo) == 0;
}

bool fwRemove(const char *path) {
  char full[MAX_PATH_BYTES];
  if (!fullPath(path, full)) return false;
  return unlink(full) == 0;
}

// Succeeds if the directory already exists.
bool fwMakeDir(const char *path) {
  char full[MAX_PATH_BYTES];
  if (!fullPath(path, full)) return false;
  struct stat info;
  if (stat(full, &info) == 0) return S_ISDIR(info.st_mode);
  return mkdir(full, 0777) == 0;
}

const Ops kFirmwareOps = {
    fwExists, fwSize, fwRead, fwWriteAll, fwWriteAt, fwRename, fwRemove, fwMakeDir,
};

}  // namespace

const Ops *ops = &kFirmwareOps;

}  // namespace vk::fileio

#endif  // VK_HOST_TEST
