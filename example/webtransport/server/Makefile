CC ?= gcc
ROOT = ../../..

CFLAGS = -Wall -Wextra -g \
         -I$(ROOT)/include \
         -I$(ROOT)/lib \
         -I$(ROOT)/lib/libttak/include \
         -I$(ROOT)/lib/cjson \
         -I$(ROOT)/lib/sqlite3 \
         -I$(ROOT)/lib/uriparser/include \
         -I$(ROOT)/lib/cnats/src \
         -I$(ROOT)/lib/boringssl/include \
         -I$(ROOT)/lib/lsquic/include \
         -I$(ROOT)/lib/multipart-parser-c \
         -D_GNU_SOURCE -D_XOPEN_SOURCE=700 -D_REENTRANT -DSQLITE_ENABLE_DESERIALIZE

LIBS = $(ROOT)/libcwist.a \
       $(ROOT)/lib/lsquic/build/src/liblsquic/liblsquic.a \
       $(ROOT)/lib/boringssl/build/libssl.a \
       $(ROOT)/lib/boringssl/build/libcrypto.a \
       -pthread -ldl -lm -lstdc++ -lz -lcurl -lnghttp2

TARGET = webtransport-server

all: $(TARGET)

$(TARGET): main.c $(ROOT)/libcwist.a
	$(CC) $(CFLAGS) -o $@ main.c $(LIBS)

clean:
	rm -f $(TARGET)
