/* SPDX-License-Identifier: BSD-3-Clause
* Copyright(c) 2021 Intel Corporation
 */
#pragma once

#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "rocksdb/env.h"
#include "rocksdb/file_system.h"
#include "rocksdb/status.h"

#define UNUSED(x) ((void)(x))

namespace ROCKSDB_NAMESPACE {

#if !defined(ROCKSDB_LITE) && defined(OS_LINUX)

// extra options for PMemFS
struct PMemOptions {
  // the initial allocation size of wal file
  // default 256 MB
  size_t wal_init_size = 256 << 20;
  // expand wal file with this size every time it is not enough
  // default 64 MB
  size_t wal_size_addition = 64 << 20;
};

class PMemFS : public FileSystemWrapper {
 public:
  explicit PMemFS();
  const char* Name() const override { return "PMemFS"; }

  // we should use PMem-awared filesystem for WAL file
  IOStatus NewWritableFile(const std::string& fname,
                           const FileOptions& file_opts,
                           std::unique_ptr<FSWritableFile>* result,
                           IODebugContext* dbg) override;

  // if to delete WAL file, we should recycle it to avoid zero-out again
  IOStatus DeleteFile(const std::string& fname, const IOOptions& options,
                      IODebugContext* dbg) override;

  // The WAL file may have extra format, so should be handled before reading
  IOStatus NewSequentialFile(const std::string& fname, const FileOptions& file_opts,
                             std::unique_ptr<FSSequentialFile>* result,
                             IODebugContext* dbg) override;

  IOStatus GetFileSize(const std::string& fname, const IOOptions& opts,
                       uint64_t* s, IODebugContext* dbg) override;

 private:
  PMemOptions options;

  static bool IsWALFile(const std::string& fname);
  static bool EndsWith(const std::string& str, const std::string& suffix);
};

Status NewPMemFS(FileSystem** fs);

#endif  // !defined(ROCKSDB_LITE) && defined(OS_LINUX)

}  // namespace ROCKSDB_NAMESPACE