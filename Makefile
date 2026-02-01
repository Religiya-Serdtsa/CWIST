# Compiler and Flags
CC = gcc
CFLAGS = -I./include -I./lib -I./lib/libttak/include -I./lib/cjson -I./lib/sqlite3 -Wall -Wextra -pthread -g -D_GNU_SOURCE -O3 -DSQLITE_ENABLE_DESERIALIZE
LIBS = -L./lib/libttak/lib -pthread -lcjson -lssl -lcrypto -luriparser -ldl -lttak

# SQLite Automation
SQLITE_YEAR = 2024
SQLITE_VER = 3450100
SQLITE_ZIP = sqlite-amalgamation-$(SQLITE_VER).zip
SQLITE_URL = https://www.sqlite.org/$(SQLITE_YEAR)/$(SQLITE_ZIP)
SQLITE_DIR = lib/sqlite3

# Detect OS
UNAME_S := $(shell uname -s)
IO_SRC = src/sys/io/io_select.c # Default fallback

ifeq ($(UNAME_S),Linux)
    CFLAGS += -DCWIST_OS_LINUX
    # Check for io_uring headers? For now assume available or user manages env.
    IO_SRC = src/sys/io/io_uring.c
endif
ifeq ($(UNAME_S),Darwin)
    CFLAGS += -DCWIST_OS_BSD
    IO_SRC = src/sys/io/kqueue.c
endif
ifeq ($(UNAME_S),FreeBSD)
    CFLAGS += -DCWIST_OS_BSD
    IO_SRC = src/sys/io/kqueue.c
endif

# Source Files
SRCS = src/core/sstring/sstring.c \
       src/sys/err/error.c \
       src/net/http/http.c \
       src/net/http/https.c \
       src/net/http/mux.c \
       src/net/http/query.c \
       src/sys/session/session_manager.c \
       src/core/siphash/siphash.c \
       src/core/db/db.c \
       src/core/db/nuke_db.c \
       src/sys/app/app.c \
       src/net/websocket/websocket.c \
       src/net/websocket/ws_utils.c \
       src/core/utils/json_builder.c \
       src/sys/app/middleware.c \
       src/core/template/template.c \
       src/core/html/builder.c \
       src/sys/app/big_dumb_reply.c \
       src/sys/sys_info.c \
       src/core/mem/alloc.c \
       lib/sqlite3/sqlite3.c \
       $(IO_SRC)

# Object Files and Target
OBJS = $(SRCS:.c=.o)
LIB_NAME = libcwist.a
LIBTTAK_DIR = lib/libttak
LIBTTAK_LIB = $(LIBTTAK_DIR)/lib/libttak.a

# Installation Paths
PREFIX ?= /usr/local
LIBDIR = $(PREFIX)/lib
INCLUDEDIR = $(PREFIX)/include

# --- Build Targets ---

all: $(LIBTTAK_LIB) $(SQLITE_DIR)/sqlite3.c $(LIB_NAME)

# SQLite Download & Extraction Rule
$(SQLITE_DIR)/sqlite3.c:
	@echo "Downloading SQLite..."
	@mkdir -p $(SQLITE_DIR)
	@wget -q $(SQLITE_URL) -O $(SQLITE_DIR)/$(SQLITE_ZIP)
	@echo "Extracting SQLite..."
	@unzip -q -j $(SQLITE_DIR)/$(SQLITE_ZIP) -d $(SQLITE_DIR)
	@rm $(SQLITE_DIR)/$(SQLITE_ZIP)
	@echo "SQLite Ready."

$(LIB_NAME): $(OBJS)
	@echo "Creating static library..."
	ar rcs $@ $^

$(LIBTTAK_LIB):
	@echo "Building libttak..."
	$(MAKE) -C $(LIBTTAK_DIR)

# --- Test Targets ---

test: $(LIB_NAME) tests/test_sstring.c
	$(CC) $(CFLAGS) -o test_sstring tests/test_sstring.c $(LIB_NAME) $(LIBS)
	./test_sstring

# ... (other tests omitted for brevity, keeping standard ones)

install: $(LIB_NAME)
	@echo "Installing library to $(LIBDIR)..."
	install -d $(LIBDIR)
	install -m 644 $(LIB_NAME) $(LIBDIR)

	@echo "Installing headers to $(INCLUDEDIR)/cwist..."
	install -d $(INCLUDEDIR)/cwist
	
	# Copy all headers including vendor (sqlite3)
	cp -r include/cwist/* $(INCLUDEDIR)/cwist/

	# Set correct permissions
	find $(INCLUDEDIR)/cwist -type d -exec chmod 755 {} \;
	find $(INCLUDEDIR)/cwist -type f -exec chmod 644 {} \;
	@echo "Installation complete."

uninstall:
	@echo "Uninstalling cwist..."
	rm -f $(LIBDIR)/$(LIB_NAME)
	rm -rf $(INCLUDEDIR)/cwist
	@echo "Uninstallation complete."

clean:
	@echo "Cleaning up build artifacts..."
	rm -f $(OBJS) $(LIB_NAME)
	rm -rf include/cwist/vendor
	rm -f test_sstring test_http test_siphash test_mux stress_test test_cors test_websocket
	@$(MAKE) -C $(LIBTTAK_DIR) clean

rebuild: clean all
