pmem_scache_SOURCES = scache/pmem_scache.cc
pmem_scache_HEADERS = scache/pmem_scache.h scache/pmem_scache_util.h
pmem_scache_LDFLAGS = -lpmem -u pmem_scache_reg