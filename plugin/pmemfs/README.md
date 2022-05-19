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
$ db_bench --fs_uri="id=PMemFS" --wal_dir=/mnt/pmem0/rocksdb_wal
```
Run RocksDB Application
```
#include "rocksdb/db.h"
#include "rocksdb/file_system.h"
#include "rocksdb/convenience.h"

int main(int argc, char *argv[]) {
    rocksdb::DB *db;
    rocksdb::Options options;
    options.create_if_missing = true;

    // create PMemFS instance, put into Env
    std::string fs_uri = "id=PMemFS";
    rocksdb::Env* env = rocksdb::Env::Default();
    std::shared_ptr<rocksdb::Env> env_guard;
    rocksdb::Status status = rocksdb::Env::CreateFromUri(rocksdb::ConfigOptions(), "", fs_uri,
                                  &env, &env_guard);
    if (!status.ok()) {
        fprintf(stderr, "Failed creating env: %s\n", status.ToString().c_str());
        return 0;
    }

    // put PMemFS plugin into options
    options.env = env;
    options.wal_dir = "/mnt/pmem0/rocksdb_wal";


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
}
```
