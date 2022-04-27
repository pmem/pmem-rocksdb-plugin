/* SPDX-License-Identifier: BSD-3-Clause
* Copyright(c) 2021 Intel Corporation
*/
#pragma once
#include <memkind.h>
#include <snappy.h>

#include <condition_variable>
#include <cstring>
#include <limits>
#include <list>
#include <mutex>
#include <string>

#include "rocksdb/memory_allocator.h"
#include "rocksdb/secondary_cache.h"
#include "rocksdb/system_clock.h"
#include "util/hash.h"
#include "util/mutexlock.h"
#include "utilities/persistent_cache/hash_table_evictable.h"
#include "utilities/persistent_cache/persistent_cache_util.h"

#ifndef PLATFORM_IS_LITTLE_ENDIAN
#define PLATFORM_IS_LITTLE_ENDIAN (__BYTE_ORDER == __LITTLE_ENDIAN)
#endif
namespace ROCKSDB_NAMESPACE {

constexpr bool kLittleEndian = PLATFORM_IS_LITTLE_ENDIAN;

static void EncodeFixed64(char* buf, uint64_t value) {
  if (kLittleEndian) {
    memcpy(buf, &value, sizeof(value));
  } else {
    buf[0] = value & 0xff;
    buf[1] = (value >> 8) & 0xff;
    buf[2] = (value >> 16) & 0xff;
    buf[3] = (value >> 24) & 0xff;
    buf[4] = (value >> 32) & 0xff;
    buf[5] = (value >> 40) & 0xff;
    buf[6] = (value >> 48) & 0xff;
    buf[7] = (value >> 56) & 0xff;
  }
}

static uint32_t DecodeFixed32(const char* ptr) {
  if (kLittleEndian) {
    // Load the raw bytes
    uint32_t result;
    memcpy(&result, ptr, sizeof(result));  // gcc optimizes this to a plain load
    return result;
  } else {
    return ((static_cast<uint32_t>(static_cast<unsigned char>(ptr[0]))) |
            (static_cast<uint32_t>(static_cast<unsigned char>(ptr[1])) << 8) |
            (static_cast<uint32_t>(static_cast<unsigned char>(ptr[2])) << 16) |
            (static_cast<uint32_t>(static_cast<unsigned char>(ptr[3])) << 24));
  }
}

static uint64_t DecodeFixed64(const char* ptr) {
  if (kLittleEndian) {
    // Load the raw bytes
    uint64_t result;
    memcpy(&result, ptr, sizeof(result));  // gcc optimizes this to a plain load
    return result;
  } else {
    uint64_t lo = DecodeFixed32(ptr);
    uint64_t hi = DecodeFixed32(ptr + 4);
    return (hi << 32) | lo;
  }
}

class ECache {
  struct PMemValue {
    explicit PMemValue(char* str, const int vsize) {
      valueptr = str;
      size = vsize;
    }
    char* valueptr;
    size_t size;
  };

  struct PMemCacheData : LRUElement<PMemCacheData> {
    explicit PMemCacheData(
        const std::string& _key, const PMemValue& _value = PMemValue(NULL, 0),
        const std::shared_ptr<MemoryAllocator>& allocator = nullptr)
        : LRUElement<PMemCacheData>(),
          key(_key),
          value(_value),
          allocator_ptr(allocator) {}

    virtual ~PMemCacheData() {}

    const std::string key;
    const PMemValue value;
    std::shared_ptr<MemoryAllocator> allocator_ptr;
  };

  struct PMemCacheDataHash {
    uint64_t operator()(const PMemCacheData* obj) const {
      assert(obj);
      return std::hash<std::string>()(obj->key);
    }
  };

  struct PMemCacheDataEqual {
    bool operator()(const PMemCacheData* lhs, const PMemCacheData* rhs) const {
      assert(lhs);
      assert(rhs);
      return lhs->key == rhs->key;
    }
  };

  static void DeleteCacheData(PMemCacheData* data) {
    assert(data);
    if (data->allocator_ptr && data->value.valueptr) {
      data->allocator_ptr->Deallocate(data->value.valueptr);
    }
    delete data;
  }

  typedef EvictableHashTable<PMemCacheData, PMemCacheDataHash,
                             PMemCacheDataEqual>
      IndexType;

  // Max attempts to insert key, value to cache in pipelined mode

  static inline uint32_t HashSlice(const Slice& s) {
    return Hash(s.data(), s.size(), 0);
  }

  port::RWMutex indexlock_;            // Synchronization
  IndexType* index_buckets_;           // in-memory cache
  std::atomic<uint64_t> max_size_{0};  // Maximum size of the cache
  std::atomic<uint64_t> size_{0};      // Size of the cache
  uint32_t bucket_num_ = 128;
  std::shared_ptr<MemoryAllocator> allocator_ptr_;
  std::thread evict_thread_;
  std::atomic<bool> stop_flag_{false};

  static void EvictMain(ECache* e_cache) {
    std::random_device rd;
    std::mt19937 mt(rd());
    std::uniform_int_distribution<size_t> dist(0, e_cache->bucket_num_ - 1);

    while (true) {
      if (e_cache->stop_flag_.load()) {
        break;
      }
      uint64_t now_size = e_cache->size_.load();
      if (now_size < e_cache->max_size_ * 0.8) {
        SystemClock::Default()->SleepForMicroseconds(10000);
        continue;
      }
      size_t eid = dist(mt);
      PMemCacheData* edata = e_cache->index_buckets_[eid].Evict(
          [e_cache](PMemCacheData* data) -> void {
            if (data->allocator_ptr && data->value.valueptr) {
              data->allocator_ptr->Deallocate(data->value.valueptr);
              e_cache->size_ -= data->value.size;
            }
          });
      delete edata;
    }
  }

 public:
  ECache(size_t max_size, std::shared_ptr<MemoryAllocator> allocator)
      : max_size_(max_size),
        allocator_ptr_(std::move(allocator)),
        evict_thread_(EvictMain, this) {
    index_buckets_ = new IndexType[bucket_num_];
  }

  ~ECache() {
    stop_flag_.store(true);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    for (uint32_t i = 0; i < bucket_num_; i++) {
      index_buckets_[i].Clear(&DeleteCacheData);
    }
    delete[] index_buckets_;
  }

  Status InsertImpl(const Slice& page_key, const Slice& data) {
    uint32_t index = HashSlice(page_key) % bucket_num_;
    PMemCacheData key(page_key.ToString());
    PMemCacheData* kv;

    if (index_buckets_[index].Find(&key, &kv)) {
      // as there is buffer for async insert, page is possibly inserted before
      // nothing to do
      kv->refs_--;
      return Status::Aborted("key already exists in cache");
    }
    uint64_t size = data.size();

    char* valueptr = (char*)allocator_ptr_->Allocate(size);
    if (valueptr == nullptr) {
      return Status::TryAgain("Unable to allocate");
    }
    // increment the size
    size_ += size;
    pmem_memcpy(valueptr, data.data(), size, PMEM_F_MEM_NONTEMPORAL);
    PMemValue value(valueptr, size);
    std::unique_ptr<PMemCacheData> cache_data(new PMemCacheData(
        std::move(page_key.ToString()), value, allocator_ptr_));

    bool ok = index_buckets_[index].Insert(cache_data.get());
    if (!ok) {
      // decrement the size that we incremented ahead of time
      assert(size_ >= size);
      size_ -= size;
      // failed to insert to cache, block already in cache
      return Status::Aborted("key already exists in volatile cache");
    }

    cache_data.release();

    return Status::OK();
  }

  Status Lookup(const Slice& page_key, void** value, size_t* charge,
                const Cache::CreateCallback& create_cb) {
    uint32_t index = HashSlice(page_key) % bucket_num_;
    PMemCacheData key(std::move(page_key.ToString()));
    PMemCacheData* kv;
    bool ok = index_buckets_[index].Find(&key, &kv);

    if (ok) {
      // set return data
      char* mptr = kv->value.valueptr;
      size_t size = DecodeFixed64(mptr);
      mptr += sizeof(uint64_t);
      Status s = create_cb((void*)mptr, size, value, charge);
      // drop the reference on cache data
      kv->refs_--;
      return Status::OK();
    }

    return Status::NotFound("key not found in volatile cache");
  }
};

class KMemAllocator : public MemoryAllocator {
 public:
  const char* Name() const override { return "KMemAllocator"; };
  void* Allocate(size_t size) override {
    void* p = memkind_malloc(MEMKIND_DAX_KMEM, size);
    if (!p) {
      throw std::bad_alloc();
    }
    return p;
  }
  void Deallocate(void* p) override { memkind_free(MEMKIND_DAX_KMEM, p); }
};

struct CacheEntry {
  void* data;
  std::shared_ptr<MemoryAllocator> allocator;
  CacheEntry(const std::shared_ptr<MemoryAllocator>& a) : allocator(a) {}
  ~CacheEntry() { allocator->Deallocate(data); }
};

class FSDaxAllocator : public MemoryAllocator {
 private:
  memkind* pmem_;

 public:
  std::atomic_size_t usage_;
  size_t max_size_;
  FSDaxAllocator(const std::string& pmem_path, const size_t max_size)
      : usage_(0), max_size_(max_size) {
    int ret = memkind_create_pmem(pmem_path.c_str(), max_size * 1.2, &pmem_);
    if (ret != MEMKIND_SUCCESS) {
      fprintf(stderr, "failed to create pmem for fs dax allocator");
      assert(ret == MEMKIND_SUCCESS);
    }
  }
  const char* Name() const override { return "FSDaxAlloctor"; };
  void* Allocate(size_t size) override { return memkind_malloc(pmem_, size); }
  void Deallocate(void* p) override { memkind_free(pmem_, p); }
  ~FSDaxAllocator() {
    memkind_destroy_kind(pmem_);
  }
};

struct PMemAllocatorOptions {
  bool is_kmem_dax;
  std::string pmem_path;
  size_t max_size;
  PMemAllocatorOptions() : is_kmem_dax(true) {}
  PMemAllocatorOptions(const std::string& path, size_t msize)
      : is_kmem_dax(false), pmem_path(path), max_size(msize) {}
};

/*
static void snappy_compression(const char* input, size_t length, std::string*
output) { output->resize(snappy::MaxCompressedLength(length)); size_t outlen;
  snappy::RawCompress(input, length, &(*output)[0], &outlen);
  output->resize(outlen);
}*/

class PMemSCacheResultHandle : public SecondaryCacheResultHandle {
 public:
  PMemSCacheResultHandle(void* value, size_t size)
      : value_(value), size_(size), is_ready_(true) {}

  ~PMemSCacheResultHandle() override {}

  bool IsReady() override { return is_ready_; }

  void Wait() override {
    while (!is_ready_ptr_->load()) {
      std::this_thread::yield();
    }
  }

  void* Value() override {
    assert(is_ready_);
    return value_;
  }

  size_t Size() override { return Value() ? size_ : 0; }

  void SetReady() { is_ready_ = true; }

 private:
  void* value_;
  size_t size_;
  bool is_ready_;
  std::shared_ptr<std::atomic_bool> is_ready_ptr_;
};

struct InsertOp {
  explicit InsertOp(const bool signal) : data_(nullptr), signal_(signal) {}
  explicit InsertOp(const Slice& key, char* data, size_t raw_data_size,
                    const Cache::CacheItemHelper* helper = nullptr)
      : key_(key),
        data_(data),
        data_size_(raw_data_size),
        helper_(helper),
        signal_(false) {}
  ~InsertOp() = default;
  InsertOp() = delete;

  // used for estimating size by bounded queue
  size_t Size() const { return data_size_ + key_.size(); }

  Slice key_;
  std::shared_ptr<char> data_;
  size_t data_size_;
  const Cache::CacheItemHelper* helper_;
  const bool signal_;
};

}  // namespace ROCKSDB_NAMESPACE
