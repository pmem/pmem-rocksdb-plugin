/* SPDX-License-Identifier: BSD-3-Clause
* Copyright(c) 2021 Intel Corporation
*/
#pragma once

#include <libpmem.h>
#include <thread>

#include "pmem_scache_util.h"

namespace ROCKSDB_NAMESPACE {
class PMemSecondaryCache : public SecondaryCache {
 public:
  explicit PMemSecondaryCache(const PMemAllocatorOptions& opt);
  ~PMemSecondaryCache() override {
    cache_.reset();
  }

  const char* Name() const override { return "PMemSecondaryCache"; }

  Status Insert(const Slice& key, void* value,
                const Cache::CacheItemHelper* helper) override;

  std::unique_ptr<SecondaryCacheResultHandle> Lookup(
      const Slice& key, const Cache::CreateCallback& create_cb,
      bool /*wait*/) override;

  void Erase(const Slice& key) override { cache_->Erase(key); }

  void WaitAll(std::vector<SecondaryCacheResultHandle*> handles) override {
    for (SecondaryCacheResultHandle* handle : handles) {
      auto* sec_handle = static_cast<PMemSCacheResultHandle*>(handle);
      sec_handle->SetReady();
    }
  }

  std::string GetPrintableOptions() const override { return ""; }

 private:

  std::shared_ptr<Cache> cache_;
  const PMemAllocatorOptions option_;
  std::shared_ptr<MemoryAllocator> allocator_;
};

}  // namespace ROCKSDB_NAMESPACE
