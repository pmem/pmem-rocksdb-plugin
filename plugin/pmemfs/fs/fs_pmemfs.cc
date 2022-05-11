/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */
#include "fs_pmemfs.h"

#include <fcntl.h>
#include <unistd.h>

#include "io_pmemfs.h"
#include "rocksdb/utilities/object_registry.h"
#include "rocksdb/utilities/options_type.h"

namespace ROCKSDB_NAMESPACE {

static std::unordered_map<std::string, OptionTypeInfo> pmemfs_type_info = {
    {"wal_init_size",
     {offsetof(struct PMemFSOptions, wal_init_size), OptionType::kSizeT,
      OptionVerificationType::kNormal, OptionTypeFlags::kNone}},
    {"wal_size_addition",
     {offsetof(struct PMemFSOptions, wal_size_addition), OptionType::kSizeT,
      OptionVerificationType::kNormal, OptionTypeFlags::kNone}},
};

PMemFS::PMemFS(const PMemFSOptions& opt)
    : FileSystemWrapper(FileSystem::Default()), opt_(opt) {
  RegisterOptions(&opt_, &pmemfs_type_info);
}

bool PMemFS::EndsWith(const std::string& str, const std::string& suffix) {
  auto str_len = str.length();
  auto suffix_len = suffix.length();
  if (str_len < suffix_len) {
    return false;
  }
  return memcmp(str.c_str() + str_len - suffix_len, suffix.c_str(),
                suffix_len) == 0;
}

bool PMemFS::IsWALFile(const std::string& fname) {
  return EndsWith(fname, ".log");
}

IOStatus PMemFS::NewWritableFile(const std::string& fname,
                                 const FileOptions& file_opts,
                                 std::unique_ptr<FSWritableFile>* result,
                                 IODebugContext* dbg) {
  // if it is not a log file, then fall back to original logic
  if (!IsWALFile(fname)) {
    return target()->NewWritableFile(fname, file_opts, result, dbg);
  }
  // else it is a log file
  FSWritableFile* file = nullptr;
  // create it
  IOStatus status = PMemWritableFile::Create(
      fname, &file, opt_.wal_init_size, opt_.wal_size_addition);

  if (status.ok()) {
    result->reset(file);
  }
  return status;
}

IOStatus PMemFS::DeleteFile(const std::string& fname,
                            const IOOptions& io_options, IODebugContext* dbg) {
  return target()->DeleteFile(fname, io_options, dbg);
}

// The WAL file may have extra format, so should be handled before reading
IOStatus PMemFS::NewSequentialFile(const std::string& fname,
                                   const FileOptions& file_opts,
                                   std::unique_ptr<FSSequentialFile>* result,
                                   IODebugContext* dbg) {
  // if it is not a log file, then fall back to original logic
  if (!IsWALFile(fname)) {
    return target()->NewSequentialFile(fname, file_opts, result, dbg);
  }
  FSSequentialFile* file;
  auto status = PMemSequentialFile::Open(fname, &file);
  if (status.ok()) {
    result->reset(file);
  }
  return status;
}

IOStatus PMemFS::GetFileSize(const std::string& fname, const IOOptions& opts,
                             uint64_t* s, IODebugContext* dbg) {
  if (!IsWALFile(fname)) {
    return target()->GetFileSize(fname, opts, s, dbg);
  }
  int fd = open(fname.c_str(), O_RDONLY);
  if (fd < 0) {
    return IOStatus::IOError(std::string("open '")
                                 .append(fname)
                                 .append("' failed: ")
                                 .append(strerror(errno)));
  }
  void* p_length = mmap(nullptr, sizeof(size_t), PROT_READ, MAP_SHARED, fd, 0);
  if (p_length == MAP_FAILED) {
    close(fd);
    return IOStatus::IOError(
        std::string("mmap first size_t failed: ").append(strerror(errno)));
  }
  *s = *(uint64_t*)p_length;
  return IOStatus::OK();
}

Status PMemFS::PrepareOptions(const ConfigOptions& /*config_options*/) {
  if (opt_.wal_init_size <= 4L * 1024 * 1024 ||
      opt_.wal_size_addition <= 4L * 1024 * 1042) {
    return Status::InvalidArgument("invalid size for pmemfs");
  }
  return Status::OK();
}

extern "C" FactoryFunc<FileSystem> pmemfs_reg;
FactoryFunc<FileSystem> pmemfs_reg =
    ObjectLibrary::Default()->Register<FileSystem>(
        PMemFS::kClassName(),
        [](const std::string& /*uri*/, std::unique_ptr<FileSystem>* f,
           std::string* /*errmsg*/) {
          PMemFSOptions options;
          auto* pmem_fs = new PMemFS(options);
          f->reset(pmem_fs);
          return f->get();
        });
}  // namespace ROCKSDB_NAMESPACE