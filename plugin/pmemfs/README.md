# PMem Filesystem For WAL
This plugin is a Filesystem API wrapper to write RocksDB's WAL files on Persistent Memory.
### Dependencies
PMem Filesystem For WAL depends on [libpmem](https://pmem.io/pmdk/libpmem/) of [PMDK](https://github.com/pmem/pmdk) to utilize Persistent Memory. PMem Filesystem For WAL works with RocksDB version v6.19.3 or later, and support PMem FSDAX mode only.
## Getting started
### Build
Please make sure you have libpmem installed before you build PMem Filesystem For WAL.
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
Copy PMem Filesystem For WAL plugin into RocksDB's plugin folder:
```
$ cp -r /home/user/pmem-rocksdb-plugin/plugin/pmemfs/ /home/user/rocksdb/plugin
```
Build and install RocksDB with PMem Filesystem For WAL enabled:
```
$ DEBUG_LEVEL=0 ROCKSDB_PLUGINS=pmemfs make -j48 db_bench install
```
### Example Usage
Run db_bench
```
# if your PMem mode is FSDAX and path is /mnt/pmem0
$ db_bench --fs_uri=pmemfs --wal_dir=/mnt/pmem0/rocksdb_wal
```