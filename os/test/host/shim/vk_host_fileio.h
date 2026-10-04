// test/host/shim/vk_host_fileio.h
// Host-test stand-in for the file layer: an in-memory implementation of vk::fileio::Ops
// (src/vk/core/fileio.h). The shim defines vk::fileio::ops itself, already pointing at the
// in-memory implementation, so a host suite must NOT list src/vk/core/fileio.cpp on its LINK line.
//
// It behaves like the firmware implementation (POSIX stdio on LittleFS) where a store could tell
// the difference:
//   - a path must be absolute ("/vk/history.bin") and at most 118 characters, else the call fails;
//   - "/" always exists; any other directory exists only after makeDir(), and writeAll(),
//     renameFile() and makeDir() fail when the parent directory of the target does not exist;
//   - makeDir() succeeds if the directory already exists and fails if a file has that name;
//   - writeAt() fails if the file does not exist; writing past the end grows the file (a gap is
//     filled with zeros);
//   - read() fails unless all `len` bytes at `offset` exist (len 0 succeeds on any existing file);
//   - size() is -1 for a missing file; renameFile() replaces an existing target;
//     removeFile() removes files only.
//
// Test hooks:
//   vk_host_fileio_install()       point vk::fileio::ops at the in-memory implementation (it already
//                                  is at start-up; call this if a test replaced the pointer)
//   vk_host_fileio_reset()         delete every file and directory; clear vk_host_fileio_fail_writes
//   vk_host_fileio_fail_writes     > 0: the next N writeAll / writeAt / renameFile calls fail and change
//                                  nothing (it counts down); < 0: every such call fails until it is 0 again
#pragma once

#include "../../../src/vk/core/fileio.h"

void vk_host_fileio_install();
void vk_host_fileio_reset();
extern int vk_host_fileio_fail_writes;
