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
$ db_bench --benchmarks=readrandom --readonly --use_existing_db=true --open_files=10000 --num=50000000 --reads=10000 --cache_size=8388608 --threads=32 --db=/tmp/cache_bench/ --use_direct_reads=true --secondary_cache_uri=pmem_scache://fsdax:capacity=8589934592&path=/mnt/pmem0/p_cache

# if your PMem mode is KMemDAX
$ db_bench --benchmarks=readrandom --readonly --use_existing_db=true --open_files=10000 --num=50000000 --reads=10000 --cache_size=8388608 --threads=32 --db=/tmp/cache_bench/ --use_direct_reads=true --secondary_cache_uri=pmem_scache://fsdax:capacity=8589934592&path=/mnt/pmem0/p_cache
```