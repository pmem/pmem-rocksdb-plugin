/* SPDX-License-Identifier: BSD-3-Clause
* Copyright(c) 2021 Intel Corporation
*/
#include "pmem_scache.h"

#include <climits>

#include "rocksdb/utilities/object_registry.h"
#include "util/compression.h"
namespace ROCKSDB_NAMESPACE {
PMemSecondaryCache::PMemSecondaryCache(const PMemAllocatorOptions& opt)
    : option_(opt) {
  if (option_.is_kmem_dax) {
    allocator_ = std::make_shared<KMemAllocator>();
  } else {
    allocator_ = std::make_shared<FSDaxAllocator>(opt.pmem_path, opt.max_size);
  }
  cache_ = NewLRUCache(opt.max_size * 0.85, 6, true, 0.0, allocator_,
                       kDefaultToAdaptiveMutex, kDontChargeCacheMetadata);
}

Status PMemSecondaryCache::Insert(const Slice& key, void* value, const Cache::CacheItemHelper* helper) {
  size_t size = (*helper->size_cb)(value);
  std::unique_ptr<char> buf_ptr(new char[size + sizeof(uint64_t)]);
  char* buf = buf_ptr.get();
  EncodeFixed64(buf, size);
  Status s = (*helper->saveto_cb)(value, 0, size, buf + sizeof(uint64_t));
  if(!s.ok()) {
    fprintf(stderr, "helper_cb failed to extract value\n");
    return s;
  }

  CompressionOptions compression_opts;
  CompressionContext compression_context(kSnappyCompression);
  uint64_t sample_for_compression = 0;
  CompressionInfo compression_info(
      compression_opts, compression_context, CompressionDict::GetEmptyDict(),
      kSnappyCompression, sample_for_compression);
  Slice vslice(buf, size + sizeof(uint64_t));

  std::string output;
  bool success = CompressData(vslice, compression_info, -1, &output);
  if(!success) {
    fprintf(stderr, "failed to compress\n");
    return Status::IOError("failed to compress");
  }

  auto* entry = new CacheEntry(allocator_);
  entry->data = (char*)allocator_->Allocate(output.size());
  entry->size = output.size();
  if(entry->data == nullptr) {
    delete entry;
    // still return OK
    return Status::OK();
  }
  pmem_memcpy(entry->data, output.c_str(), output.size(), PMEM_F_MEM_NONTEMPORAL);

  s = cache_->Insert(key, entry, output.size(),
                                     [](const Slice& /*key*/, void* val) -> void {
                                       delete static_cast<CacheEntry*>(val);
                                     });
  if(!s.ok()) {
    fprintf(stderr, "internal lru cache failed to insert entry\n");
  }
  return Status::OK();
}

std::unique_ptr<SecondaryCacheResultHandle> PMemSecondaryCache::Lookup(
    const Slice& key, const Cache::CreateCallback& create_cb,
    bool /*wait*/) {
  std::string key_str = key.ToString();

  std::unique_ptr<SecondaryCacheResultHandle> secondary_handle;

  Cache::Handle* handle = cache_->Lookup(key);
  if (handle) {
    void* value = nullptr;
    size_t charge = 0;
    Status s;
    auto* cvalue = (CacheEntry*)cache_->Value(handle);
    size_t csize = cvalue->size;
    const char* cdata = (const char*)cvalue->data;

    UncompressionContext uncompressionContext(kSnappyCompression);
    UncompressionInfo uncompressionInfo(uncompressionContext, UncompressionDict::GetEmptyDict(), kSnappyCompression);
    size_t uncompress_size;
    std::string output;
    CacheAllocationPtr uncompress_ptr = UncompressData(uncompressionInfo, cdata, csize, &uncompress_size, -1);
    char* uncompress_raw = uncompress_ptr.get();

    if(!uncompress_ptr) {
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

  return secondary_handle;
}

extern "C" FactoryFunc<SecondaryCache> pmem_scache_reg;

FactoryFunc<SecondaryCache> pmem_scache_reg =
    ObjectLibrary::Default()->Register<SecondaryCache>(
        "pmem_scache://.*",
        [](const std::string& uri, std::unique_ptr<SecondaryCache>* f,
           std::string* errmsg) -> SecondaryCache* {
          std::string dev_id = uri;
          dev_id.replace(0, strlen("pmem_scache://"), "");
          PMemAllocatorOptions pmem_option;
          bool is_kmemdax = false;
          if (dev_id.find("kmemdax:") != std::string::npos) {
            is_kmemdax = true;
          } else if (dev_id.find("fsdax:") != std::string::npos) {
            is_kmemdax = false;
          } else {
            *errmsg = "Malformed Memkind, should be kmemdax or fsdax";
            fprintf(stderr, "error: %s \n", errmsg->c_str());
            f->reset(nullptr);
            return nullptr;
          }
          if (is_kmemdax) {
            dev_id.replace(0, strlen("kmemdax:"), "");
          } else {
            dev_id.replace(0, strlen("fsdax:"), "");
          }
          std::vector<std::string> raw_param;
          size_t sep_pos = dev_id.find('&');
          while (sep_pos != std::string::npos) {
            auto each_param = dev_id.substr(0, sep_pos);
            raw_param.emplace_back(each_param);
            dev_id.replace(0, each_param.size() + 1, "");
            sep_pos = dev_id.find('&');
          }
          if (!dev_id.empty()) {
            raw_param.emplace_back(dev_id);
          }
          if (raw_param.empty()) {
            *errmsg = "Malformed Param, no path or capacity";
            fprintf(stderr, "error: %s \n", errmsg->c_str());
            f->reset(nullptr);
            return nullptr;
          }
          bool is_malformed = false;
          std::string path;
          size_t cap = 0;
          for (auto& each_param : raw_param) {
            size_t e_pos = each_param.find('=');
            if (e_pos == std::string::npos) {
              *errmsg = "Malformed Param, invalid param";
              is_malformed = true;
              break;
            }
            std::string key = each_param.substr(0, e_pos);
            std::string value = each_param.substr(e_pos + 1);
            if (key == "path") {
              if (is_kmemdax) {
                *errmsg = "Malformed Param, kmemdax has no path";
                is_malformed = true;
                break;
              }
              if (!path.empty()) {
                *errmsg = "Malformed Param, duplicate path";
                is_malformed = true;
                break;
              }
              path = value;
            } else if (key == "capacity") {
              if (cap != 0) {
                *errmsg = "Malformed Param, duplicate capacity";
                is_malformed = true;
                break;
              }
              cap = strtoul(value.c_str(), nullptr, 10);
              if (cap == ULONG_MAX && errno == ERANGE) {
                *errmsg = "Malformed Param, invalid capacity";
                is_malformed = true;
                break;
              }
            } else {
              *errmsg = "Malformed Param: " + key;
              is_malformed = true;
              break;
            }
          }
          if (is_malformed) {
            fprintf(stderr, "error: %s \n", errmsg->c_str());
            f->reset(nullptr);
            return nullptr;
          }
          pmem_option.is_kmem_dax = is_kmemdax;
          pmem_option.max_size = cap;
          if (!is_kmemdax) {
            pmem_option.pmem_path = path;
            int r = memkind_check_dax_path(path.c_str());
            if (r != MEMKIND_SUCCESS) {
              *errmsg = "Error PMem Path " + path;
              fprintf(stderr, "error: %s \n", errmsg->c_str());
              f->reset(nullptr);
              return nullptr;
            }
          }
          f->reset(new PMemSecondaryCache(pmem_option));
          return f->get();
        });

}  // namespace ROCKSDB_NAMESPACE
