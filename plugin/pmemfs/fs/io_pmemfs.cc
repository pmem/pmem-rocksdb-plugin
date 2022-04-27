/* SPDX-License-Identifier: BSD-3-Clause
* Copyright(c) 2021 Intel Corporation
 */

#if !defined(ROCKSDB_LITE) && !defined(OS_WIN)

#include "io_pmemfs.h"

#include <errno.h>
#include <fcntl.h>
#include <libpmem.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ROCKSDB_NAMESPACE {
PMemWritableFile::PMemWritableFile(int _fd, size_t* _p_length, void* base,
                                   size_t init_size, size_t _size_addition)
    : data_length(0),
      file_size(init_size),
      size_addition(_size_addition),
      fd(_fd),
      p_length(_p_length),
      map_base(base),
      map_length(init_size),
      buffer((uint8_t*)base + sizeof(size_t)) {
  // reset the data length
  SetSizeNT(p_length, 0);
}

IOStatus PMemWritableFile::Create(const std::string& fname,
                                  FSWritableFile** p_file, size_t init_size,
                                  size_t size_addition) {
  auto s = CheckArguments(init_size, size_addition);
  if (!s.ok()) {
    return s;
  }
  // explicit create new file
  int fd = open(fname.c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
  if (fd < 0) {
    return IOStatus::IOError(std::string("create '")
                                 .append(fname)
                                 .append("' failed: ")
                                 .append(strerror(errno)));
  }
  // pre-allocate space
  if (fallocate(fd, 0, 0, init_size) != 0) {
    close(fd);
    return IOStatus::IOError(std::string("fallocate '")
                                 .append(fname)
                                 .append("' failed: ")
                                 .append(strerror(errno)));
  }
  return MapFile(fd, init_size, size_addition, p_file);
}

IOStatus PMemWritableFile::CheckArguments(size_t init_size,
                                          size_t size_addition) {
  if (init_size < sizeof(size_t)) {
    return IOStatus::InvalidArgument("too small init_size");
  }
  if (size_addition == 0) {
    return IOStatus::InvalidArgument("size_addition is zero");
  }
  return IOStatus::OK();
}

IOStatus PMemWritableFile::MapFile(int fd, size_t init_size,
                                   size_t size_addition,
                                   FSWritableFile** p_file) {
  // first size_t is to record length of data
  void* p_length = mmap(nullptr, sizeof(size_t), PROT_WRITE, MMAP_FLAGS, fd, 0);
  if (p_length == MAP_FAILED) {
    close(fd);
    return IOStatus::IOError(
        std::string("mmap first size_t failed: ").append(strerror(errno)));
  }
  // whole file
  void* base = mmap(nullptr, init_size, PROT_WRITE, MMAP_FLAGS, fd, 0);
  if (base == MAP_FAILED) {
    close(fd);
    munmap(p_length, sizeof(size_t));
    return IOStatus::IOError(
        std::string("mmap whole file failed: ").append(strerror(errno)));
  }
  *p_file = new PMemWritableFile(fd, (size_t*)p_length, base, init_size,
                                 size_addition);
  return IOStatus::OK();
}

void PMemWritableFile::MemcpyNT(void* dst, const void* src, size_t len) {
  pmem_memcpy(dst, src, len, PMEM_F_MEM_NONTEMPORAL);
}

void PMemWritableFile::SetSizeNT(size_t* dst, size_t value) {
  assert(sizeof(size_t) == 8);
  assert((size_t)dst % sizeof(size_t) == 0);
  MemcpyNT(dst, (void*)&value, sizeof(size_t));
}

IOStatus PMemWritableFile::Append(const Slice& slice, const IOOptions& options,
                                  IODebugContext* dbg) {
  UNUSED(options);
  UNUSED(dbg);
  size_t len = slice.size();
  // the least file size to write the new slice
  size_t least_file_size = sizeof(size_t) + data_length + len;
  // left space is not enough, need expand
  if (least_file_size > file_size) {
    // the multiple of size_addition to add
    size_t count = (least_file_size - file_size - 1) / size_addition + 1;
    size_t add_size = count * size_addition;
    // allocate new space
    if (fallocate(fd, 0, file_size, add_size) != 0) {
      return IOStatus::IOError(
          std::string("expand file failed: ").append(strerror(errno)));
    }
    size_t new_file_size = file_size + add_size;
    assert(new_file_size >= least_file_size);
    // the offset in file to mmap(), align to 4K
    size_t offset = sizeof(size_t) + data_length;
    size_t offset_aligned = offset & ~(4096 - 1);
    assert(offset_aligned <= offset);
    // mmaped() the new area, but because aligned,
    // usually overlip with current mmap()-ed range
    size_t new_map_length = new_file_size - offset_aligned;
    void* new_map_base =
        mmap(0, new_map_length, PROT_WRITE, MMAP_FLAGS, fd, offset_aligned);
    if (new_map_base == MAP_FAILED) {
      return IOStatus::IOError(
          std::string("mmap new range failed: ").append(strerror(errno)));
    }
    // unmap the previous range
    munmap(map_base, map_length);
    file_size = new_file_size;
    map_base = new_map_base;
    map_length = new_map_length;
    buffer = (uint8_t*)new_map_base + offset - offset_aligned - data_length;
  }
  MemcpyNT(buffer + data_length, slice.data(), len);
  data_length += len;
  SetSizeNT(p_length, data_length);
  return IOStatus::OK();
}

IOStatus PMemWritableFile::Truncate(uint64_t size, const IOOptions& options,
                                    IODebugContext* dbg) {
  UNUSED(options);
  UNUSED(dbg);
  data_length = size;
  SetSizeNT(p_length, data_length);
  return IOStatus::OK();
}

IOStatus PMemWritableFile::Close(const IOOptions& options,
                                 IODebugContext* dbg) {
  UNUSED(options);
  UNUSED(dbg);
  close(fd);
  munmap(p_length, sizeof(size_t));
  munmap(map_base, map_length);
  return IOStatus::OK();
}

IOStatus PMemWritableFile::Flush(const IOOptions& options,
                                 IODebugContext* dbg) {
  UNUSED(options);
  UNUSED(dbg);
  // PMem need no flush
  return IOStatus::OK();
}

IOStatus PMemWritableFile::Sync(const IOOptions& options, IODebugContext* dbg) {
  UNUSED(options);
  UNUSED(dbg);
  return IOStatus::OK();
}

uint64_t PMemWritableFile::GetFileSize(const IOOptions& options,
                                       IODebugContext* dbg) {
  UNUSED(options);
  UNUSED(dbg);
  // it is the data length, not the physical space size
  return data_length;
}

IOStatus PMemSequentialFile::Open(const std::string& fname,
                                  FSSequentialFile** p_file) {
  int fd = open(fname.c_str(), O_RDONLY);
  if (fd < 0) {
    return IOStatus::IOError(std::string("open '")
                                 .append(fname)
                                 .append("' failed: ")
                                 .append(strerror(errno)));
  }
  // get the file size
  struct stat stat;
  if (fstat(fd, &stat) < 0) {
    close(fd);
    return IOStatus::IOError(std::string("fstat '")
                                 .append(fname)
                                 .append("' failed: ")
                                 .append(strerror(errno)));
  }
  // whole file
  void* base = mmap(0, (size_t)stat.st_size, PROT_READ, MAP_SHARED, fd, 0);
  // no matter map ok or fail, we can close now
  close(fd);
  if (base == MAP_FAILED) {
    return IOStatus::IOError(
        std::string("mmap file failed: ").append(strerror(errno)));
  }
  *p_file = new PMemSequentialFile(base, (size_t)stat.st_size);
  return IOStatus::OK();
}

IOStatus PMemSequentialFile::Read(size_t n, const IOOptions& options,
                                  Slice* result, char* scratch,
                                  IODebugContext* dbg) {
  UNUSED(options);
  UNUSED(scratch);
  UNUSED(dbg);
  assert(seek <= data_length);
  auto len = std::min(data_length - seek, n);

  *result = Slice((char*)data + seek, len);
  seek += len;
  return IOStatus::OK();
}

IOStatus PMemSequentialFile::Skip(uint64_t n) {
  assert(seek <= data_length);
  auto len = std::min(data_length - seek, n);
  seek += len;
  return IOStatus::OK();
}

PMemSequentialFile::~PMemSequentialFile() {
  void* base = (void*)(data - sizeof(size_t));
  munmap(base, file_size);
}

PMemSequentialFile::PMemSequentialFile(void* base, size_t _file_size)
    : file_size(_file_size) {
  data_length = *((size_t*)base);
  data = (uint8_t*)base + sizeof(size_t);
  seek = 0;
}

}  // namespace ROCKSDB_NAMESPACE
#endif