// test/host/shim/vk_host_fileio.cpp - in-memory vk::fileio::Ops. See vk_host_fileio.h.
#include "vk_host_fileio.h"

#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

int vk_host_fileio_fail_writes = 0;

namespace {

// The firmware builds FS_ROOT + path into 128 bytes; FS_ROOT is "/littlefs" (9 characters).
constexpr size_t MAX_PATH_CHARS = 118;

// Function-local so that they exist before any static object that touches files.
std::map<std::string, std::vector<uint8_t>> &files() {
  static std::map<std::string, std::vector<uint8_t>> f;
  return f;
}
std::set<std::string> &dirs() {
  static std::set<std::string> d;
  return d;
}

// Absolute, not too long; trailing slashes dropped ("/vk/" is "/vk"). Empty result = refused.
std::string clean(const char *path) {
  if (path == nullptr || path[0] != '/' || strlen(path) > MAX_PATH_CHARS) return std::string();
  std::string p(path);
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

bool isDir(const std::string &p) { return p == "/" || dirs().count(p) != 0; }
bool isFile(const std::string &p) { return files().count(p) != 0; }

bool parentExists(const std::string &p) {
  const size_t slash = p.rfind('/');
  if (slash == std::string::npos) return false;
  return isDir(slash == 0 ? std::string("/") : p.substr(0, slash));
}

// True if this write must fail; counts a positive vk_host_fileio_fail_writes down.
bool injectedFailure() {
  if (vk_host_fileio_fail_writes > 0) { vk_host_fileio_fail_writes--; return true; }
  return vk_host_fileio_fail_writes < 0;
}

bool memExists(const char *path) {
  const std::string p = clean(path);
  return !p.empty() && (isFile(p) || isDir(p));
}

long memSize(const char *path) {
  const std::string p = clean(path);
  if (p.empty() || !isFile(p)) return -1;
  return (long)files()[p].size();
}

bool memRead(const char *path, size_t offset, uint8_t *out, size_t len) {
  const std::string p = clean(path);
  if (p.empty() || !isFile(p)) return false;
  const std::vector<uint8_t> &data = files()[p];
  if (len == 0) return true;
  if (out == nullptr || offset > data.size() || len > data.size() - offset) return false;
  memcpy(out, data.data() + offset, len);
  return true;
}

bool memWriteAll(const char *path, const uint8_t *data, size_t len) {
  const std::string p = clean(path);
  if (p.empty() || p == "/" || isDir(p) || !parentExists(p) || (data == nullptr && len != 0)) return false;
  if (injectedFailure()) return false;
  files()[p].assign(data, data + len);
  return true;
}

bool memWriteAt(const char *path, size_t offset, const uint8_t *data, size_t len) {
  const std::string p = clean(path);
  if (p.empty() || !isFile(p) || (data == nullptr && len != 0)) return false;
  if (injectedFailure()) return false;
  std::vector<uint8_t> &file = files()[p];
  if (len == 0) return true;
  if (file.size() < offset + len) file.resize(offset + len, 0);
  memcpy(file.data() + offset, data, len);
  return true;
}

bool memRename(const char *from, const char *to) {
  const std::string a = clean(from), b = clean(to);
  if (a.empty() || b.empty() || !isFile(a) || isDir(b) || !parentExists(b)) return false;
  if (injectedFailure()) return false;
  if (a == b) return true;
  files()[b] = files()[a];
  files().erase(a);
  return true;
}

bool memRemove(const char *path) {
  const std::string p = clean(path);
  if (p.empty()) return false;
  return files().erase(p) == 1;
}

bool memMakeDir(const char *path) {
  const std::string p = clean(path);
  if (p.empty() || isFile(p)) return false;
  if (isDir(p)) return true;
  if (!parentExists(p)) return false;
  dirs().insert(p);
  return true;
}

const vk::fileio::Ops kMemoryOps = {
    memExists, memSize, memRead, memWriteAll, memWriteAt, memRename, memRemove, memMakeDir,
};

}  // namespace

// The firmware defines this in src/vk/core/fileio.cpp, which host suites do not link.
namespace vk::fileio {
const Ops *ops = &kMemoryOps;
}

void vk_host_fileio_install() { vk::fileio::ops = &kMemoryOps; }

void vk_host_fileio_reset() {
  files().clear();
  dirs().clear();
  vk_host_fileio_fail_writes = 0;
}
