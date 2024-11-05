/* SPDX-License-Identifier: BSD-3-Clause
* Copyright(c) 2021 Intel Corporation
*/
#pragma once

#include <libpmem.h>

#include <thread>

#include <cachelib/allocator/CacheAllocator.h>
#include <cachelib/allocator/CacheAllocatorConfig.h>
#include <cachelib/allocator/MemoryTierCacheConfig.h>
//#include <folly/init/Init.h>

#include "pmem_scache_util.h"

namespace ROCKSDB_NAMESPACE {

using CacheLibAllocator = facebook::cachelib::LruAllocator; // or Lru2QAllocator, or TinyLFUAllocator

struct PMemSecondaryCacheOptions {
  static const char* kName() { return "PMemSecondaryCacheOptions"; }
  std::string path;
  size_t capacity = 32L * 1024 * 1024 * 1024;

  unsigned int bucket_power = 25;

  unsigned int locks_power = 10;
};


class PMemSecondaryCache : public SecondaryCache {
 public:
  explicit PMemSecondaryCache(const PMemSecondaryCacheOptions& opt);
  ~PMemSecondaryCache() override {
    //cache_.reset();
  }

  Status PrepareOptions(const ConfigOptions& config_options) override;

  static const char* kClassName() { return "PMemSecondaryCache"; }
  const char* Name() const override { return kClassName(); }

  Status Insert(const Slice& key, void* value,
                const Cache::CacheItemHelper* helper) override;

  std::unique_ptr<SecondaryCacheResultHandle> Lookup(
      const Slice& key, const Cache::CreateCallback& create_cb,
      bool /*wait*/) override;

  void Erase(const Slice& key) override { 
    //cache_->Erase(key); 
    CacheLibAllocator::Key cacheKey(key.data(), key.size());
    cache_lib_->remove(cacheKey);
  }

  void WaitAll(std::vector<SecondaryCacheResultHandle*> handles) override {
    for (SecondaryCacheResultHandle* handle : handles) {
      auto* sec_handle = static_cast<PMemSCacheResultHandle*>(handle);
      sec_handle->SetReady();
    }
  }

  std::string GetPrintableOptions() const override { return ""; }

 private:

  void initialize_cache(const PMemSecondaryCacheOptions& option);

  //std::shared_ptr<Cache> cache_;
  PMemSecondaryCacheOptions opt_;
  //std::shared_ptr<MemoryAllocator> allocator_;

  std::unique_ptr<CacheLibAllocator> cache_lib_;
  facebook::cachelib::PoolId default_pool_;
};

}  // namespace ROCKSDB_NAMESPACE
