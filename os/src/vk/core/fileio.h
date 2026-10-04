// src/vk/core/fileio.h
// The one file layer every store uses (wallet/stores.md, Common rules). A store never calls the
// filesystem directly: every read, write, rename and remove goes through vk::fileio::ops.
// Paths are LittleFS-relative ("/vk/history.bin"). The firmware implementation (fileio.cpp)
// prepends FS_ROOT and uses POSIX stdio; the host tests install an in-memory one behind the
// same pointer.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vk::fileio {
struct Ops {
  bool (*exists)(const char *path);
  long (*size)(const char *path);
  bool (*read)(const char *path, size_t offset, uint8_t *out, size_t len);
  bool (*writeAll)(const char *path, const uint8_t *data, size_t len);
  bool (*writeAt)(const char *path, size_t offset, const uint8_t *data, size_t len);
  bool (*renameFile)(const char *from, const char *to);
  bool (*removeFile)(const char *path);
  bool (*makeDir)(const char *path);
};
extern const Ops *ops;
}
