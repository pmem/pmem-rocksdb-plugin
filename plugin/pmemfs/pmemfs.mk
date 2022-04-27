pmemfs_SOURCES = fs/fs_pmemfs.cc fs/io_pmemfs.cc fs/io_pmemfs.cc
pmemfs_HEADERS = fs/fs_pmemfs.h fs/zbd_pmemfs.h fs/io_pmemfs.h
pmemfs_LDFLAGS = -lpmem -u pmemfs_reg