/* SPDX-License-Identifier: BSD-3-Clause
* Copyright(c) 2021 Intel Corporation
*/

#pragma once
#include <sys/mman.h>

#include "rocksdb/file_system.h"
#include "rocksdb/status.h"

#define UNUSED(x) ((void)(x))

namespace ROCKSDB_NAMESPACE {
class PMemWritableFile : public FSWritableFile {
 public:
  // create a new WAL file on PMem
  static IOStatus Create(const std::string& fname, FSWritableFile** p_file,
                         size_t init_size, size_t size_addition);

  static IOStatus CheckArguments(size_t init_size, size_t size_addition);

  static IOStatus MapFile(int fd, size_t init_size, size_t size_addition,
                          FSWritableFile** p_file);

  // memcpy non-temporally
  static void MemcpyNT(void* dst, const void* src, size_t len);

  // set a 64-bit non-temporally
  static void SetSizeNT(size_t* dst, size_t value);
  virtual IOStatus Append(const Slice& slice, const IOOptions& options,
                          IODebugContext* dbg) override;
  virtual IOStatus Append(const Slice& data, const IOOptions& opts,
                          const DataVerificationInfo& /* verification_info */,
                          IODebugContext* dbg) override {
    return Append(data, opts, dbg);
  }
  IOStatus Truncate(uint64_t size, const IOOptions& options,
                    IODebugContext* dbg) override;
  IOStatus Close(const IOOptions& options, IODebugContext* dbg) override;
  IOStatus Flush(const IOOptions& options, IODebugContext* dbg) override;
  IOStatus Sync(const IOOptions& options, IODebugContext* dbg) override;
  uint64_t GetFileSize(const IOOptions& /*options*/,
                       IODebugContext* /*dbg*/) override;

 private:
  // this flag strictly requires the file system to be on PMem
  static const int MMAP_FLAGS = MAP_SHARED;
  size_t data_length;    // the writen data length
  size_t file_size;      // the length of the whole file
  size_t size_addition;  // expand size_addition bytes
                         // every time left space is not enough
  int fd;                // the file descriptor
  size_t* p_length;      // the first size_t to record data length
  void* map_base;        // mmap()-ed area
  size_t map_length;     // mmap()-ed length
  uint8_t* buffer;       // the base address of buffer to write
  PMemWritableFile(int _fd, size_t* _p_length, void* base, size_t init_size,
                    size_t _size_addition);
};

class PMemSequentialFile : public FSSequentialFile {
 public:
  static IOStatus Open(const std::string& fname, FSSequentialFile** p_file);
  IOStatus Read(size_t n, const IOOptions& options, Slice* result,
                char* scratch, IODebugContext* dbg) override;
  IOStatus Skip(uint64_t n) override;
  ~PMemSequentialFile();

 private:
  PMemSequentialFile(void* base, size_t _file_size);

  size_t file_size;    // the length of the whole file
  uint8_t* data;       // the base address of data
  size_t data_length;  // the length of data
  size_t seek;         // next offset to read
};

}  // namespace ROCKSDB_NAMESPACE