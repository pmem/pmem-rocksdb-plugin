/* SPDX-License-Identifier: BSD-3-Clause
* Copyright(c) 2021 Intel Corporation
*/
#pragma once
#include <memkind.h>
#include <snappy.h>

#include <cstring>
#include <string>
#include <list>
#include <limits>
#include <mutex>
#include <condition_variable>

#include "rocksdb/memory_allocator.h"
#include "rocksdb/secondary_cache.h"

namespace ROCKSDB_NAMESPACE {

class KMemAllocator : public MemoryAllocator {
 public:
  const char* Name() const override { return "KMemAllocator"; };
  void* Allocate(size_t size) override {
    void* p = memkind_malloc(MEMKIND_DAX_KMEM, size);
    if (p == NULL) {
      throw std::bad_alloc();
    }
    return p;
  }
  void Deallocate(void* p) override { memkind_free(MEMKIND_DAX_KMEM, p); }
  size_t UsableSize(void* p, size_t /*allocation_size*/) const override {
    return memkind_malloc_usable_size(MEMKIND_DAX_KMEM, p);
  }
};

struct CacheEntry {
  void* data;
  size_t size;
  std::shared_ptr<MemoryAllocator> allocator;
  CacheEntry(const std::shared_ptr<MemoryAllocator>& a) : allocator(a) {}
  ~CacheEntry() {
    allocator->Deallocate(data);
  }
};

class FSDaxAllocator : public MemoryAllocator {
 private:
  memkind* pmem_;

 public:
  FSDaxAllocator(const std::string& pmem_path, const size_t max_size) {
    int ret = memkind_create_pmem(pmem_path.c_str(), max_size, &pmem_);
    if (ret != MEMKIND_SUCCESS) {
      fprintf(stderr, "failed to create pmem for fs dax allocator");
      assert(ret == MEMKIND_SUCCESS);
    }
  }
  const char* Name() const override { return "FSDaxAlloctor"; };
  void* Allocate(size_t size) override {
    return memkind_malloc(pmem_, size);
  }
  void Deallocate(void* p) override {
    memkind_free(pmem_, p);
  }
  size_t UsableSize(void* p, size_t /*allocation_size*/) const override {
    return memkind_malloc_usable_size(MEMKIND_DAX_KMEM, p);
  }
};

class PMemSCacheResultHandle : public SecondaryCacheResultHandle {
 public:
  PMemSCacheResultHandle(Cache* cache, Cache::Handle* handle, void* value,
                         size_t size)
      : cache_(cache),
        handle_(handle),
        value_(value),
        size_(size),
        is_ready_(true) {}

  ~PMemSCacheResultHandle() override { cache_->Release(handle_); }

  bool IsReady() override { return is_ready_; }

  void Wait() override {
    while(!is_ready_ptr_->load()) {
      std::this_thread::yield();
    }
  }

  void* Value() override {
    assert(is_ready_);
    return value_;
  }

  size_t Size() override {
    return Value() ? size_ : 0;
  }

  void SetReady() { is_ready_ = true; }

 private:
  Cache* cache_;
  Cache::Handle* handle_;
  void* value_;
  size_t size_;
  bool is_ready_;
  std::shared_ptr<std::atomic_bool> is_ready_ptr_;
};

}  // namespace ROCKSDB_NAMESPACE
