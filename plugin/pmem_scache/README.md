# PMem Secondary Cache
This plugin is a Persistent Memory based [Secondary Cache](http://rocksdb.org/blog/2021/05/27/rocksdb-secondary-cache.html).
### Dependencies
PMem Secondary Cache depends on [memkind](http://memkind.github.io/memkind/) and [libpmem](https://pmem.io/pmdk/libpmem/) of  [PMDK](https://github.com/pmem/pmdk) to utilize Persistent Memory. PMem Secondary Cache works with RocksDB version v6.19.3 or later, and support PMem FSDAX mode and PMem KMEMDAX mode.
## Getting started
### Build
Please make sure you have memkind and libpmem installed before you build PMem Secondary Cache.
Download RocksDB:
```
$ git clone https://github.com/facebook/rocksdb.git /home/user/rocksdb
$ cd /home/user/rocksdb
$ git checkout v6.29.3 # uses latest version of RocksDB
```
Download PMem RocksDB Plugin Collections:
```
$ git clone https://github.com/pmem/pmem-rocksdb-plugin /home/user/pmem-rocksdb-plugin
```
Copy PMem Secondary Cache plugin into RocksDB's plugin folder:
```
$ cp -r /home/user/pmem-rocksdb-plugin/plugin/pmem_scache/ /home/user/rocksdb/plugin
```
Build and install RocksDB with PMem Secondary Cache enabled:
```
$ DEBUG_LEVEL=0 ROCKSDB_PLUGINS=pmem_scache make -j48 db_bench install
```
### Example Usage
Run db_bench
```
# if your PMem mode is FSDAX and path is /mnt/pmem0
$ db_bench --benchmarks=readrandom --readonly --use_existing_db=true --open_files=10000 --num=50000000 --reads=10000 --cache_size=8388608 --threads=32 --db=/tmp/cache_bench/ --use_direct_reads=true --secondary_cache_uri="id=PMemSecondaryCache; path=/mnt/pmem0/p_cache; capacity=8589934592"

# if your PMem mode is KMemDAX
$ db_bench --benchmarks=readrandom --readonly --use_existing_db=true --open_files=10000 --num=50000000 --reads=10000 --cache_size=8388608 --threads=32 --db=/tmp/cache_bench/ --use_direct_reads=true --secondary_cache_uri="id=PMemSecondaryCache; is_kmem_dax=true; capacity=8589934592"
```
Run RocksDB Application
```
#include "rocksdb/db.h"
#include "rocksdb/cache.h"
#include "rocksdb/secondary_cache.h"
#include "rocksdb/convenience.h"

int main(int argc, char *argv[]) {
    rocksdb::DB *db;
    rocksdb::Options options;
    options.create_if_missing = true;

    // create secondary cache instance
    std::string secondary_cache_uri = "id=PMemSecondaryCache; path=/mnt/pmem0/p_cache; capacity=8589934592";
    std::shared_ptr <rocksdb::SecondaryCache> secondary_cache_ptr;
    rocksdb::Status status = rocksdb::SecondaryCache::CreateFromString(
            rocksdb::ConfigOptions(), secondary_cache_uri, &secondary_cache_ptr);
    if (!status.ok() || !secondary_cache_ptr) {
        fprintf(stderr, "No secondary cache registered matching string: %s status=%s\n", secondary_cache_uri.c_str(),
                status.ToString().c_str());
        return 0;
    }

    // put secondary cache into block cache
    rocksdb::LRUCacheOptions opts;
    opts.secondary_cache = secondary_cache_ptr;
    std::shared_ptr <rocksdb::Cache> cache_ptr = rocksdb::NewLRUCache(opts);
    rocksdb::BlockBasedTableOptions block_based_options;
    block_based_options.block_cache = cache_ptr;
    options.table_factory.reset(NewBlockBasedTableFactory(block_based_options));

    std::string rocksdb_path = "db_data";
    status = rocksdb::DB::Open(options, "db_data", &db);
    fprintf(stdout, "Open RocksDB at path=%s, s status=%s\n", rocksdb_path.c_str(), status.ToString().c_str());

    std::string test_key("key");
    std::string test_val("value");
    status = db->Put(rocksdb::WriteOptions(), test_key, test_val);
    if (status.ok()) {
        std::string saved_val;
        db->Get(rocksdb::ReadOptions(), test_key, &saved_val);
    } else {
        fprintf(stderr, "Failed to put key, status=%s\n", status.ToString().c_str());
    }

    db->Close();
    return 0;

```