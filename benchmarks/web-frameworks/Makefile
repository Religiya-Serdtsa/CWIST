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

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c $(LDFLAGS) $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
