CC = gcc
CFLAGS = -O2 -Wall -Wextra -I../../include
LDFLAGS = -L../../ \
          -L../../lib/boringssl/build \
          -L../../lib/lsquic/build/src/liblsquic \
          -L../../lib/libttak/lib \
          -L../../lib/cnats/build/lib \
          -L../../lib/cjson \
          -L../../lib/uriparser/build
LIBS = -lcwist -lnats_static -lttak -lcjson -luriparser -llsquic \
       -lssl -lcrypto \
       $(shell pkg-config --libs libcurl 2>/dev/null) \
       $(shell pkg-config --libs libnghttp2 2>/dev/null) \
       $(shell pkg-config --libs libbrotlienc libbrotlicommon libbrotlidec 2>/dev/null) \
       $(shell pkg-config --libs libzstd 2>/dev/null || echo -lzstd) \
       -lz -ldl -lpthread -lm -lstdc++

TARGET = cwist_bench

# USE_MIMALLOC=1: link mimalloc's static override library ahead of every
# other dependency so its malloc/free/calloc/realloc/posix_memalign symbols
# win process-wide - libttak, sqlite3 (bundled in libcwist.a), cJSON,
# lsquic, and boringssl all keep calling plain libc malloc and never notice
# the swap. See Makefile (top level) for how libmimalloc.a gets built.
#
# EXPERIMENTAL, measured WORSE for this app's default C1M/prefork mode: a
# same-binary/same-config GitHub Actions A/B (issue #25) showed mimalloc
# regressing RPS, P90/P99/P99.999 latency (P99.999 nearly doubled), and RSS
# (+120%) all at once - mimalloc's eager per-process arena reservation
# doesn't amortize across many short-lived, low-allocation prefork
# processes. Kept as an opt-in baseline for comparing other allocators/
# tunings against, not as a recommended flag.
MIMALLOC_LIB = ../../lib/mimalloc/build/libmimalloc.a
ifeq ($(USE_MIMALLOC),1)
TARGET := cwist_bench_mimalloc
# --whole-archive: with dynamic linking (the default here), `malloc` etc.
# can be satisfied at runtime by libc.so, so a plain `-l`/archive link never
# even looks inside libmimalloc.a for them - the linker only pulls archive
# members in to resolve symbols that are *not yet* resolvable, and a
# dynamically-resolvable libc symbol doesn't count. Forcing every object in
# the archive into the binary regardless makes mimalloc's malloc/free/etc.
# into local (T) symbols in the executable, which - per normal ELF symbol
# resolution - take precedence over the same names in a shared library.
LIBS := -Wl,--whole-archive $(MIMALLOC_LIB) -Wl,--no-whole-archive $(LIBS) -lrt -latomic
endif

all: $(TARGET)

$(TARGET): main.c
ifeq ($(USE_MIMALLOC),1)
	$(MAKE) -C ../.. lib/mimalloc/build/libmimalloc.a USE_MIMALLOC=1
endif
	$(CC) $(CFLAGS) -o $(TARGET) main.c $(LDFLAGS) $(LIBS)

clean:
	rm -f cwist_bench cwist_bench_mimalloc

.PHONY: all clean
