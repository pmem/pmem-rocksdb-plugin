/* SPDX-License-Identifier: BSD-3-Clause
* Copyright(c) 2021 Intel Corporation
*/
#pragma once

#include <libpmem.h>

#include <chrono>
#include <ctime>
#include <thread>

#include "pmem_scache_util.h"

namespace ROCKSDB_NAMESPACE {
class PMemSecondaryCache : public SecondaryCache {
 public:
  explicit PMemSecondaryCache(const PMemAllocatorOptions& opt);
  ~PMemSecondaryCache() override { Close(); }

  const char* Name() const override { return "PMemSecondaryCache"; }

  Status Insert(const Slice& key, void* value,
                const Cache::CacheItemHelper* helper) override;

  static void InsertMain(PMemSecondaryCache* s_cache);
  void Close();

  std::unique_ptr<SecondaryCacheResultHandle> Lookup(
      const Slice& key, const Cache::CreateCallback& create_cb,
      bool /*wait*/) override {
    size_t charge = 0;
    void* value = nullptr;
    Status s = ecache_ptr->Lookup(key, &value, &charge, create_cb);
    std::unique_ptr<SecondaryCacheResultHandle> secondary_handle;
    if (s.ok()) {
      secondary_handle.reset(new PMemSCacheResultHandle(value, charge));
    }

    return secondary_handle;
  }

  void Erase(const Slice& /*key*/) override {
    // not support
  }

  void WaitAll(std::vector<SecondaryCacheResultHandle*> handles) override {
    for (SecondaryCacheResultHandle* handle : handles) {
      auto* sec_handle = static_cast<PMemSCacheResultHandle*>(handle);
      sec_handle->SetReady();
    }
  }

  std::string GetPrintableOptions() const override { return ""; }

 private:
  static const int kMaxRetry = 3;
  const PMemAllocatorOptions option_;
  std::shared_ptr<MemoryAllocator> allocator_;
  BoundedQueue<InsertOp> insert_queue_;
  std::thread insert_thread_;
  std::thread evict_thread_;
  std::shared_ptr<ECache> ecache_ptr;
};

}  // namespace ROCKSDB_NAMESPACE
