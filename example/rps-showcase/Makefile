CC = gcc
CWIST_ROOT = ../..
LIBTTAK_DIR = $(CWIST_ROOT)/lib/libttak
CWIST_LIB = $(CWIST_ROOT)/libcwist.a

CFLAGS = -Wall -Wextra -O2 -I$(CWIST_ROOT)/include -I$(LIBTTAK_DIR)/include
LDFLAGS = -L$(CWIST_ROOT) -L$(CWIST_ROOT)/lib -L$(LIBTTAK_DIR)/lib
LIBS = -lcwist -lttak -lcjson -lsqlite3 -luriparser -lssl -lcrypto -lpthread

SRCS = main.c
TARGET = rps_showcase

all: $(TARGET)

$(TARGET): $(SRCS) $(CWIST_LIB)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS) $(LIBS)

$(CWIST_LIB):
	$(MAKE) -C $(CWIST_ROOT)

clean:
	rm -f $(TARGET)
