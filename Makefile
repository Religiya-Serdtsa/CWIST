# Compiler and Flags
CC = gcc
# Added -g for debug info, -O2 for optimization (optional, adjust as needed)
CFLAGS = -I./include -I./lib -I./lib/cjson -Wall -Wextra -pthread -g
LIBS = -pthread -lcjson -lssl -lcrypto -luriparser -lsqlite3

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
       src/sys/app/app.c \
       src/net/websocket/websocket.c \
       src/net/websocket/ws_utils.c \
       src/core/utils/json_builder.c \
       src/sys/app/middleware.c \
       src/core/template/template.c \
       src/core/html/builder.c

# Object Files and Target
OBJS = $(SRCS:.c=.o)
LIB_NAME = libcwist.a

# Installation Paths
PREFIX ?= /usr/local
LIBDIR = $(PREFIX)/lib
INCLUDEDIR = $(PREFIX)/include

# --- Build Targets ---

all: $(LIB_NAME)

$(LIB_NAME): $(OBJS)
	@echo "Creating static library..."
	ar rcs $@ $^

# --- Test Targets ---

test: $(LIB_NAME) tests/test_sstring.c
	$(CC) $(CFLAGS) -o test_sstring tests/test_sstring.c $(LIB_NAME) $(LIBS)
	./test_sstring

test_websocket: $(LIB_NAME) tests/test_websocket.c
	$(CC) $(CFLAGS) -o test_websocket tests/test_websocket.c $(LIB_NAME) $(LIBS)
	./test_websocket

stress-test: $(LIB_NAME) tests/stress_test.c
	$(CC) $(CFLAGS) -o stress_test tests/stress_test.c $(LIB_NAME) $(LIBS)
	./stress_test

test_http: $(LIB_NAME) tests/test_http.c
	$(CC) $(CFLAGS) -o test_http tests/test_http.c $(LIB_NAME) $(LIBS)
	./test_http

test_siphash: $(LIB_NAME) tests/test_siphash.c
	$(CC) $(CFLAGS) -o test_siphash tests/test_siphash.c $(LIB_NAME) $(LIBS)
	./test_siphash

test_mux: $(LIB_NAME) tests/test_mux.c
	$(CC) $(CFLAGS) -o test_mux tests/test_mux.c $(LIB_NAME) $(LIBS)
	./test_mux

test_cors: $(LIB_NAME) tests/test_cors.c
	$(CC) $(CFLAGS) -o test_cors tests/test_cors.c $(LIB_NAME) $(LIBS)
	./test_cors

# Pattern rule for object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# --- Install / Uninstall ---

install: $(LIB_NAME)
	@echo "Installing library to $(LIBDIR)..."
	install -d $(LIBDIR)
	install -m 644 $(LIB_NAME) $(LIBDIR)

	@echo "Installing headers to $(INCLUDEDIR)/cwist..."
	install -d $(INCLUDEDIR)/cwist

	# Recursively copy headers to preserve the directory structure
	# (e.g., include/cwist/net/http/http.h -> /usr/local/include/cwist/net/http/http.h)
	cp -r include/cwist/* $(INCLUDEDIR)/cwist/

	# Set correct permissions for the copied files and directories
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
	rm -f test_sstring test_http test_siphash test_mux stress_test test_cors test_websocket
