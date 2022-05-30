/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */
#include "pmem_scache.h"

#include "rocksdb/utilities/object_registry.h"
#include "rocksdb/utilities/options_type.h"
#include "util/compression.h"

namespace ROCKSDB_NAMESPACE {

static std::unordered_map<std::string, OptionTypeInfo> scache_type_info = {
    {"path",
     {offsetof(struct PMemSecondaryCacheOptions, path), OptionType::kString,
      OptionVerificationType::kNormal, OptionTypeFlags::kNone}},
    {"capacity",
     {offsetof(struct PMemSecondaryCacheOptions, capacity), OptionType::kSizeT,
      OptionVerificationType::kNormal, OptionTypeFlags::kNone}},
      {"bucket_power", 
      {offsetof(struct PMemSecondaryCacheOptions, bucket_power), OptionType::kUInt32T,
      OptionVerificationType::kNormal, OptionTypeFlags::kNone}},
      {"locks_power", 
      {offsetof(struct PMemSecondaryCacheOptions, locks_power), OptionType::kUInt32T,
      OptionVerificationType::kNormal, OptionTypeFlags::kNone}},
};
//using MemoryTierCacheConfig = typename facebook::cachelib::MemoryTierCacheConfig;
using LruAllocatorConfig = facebook::cachelib::CacheAllocatorConfig<facebook::cachelib::LruAllocator>;
using CacheKey = typename CacheLibAllocator::Key;
void PMemSecondaryCache::initialize_cache(const PMemSecondaryCacheOptions& option) {
      LruAllocatorConfig cfg;
      //.configureMemoryTiers(config_tier).validate(); // will throw if bad config
      cfg.setCacheSize(option.capacity) 
              .setCacheName("MultiTierCache")
              .enableCachePersistence(option.path)
              .setAccessConfig(
                      {option.bucket_power, option.locks_power}) // assuming caching 20 million items
              .configureMemoryTiers({
                    facebook::cachelib::MemoryTierCacheConfig::fromShm().setRatio(1),
                    facebook::cachelib::MemoryTierCacheConfig::fromFile(option.path + "/file1").setRatio(1)})
              .validate(); // will throw if bad config
      cache_lib_ = std::make_unique<CacheLibAllocator>(CacheLibAllocator::SharedMemNew, cfg);
      default_pool_ = cache_lib_->addPool("default", cache_lib_->getCacheMemoryStats().cacheSize);
}

PMemSecondaryCache::PMemSecondaryCache(const PMemSecondaryCacheOptions& opt)
    : opt_(opt) {
  RegisterOptions(&opt_, &scache_type_info);
}

Status PMemSecondaryCache::Insert(const Slice& key, void* value,
                                  const Cache::CacheItemHelper* helper) {
  size_t size = (*helper->size_cb)(value);
  std::unique_ptr<char> buf_ptr(new char[size + sizeof(uint64_t)]);
  char* buf = buf_ptr.get();
  EncodeFixed64(buf, size);
  Status s = (*helper->saveto_cb)(value, 0, size, buf + sizeof(uint64_t));
  if (!s.ok()) {
    fprintf(stderr, "helper_cb failed to extract value\n");
    return s;
  }

  CompressionOptions compression_opts;
  CompressionContext compression_context(kSnappyCompression);
  uint64_t sample_for_compression = 0;
  CompressionInfo compression_info(compression_opts, compression_context,
                                   CompressionDict::GetEmptyDict(),
                                   kSnappyCompression, sample_for_compression);
  Slice vslice(buf, size + sizeof(uint64_t));

  std::string output;
  bool success = CompressData(vslice, compression_info, -1, &output);
  if (!success) {
    fprintf(stderr, "failed to compress\n");
    return Status::IOError("failed to compress");
  }

  auto alloc_handle = cache_lib_->allocate(default_pool_, key.data(), output.size());
  if (!alloc_handle) {
    return Status::IOError("cache may fail to evict due to too many pending writes"); // cache may fail to evict due to too many pending writes
  }
  std::memcpy(alloc_handle->getMemory(), output.c_str(), output.size());
  cache_lib_->insertOrReplace(alloc_handle);
  /*
auto* entry = new CacheEntry(allocator_);
  entry->data = (char*)allocator_->Allocate(output.size());
  entry->size = output.size();
  if (entry->data == nullptr) {
    delete entry;
    // still return OK
    return Status::OK();
  }
  pmem_memcpy(entry->data, output.c_str(), output.size(),
              PMEM_F_MEM_NONTEMPORAL);

  s = cache_->Insert(key, entry, output.size(),
                     [](const Slice& key, void* val) -> void {
                       delete static_cast<CacheEntry*>(val);
                     });
  if (!s.ok()) {
    fprintf(stderr, "internal lru cache failed to insert entry\n");
  }
   */
  return Status::OK();
}

std::unique_ptr<SecondaryCacheResultHandle> PMemSecondaryCache::Lookup(
    const Slice& key, const Cache::CreateCallback& create_cb, bool /*wait*/) {
  std::unique_ptr<SecondaryCacheResultHandle> secondary_handle;
  CacheLibAllocator::Key cacheKey(key.data(), key.size());
  CacheLibAllocator::ReadHandle handle = cache_lib_->find(cacheKey);
  auto* item = handle.get();
  if(item) {
    void* value = nullptr;
    size_t charge = 0;
    Status s;
    size_t csize = item->getSize();
    const char* cdata = (const char*)item->getMemory();
    UncompressionContext uncompressionContext(kSnappyCompression);
    UncompressionInfo uncompressionInfo(uncompressionContext,
                                        UncompressionDict::GetEmptyDict(),
                                        kSnappyCompression);
    size_t uncompress_size;
    std::string output;
    CacheAllocationPtr uncompress_ptr =
        UncompressData(uncompressionInfo, cdata, csize, &uncompress_size, -1);
    char* uncompress_raw = uncompress_ptr.get();

    if (!uncompress_ptr) {
      fprintf(stderr, "failed to decompress\n");
      // TODO release cachelib handle
      //cache_->Release(handle);
      return secondary_handle;
    }

    size_t size = DecodeFixed64(uncompress_raw);
    uncompress_raw += sizeof(uint64_t);
    s = create_cb(uncompress_raw, size, &value, &charge);
    if (s.ok()) {
      // TODO null handle ?
      secondary_handle.reset(
          new PMemSCacheResultHandle(/*cache_.get()*/nullptr, nullptr, value, charge));
    } else {
      // TODO relase Cachelib handler
      //cache_->Release(handle);
    }
  }
/*
  Cache::Handle* handle = cache_->Lookup(key);
  if (handle) {
    void* value = nullptr;
    size_t charge = 0;
    Status s;
    auto* cvalue = (CacheEntry*)cache_->Value(handle);
    size_t csize = cvalue->size;
    const char* cdata = (const char*)cvalue->data;

    UncompressionContext uncompressionContext(kSnappyCompression);
    UncompressionInfo uncompressionInfo(uncompressionContext,
                                        UncompressionDict::GetEmptyDict(),
                                        kSnappyCompression);
    size_t uncompress_size;
    std::string output;
    CacheAllocationPtr uncompress_ptr =
        UncompressData(uncompressionInfo, cdata, csize, &uncompress_size, -1);
    char* uncompress_raw = uncompress_ptr.get();

    if (!uncompress_ptr) {
      fprintf(stderr, "failed to decompress\n");
      cache_->Release(handle);
      return secondary_handle;
    }

    size_t size = DecodeFixed64(uncompress_raw);
    uncompress_raw += sizeof(uint64_t);
    s = create_cb(uncompress_raw, size, &value, &charge);
    if (s.ok()) {
      secondary_handle.reset(
          new PMemSCacheResultHandle(cache_.get(), handle, value, charge));
    } else {
      cache_->Release(handle);
    }
  }
  */

  return secondary_handle;
}

Status PMemSecondaryCache::PrepareOptions(const ConfigOptions& /*config_options*/) {
  /*
  if (opt_.capacity < (1L * 1024 * 1024 * 1024)) {
    return Status::InvalidArgument("capacity should be larger than 1GB");
  }
  if (opt_.is_kmem_dax && !opt_.path.empty()) {
    return Status::InvalidArgument("kmem dax mode has no path option");
  }
  if (!opt_.is_kmem_dax && opt_.path.empty()) {
    return Status::InvalidArgument("no path for fsdax mode");
  }
  if(memkind_check_dax_path(opt_.path.c_str()) != MEMKIND_SUCCESS) {
    return Status::InvalidArgument("invalid path for fsdax mode");
  }
  if (opt_.is_kmem_dax) {
    allocator_ = std::make_shared<KMemAllocator>();
  } else {
    allocator_ = std::make_shared<FSDaxAllocator>(opt_.path, opt_.capacity);
  }
  cache_ = NewLRUCache(opt_.capacity * opt_.ratio, 6, true, 0.0, allocator_,
                       kDefaultToAdaptiveMutex, kDontChargeCacheMetadata);
                       */
  // TODO cachelib
  initialize_cache(opt_);

  return Status::OK();
}

extern "C" FactoryFunc<SecondaryCache> pmem_scache_reg;
FactoryFunc<SecondaryCache> pmem_scache_reg =
    ObjectLibrary::Default()->AddFactory<SecondaryCache>(
        PMemSecondaryCache::kClassName(),
        [](const std::string& /*uri*/, std::unique_ptr<SecondaryCache>* f,
           std::string* /*errmsg*/) {
          PMemSecondaryCacheOptions options;
          auto* pmem_scache = new PMemSecondaryCache(options);
          f->reset(pmem_scache);
          return f->get();
        });

}  // namespace ROCKSDB_NAMESPACE
