/* SPDX-License-Identifier: BSD-3-Clause
* Copyright(c) 2021 Intel Corporation
*/
#include "pmem_scache.h"

#include <climits>
#include <memory>

#include "rocksdb/utilities/object_registry.h"
namespace ROCKSDB_NAMESPACE {
PMemSecondaryCache::PMemSecondaryCache(const PMemAllocatorOptions& opt)
    : option_(opt),
      insert_queue_(1LL * 1024 * 1024 * 1024),
      insert_thread_(InsertMain, this) {
  if (option_.is_kmem_dax) {
    allocator_ = std::make_shared<KMemAllocator>();
  } else {
    allocator_ = std::make_shared<FSDaxAllocator>(opt.pmem_path, opt.max_size);
  }
  ecache_ptr = std::make_shared<ECache>(opt.max_size, allocator_);
}

Status PMemSecondaryCache::Insert(const Slice& key, void* value,
                                  const Cache::CacheItemHelper* helper) {
  size_t size = (*helper->size_cb)(value);
  char* buf = new char[size + sizeof(uint64_t)];
  EncodeFixed64(buf, size);
  Status s = (*helper->saveto_cb)(value, 0, size, buf + sizeof(uint64_t));
  if (!s.ok()) {
    delete[] buf;
    fprintf(stderr, "helper_cb failed to extract value\n");
    return s;
  }
  insert_queue_.Push(InsertOp(key, buf, size + sizeof(uint64_t)));
  return Status::OK();
}

void PMemSecondaryCache::InsertMain(PMemSecondaryCache* s_cache) {
  while (true) {
    InsertOp op(s_cache->insert_queue_.Pop());
    if (op.signal_) {
      // that is a secret signal to exit
      break;
    }
    Slice value(op.data_.get(), op.data_size_);
    int retry = 0;
    while ((s_cache->ecache_ptr->InsertImpl(op.key_, value)).IsTryAgain()) {
      if (retry > kMaxRetry) {
        break;
      }
      retry++;
    }
  }
}
void PMemSecondaryCache::Close() {
  if (insert_thread_.joinable()) {
    insert_queue_.Push(InsertOp(true));
    insert_thread_.join();
  }
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
