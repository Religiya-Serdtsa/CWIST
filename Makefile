# Compiler and Flags
CC ?= gcc

CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
NGHTTP2_CFLAGS := $(shell pkg-config --cflags libnghttp2 2>/dev/null)
BROTLI_CFLAGS := $(shell pkg-config --cflags libbrotlienc libbrotlicommon libbrotlidec 2>/dev/null)

INCLUDE_PATHS = -I./include -I./lib -I./lib/libttak/include -I./lib/cjson -I./lib/sqlite3 -I./lib/uriparser/include -I./lib/cnats/src -I./lib/boringssl/include -I./lib/lsquic/include -I./lib/multipart-parser-c $(CURL_CFLAGS) $(NGHTTP2_CFLAGS) $(BROTLI_CFLAGS)
COMMON_DEFINES = -D_GNU_SOURCE -D_XOPEN_SOURCE=700 -D_REENTRANT -DSQLITE_ENABLE_DESERIALIZE
COMMON_WARNINGS = -std=c2x -Wall -pthread -fPIC
COMMON_CFLAGS = $(INCLUDE_PATHS) $(COMMON_WARNINGS) $(COMMON_DEFINES)

BUILD_PROFILE = perf
ifneq (,$(findstring tcc,$(notdir $(CC))))
    BUILD_PROFILE = tcc
endif

TCC_STACK_FLAGS = -O3 -g \
                  -fno-inline \
                  -fno-omit-frame-pointer \
                  -fno-optimize-sibling-calls \
                  -fno-semantic-interposition \
                  -fno-trapping-math \
                  -falign-functions=32 \
                  -fno-plt \
                  -fno-math-errno

PERF_WARNINGS = -Wextra
PERF_STACK_FLAGS = -O3 -g

ifeq ($(BUILD_PROFILE),tcc)
    CFLAGS = $(COMMON_CFLAGS) $(TCC_STACK_FLAGS) -ftls-model=global-dynamic
else
    CFLAGS = $(COMMON_CFLAGS) $(PERF_WARNINGS) $(PERF_STACK_FLAGS)
endif

URIPARSER_DIR = lib/uriparser
URIPARSER_BUILD_DIR = $(URIPARSER_DIR)/build
URIPARSER_LIB = $(URIPARSER_BUILD_DIR)/liburiparser.a
URIPARSER_CMAKE_FLAGS = -DCMAKE_BUILD_TYPE=Release \
                        -DBUILD_SHARED_LIBS=OFF \
                        -DURIPARSER_SHARED_LIBS=OFF \
                        -DURIPARSER_BUILD_DOCS=OFF \
                        -DURIPARSER_BUILD_TESTS=OFF \
                        -DURIPARSER_BUILD_FUZZERS=OFF \
                        -DURIPARSER_BUILD_TOOLS=OFF

BORINGSSL_DIR = lib/boringssl
BORINGSSL_BUILD_DIR = $(BORINGSSL_DIR)/build
BORINGSSL_SSL_LIB = $(BORINGSSL_BUILD_DIR)/libssl.a
BORINGSSL_CRYPTO_LIB = $(BORINGSSL_BUILD_DIR)/libcrypto.a

LSQUIC_DIR = lib/lsquic
LSQUIC_BUILD_DIR = $(LSQUIC_DIR)/build
LSQUIC_LIB = $(LSQUIC_BUILD_DIR)/src/liblsquic/liblsquic.a

CURL_LIBS := $(shell pkg-config --libs libcurl 2>/dev/null)
NGHTTP2_LIBS := $(shell pkg-config --libs libnghttp2 2>/dev/null)
BROTLI_LIBS := $(shell pkg-config --libs libbrotlienc libbrotlicommon libbrotlidec 2>/dev/null)

LIBS = -pthread -ldl -lm -lstdc++ -lz -lzstd \
       $(LSQUIC_LIB) \
       $(BORINGSSL_SSL_LIB) \
       $(BORINGSSL_CRYPTO_LIB) \
       $(CURL_LIBS) \
       $(NGHTTP2_LIBS) \
       $(BROTLI_LIBS)

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
       src/core/seq/seq.c \
       src/sys/err/error.c \
       src/net/http/http.c \
       src/net/http/http2.c \
       src/net/http/http3.c \
       src/net/http/curl_global.c \
       src/net/http/http_client.c \
       src/net/http/http3_client.c \
       src/net/http/https.c \
       src/net/http/tls_chain.c \
       src/https/pqc_layer.c \
       src/net/http/mux.c \
       src/net/http/multipart.c \
       src/net/http/async_server.c \
       src/net/http/cookie.c \
       src/net/http/session.c \
       src/net/http/query.c \
       lib/multipart-parser-c/multipart_parser.c \
       src/sys/session/session_manager.c \
       src/sys/app/csrf.c \
       src/core/siphash/siphash.c \
       src/core/db/db.c \
       src/core/db/pool.c \
       src/core/db/nuke_db.c \
       src/core/db/migrate.c \
       src/core/orm/orm.c \
       src/core/orm/orm_socket.c \
       src/core/orm/rdbms_auto_mount.c \
       src/sys/app/app.c \
       src/net/websocket/websocket.c \
       src/net/websocket/ws_utils.c \
       src/core/utils/json_builder.c \
       src/core/utils/json_heal.c \
       src/core/utils/zod.c \
       src/sys/app/middleware.c \
       src/sys/app/config.c \
       src/sys/app/logger.c \
       src/sys/app/shutdown.c \
       src/sys/app/compress.c \
       src/sys/app/test_client.c \
       src/core/log/log.c \
       src/sys/session/flash.c \
       src/core/template/template.c \
       src/core/html/builder.c \
       src/core/html/css_composer.c \
       src/sys/app/big_dumb_reply.c \
       src/sys/sys_info.c \
       src/core/mem/alloc.c \
       src/core/mem/gc.c \
       lib/sqlite3/sqlite3.c \
       src/security/jwt/jwt.c \
       src/security/db_crypt/db_crypt.c \
       src/security/tls/ech.c \
       src/net/db_sync/db_sync.c \
       src/net/nats/cwist_nats.c \
       src/net/redis/cwist_redis.c \
       src/core/validation/bind.c \
       src/sys/io/io_uring_backend.c \
       src/sys/io/reactor.c \
       src/sys/job/scheduler.c \
       src/sys/metrics/metrics.c \
       src/sys/health/healthz.c \
       $(IO_SRC)

# Object Files and Target
OBJS = $(SRCS:.c=.o)
LIB_NAME = libcwist.a
LIBTTAK_DIR = lib/libttak
LIBTTAK_LIB = $(LIBTTAK_DIR)/lib/libttak.a
CJSON_DIR = lib/cjson
CJSON_LIB = $(CJSON_DIR)/libcjson.a
CNATS_DIR = lib/cnats
CNATS_LIB = $(CNATS_DIR)/build/lib/libnats_static.a

# Installation Paths
PREFIX ?= /usr/local
LIBDIR = $(PREFIX)/lib
INCLUDEDIR = $(PREFIX)/include

# --- Build Targets ---

all: $(LIBTTAK_LIB) $(CJSON_LIB) $(URIPARSER_LIB) $(SQLITE_DIR)/sqlite3.c $(LSQUIC_LIB) $(LIB_NAME)

# SQLite Download & Extraction Rule
$(SQLITE_DIR)/sqlite3.c:
	@echo "Downloading SQLite..."
	@mkdir -p $(SQLITE_DIR)
	@wget -q $(SQLITE_URL) -O $(SQLITE_DIR)/$(SQLITE_ZIP)
	@echo "Extracting SQLite..."
	@unzip -q -j $(SQLITE_DIR)/$(SQLITE_ZIP) -d $(SQLITE_DIR)
	@rm $(SQLITE_DIR)/$(SQLITE_ZIP)
	@echo "SQLite Ready."

# Ensure lsquic submodule is checked out before compiling objects that need its headers
$(OBJS): | lib/lsquic/include/lsquic.h

lib/lsquic/include/lsquic.h:
	@if [ ! -f "$@" ]; then \
		echo "Initializing lsquic submodule..."; \
		git submodule update --init --recursive $(LSQUIC_DIR); \
	fi

$(LIB_NAME): $(URIPARSER_LIB) $(CJSON_LIB) $(LIBTTAK_LIB) $(CNATS_LIB) $(BORINGSSL_SSL_LIB) $(BORINGSSL_CRYPTO_LIB) $(LSQUIC_LIB) $(OBJS)
	@echo "Creating static library..."
	@rm -rf .lib_merge_tmp
	@mkdir -p .lib_merge_tmp
	@cd .lib_merge_tmp && \
		ar x $(abspath $(URIPARSER_LIB)) && \
		ar x $(abspath $(CJSON_LIB)) && \
		ar x $(abspath $(LIBTTAK_LIB)) && \
		ar x $(abspath $(CNATS_LIB)) && \
		ar x $(abspath $(LSQUIC_LIB)) && \
		ar x $(abspath $(BORINGSSL_SSL_LIB)) && \
		ar x $(abspath $(BORINGSSL_CRYPTO_LIB))
	ar rcs $@ $(OBJS) .lib_merge_tmp/*.o
	@rm -rf .lib_merge_tmp

$(LIBTTAK_LIB):
	@echo "Building libttak..."
	$(MAKE) -C $(LIBTTAK_DIR)

$(CJSON_LIB):
	@echo "Building cJSON..."
	$(CC) -O3 -fPIC -I$(CJSON_DIR) -c $(CJSON_DIR)/cJSON.c -o $(CJSON_DIR)/cJSON.o
	ar rcs $@ $(CJSON_DIR)/cJSON.o
	@echo "cJSON Ready."

$(URIPARSER_LIB):
	@echo "Configuring uriparser..."
	cmake -S $(URIPARSER_DIR) -B $(URIPARSER_BUILD_DIR) -DCMAKE_C_COMPILER=$(CC) $(URIPARSER_CMAKE_FLAGS)
	@echo "Building uriparser..."
	cmake --build $(URIPARSER_BUILD_DIR) --target uriparser

BORINGSSL_STAMP = $(BORINGSSL_BUILD_DIR)/.boringssl_built

$(BORINGSSL_STAMP):
	@echo "Building BoringSSL..."
	@mkdir -p $(BORINGSSL_BUILD_DIR)
	cmake -S $(BORINGSSL_DIR) -B $(BORINGSSL_BUILD_DIR) \
		-DCMAKE_C_COMPILER=$(CC) \
		-DCMAKE_CXX_COMPILER=$(CXX) \
		-DCMAKE_BUILD_TYPE=Release
	cmake --build $(BORINGSSL_BUILD_DIR) --target ssl crypto
	@touch $@

$(BORINGSSL_SSL_LIB) $(BORINGSSL_CRYPTO_LIB): $(BORINGSSL_STAMP)

$(LSQUIC_LIB): $(BORINGSSL_SSL_LIB) $(BORINGSSL_CRYPTO_LIB)
	@echo "Building lsquic..."
	@mkdir -p $(LSQUIC_BUILD_DIR)
	cmake -S $(LSQUIC_DIR) -B $(LSQUIC_BUILD_DIR) \
		-DCMAKE_C_COMPILER=$(CC) \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_FLAGS="-Wno-unused-function" \
		-DBORINGSSL_DIR=$(abspath $(BORINGSSL_DIR)) \
		-DBORINGSSL_LIB_ssl=$(abspath $(BORINGSSL_SSL_LIB)) \
		-DBORINGSSL_LIB_crypto=$(abspath $(BORINGSSL_CRYPTO_LIB)) \
		-DBORINGSSL_INCLUDE=$(abspath $(BORINGSSL_DIR)/include) \
		-DBUILD_SHARED_LIBS=OFF
	cmake --build $(LSQUIC_BUILD_DIR) --target lsquic

$(CNATS_LIB):
	@echo "Building cnats..."
	@mkdir -p $(CNATS_DIR)/build
	cmake -S $(CNATS_DIR) -B $(CNATS_DIR)/build \
		-DCMAKE_C_COMPILER=$(CC) \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_SHARED_LIBS=OFF \
		-DNATS_BUILD_WITH_TLS=OFF \
		-DNATS_BUILD_STREAMING=OFF
	cmake --build $(CNATS_DIR)/build --target nats_static

# --- Test Targets ---

TEST_TARGETS = test_sstring \
               test_seq \
               test_http \
               test_siphash \
               test_mux \
               test_mux_param \
               test_rdbms_auto_mount \
               stress_test \
               test_cors \
               test_websocket \
               test_jwt \
               test_migrate \
               test_json_heal \
               test_https \
               test_http2 \
               test_http3 \
               test_shutdown \
               test_compress \
               test_log \
               nuke_missing_user_test \
               test_bind \
               test_metrics \
               test_io_uring \
               test_io_uring_demolition \
               test_access_log \
               test_rate_limit \
               test_cache \
               test_secure_headers \
               test_http_chunked \
               test_static_and_range \
               test_session \
               test_csrf \
               test_db_pool \
               test_redis \
               test_scheduler \
               test_test_client \
               test_multiport

.PHONY: all test $(TEST_TARGETS) install uninstall clean rebuild examples clean-examples

test: $(TEST_TARGETS)

test_sstring: $(LIB_NAME) tests/test_sstring.c
	$(CC) $(CFLAGS) -o test_sstring tests/test_sstring.c $(LIB_NAME) $(LIBS)
	./test_sstring

test_seq: $(LIB_NAME) tests/test_seq.c
	$(CC) $(CFLAGS) -o test_seq tests/test_seq.c $(LIB_NAME) $(LIBS)
	./test_seq

test_http: $(LIB_NAME) tests/test_http.c
	$(CC) $(CFLAGS) -o test_http tests/test_http.c $(LIB_NAME) $(LIBS)
	./test_http

test_siphash: $(LIB_NAME) tests/test_siphash.c
	$(CC) $(CFLAGS) -o test_siphash tests/test_siphash.c $(LIB_NAME) $(LIBS)
	./test_siphash

test_mux: $(LIB_NAME) tests/test_mux.c
	$(CC) $(CFLAGS) -o test_mux tests/test_mux.c $(LIB_NAME) $(LIBS)
	./test_mux

test_mux_param: $(LIB_NAME) tests/test_mux_param.c
	$(CC) $(CFLAGS) -o test_mux_param tests/test_mux_param.c $(LIB_NAME) $(LIBS)
	./test_mux_param

test_jwt: $(LIB_NAME) tests/test_jwt.c
	$(CC) $(CFLAGS) -o test_jwt tests/test_jwt.c $(LIB_NAME) $(LIBS)
	./test_jwt

test_migrate: $(LIB_NAME) tests/test_migrate.c
	$(CC) $(CFLAGS) -o test_migrate tests/test_migrate.c $(LIB_NAME) $(LIBS)
	./test_migrate

test_json_heal: $(LIB_NAME) tests/test_json_heal.c
	$(CC) $(CFLAGS) -o test_json_heal tests/test_json_heal.c $(LIB_NAME) $(LIBS)
	./test_json_heal

test_https: $(LIB_NAME) tests/test_https.c
	$(CC) $(CFLAGS) -o test_https tests/test_https.c $(LIB_NAME) $(LIBS)
	./test_https

test_http2: $(LIB_NAME) tests/test_http2.c
	$(CC) $(CFLAGS) -o test_http2 tests/test_http2.c $(LIB_NAME) $(LIBS)
	./test_http2

test_http3: $(LIB_NAME) tests/test_http3.c
	$(CC) $(CFLAGS) -o test_http3 tests/test_http3.c $(LIB_NAME) $(LIBS)
	./test_http3

test_rdbms_auto_mount: $(LIB_NAME) tests/test_rdbms_auto_mount.c
	$(CC) $(CFLAGS) -o test_rdbms_auto_mount tests/test_rdbms_auto_mount.c $(LIB_NAME) $(LIBS)
	./test_rdbms_auto_mount

stress_test: $(LIB_NAME) tests/stress_test.c
	$(CC) $(CFLAGS) -o stress_test tests/stress_test.c $(LIB_NAME) $(LIBS)
	./stress_test

test_cors: $(LIB_NAME) tests/test_cors.c
	$(CC) $(CFLAGS) -o test_cors tests/test_cors.c $(LIB_NAME) $(LIBS)
	./test_cors

test_websocket: $(LIB_NAME) tests/test_websocket.c
	$(CC) $(CFLAGS) -o test_websocket tests/test_websocket.c $(LIB_NAME) $(LIBS)
	./test_websocket

test_shutdown: $(LIB_NAME) tests/test_shutdown.c
	$(CC) $(CFLAGS) -o test_shutdown tests/test_shutdown.c $(LIB_NAME) $(LIBS)
	./test_shutdown

test_compress: $(LIB_NAME) tests/test_compress.c
	$(CC) $(CFLAGS) -o test_compress tests/test_compress.c $(LIB_NAME) $(LIBS)
	./test_compress

test_log: $(LIB_NAME) tests/test_log.c
	$(CC) $(CFLAGS) -o test_log tests/test_log.c $(LIB_NAME) $(LIBS)
	./test_log

nuke_missing_user_test: $(LIB_NAME) tests/nuke_missing_user_test.c
	$(CC) $(CFLAGS) -o nuke_missing_user_test tests/nuke_missing_user_test.c $(LIB_NAME) $(LIBS)
	./nuke_missing_user_test

test_bind: $(LIB_NAME) tests/test_bind.c
	$(CC) $(CFLAGS) -o test_bind tests/test_bind.c $(LIB_NAME) $(LIBS)
	./test_bind

test_metrics: $(LIB_NAME) tests/test_metrics.c
	$(CC) $(CFLAGS) -o test_metrics tests/test_metrics.c $(LIB_NAME) $(LIBS)
	./test_metrics

test_io_uring: $(LIB_NAME) tests/test_io_uring.c
	$(CC) $(CFLAGS) -o test_io_uring tests/test_io_uring.c $(LIB_NAME) $(LIBS)
	./test_io_uring

test_io_uring_demolition: $(LIB_NAME) tests/test_io_uring_demolition.c
	$(CC) $(CFLAGS) -o test_io_uring_demolition tests/test_io_uring_demolition.c $(LIB_NAME) $(LIBS)
	./test_io_uring_demolition

test_access_log: $(LIB_NAME) tests/test_access_log.c
	$(CC) $(CFLAGS) -o test_access_log tests/test_access_log.c $(LIB_NAME) $(LIBS)
	./test_access_log

test_secure_headers: $(LIB_NAME) tests/test_secure_headers.c
	$(CC) $(CFLAGS) -o test_secure_headers tests/test_secure_headers.c $(LIB_NAME) $(LIBS)
	./test_secure_headers

test_rate_limit: $(LIB_NAME) tests/test_rate_limit.c
	$(CC) $(CFLAGS) -o test_rate_limit tests/test_rate_limit.c $(LIB_NAME) $(LIBS)
	./test_rate_limit

test_cache: $(LIB_NAME) tests/test_cache.c
	$(CC) $(CFLAGS) -o test_cache tests/test_cache.c $(LIB_NAME) $(LIBS)
	./test_cache

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
	rm -f $(TEST_TARGETS)
	rm -f $(CJSON_DIR)/cJSON.o $(CJSON_LIB)
	rm -rf $(URIPARSER_BUILD_DIR)
	rm -rf .lib_merge_tmp
	@rm -rf $(CNATS_DIR)/build
	@rm -rf $(BORINGSSL_BUILD_DIR)
	@rm -rf $(LSQUIC_BUILD_DIR)
	-@$(MAKE) -C $(LIBTTAK_DIR) clean || true

rebuild: clean all

# ------------------------------------------------------------------
# Examples
# ------------------------------------------------------------------

EXAMPLE_BINS = example/simple-server/simple-server \
               example/http/step-1-hello-world/hello-world \
               example/http/step-2-mux-router/mux-router \
               example/http/step-3-query-params/query-params \
               example/http/step-4-json-api/json-api \
               example/jwt/step-2-http-auth/jwt-auth \
               example/cde-json-viewer/cde-json-viewer \
               example/db/step-1-open-query/open-query \
               example/db/step-2-migrations/migrations \
               example/db/step-4-json-insert/json-insert \
               example/rps-showcase/rps-showcase

examples: $(EXAMPLE_BINS)

example/simple-server/simple-server: $(LIB_NAME) example/simple-server/main.c
	$(CC) $(CFLAGS) -o $@ example/simple-server/main.c $(LIB_NAME) $(LIBS)

example/http/step-1-hello-world/hello-world: $(LIB_NAME) example/http/step-1-hello-world/main.c
	$(CC) $(CFLAGS) -o $@ example/http/step-1-hello-world/main.c $(LIB_NAME) $(LIBS)

example/http/step-2-mux-router/mux-router: $(LIB_NAME) example/http/step-2-mux-router/main.c
	$(CC) $(CFLAGS) -o $@ example/http/step-2-mux-router/main.c $(LIB_NAME) $(LIBS)

example/http/step-3-query-params/query-params: $(LIB_NAME) example/http/step-3-query-params/main.c
	$(CC) $(CFLAGS) -o $@ example/http/step-3-query-params/main.c $(LIB_NAME) $(LIBS)

example/http/step-4-json-api/json-api: $(LIB_NAME) example/http/step-4-json-api/main.c
	$(CC) $(CFLAGS) -o $@ example/http/step-4-json-api/main.c $(LIB_NAME) $(LIBS)

example/jwt/step-2-http-auth/jwt-auth: $(LIB_NAME) example/jwt/step-2-http-auth/main.c
	$(CC) $(CFLAGS) -o $@ example/jwt/step-2-http-auth/main.c $(LIB_NAME) $(LIBS)

example/cde-json-viewer/cde-json-viewer: $(LIB_NAME) example/cde-json-viewer/main.c
	$(CC) $(CFLAGS) -o $@ example/cde-json-viewer/main.c $(LIB_NAME) $(LIBS)

example/db/step-1-open-query/open-query: $(LIB_NAME) example/db/step-1-open-query/main.c
	$(CC) $(CFLAGS) -o $@ example/db/step-1-open-query/main.c $(LIB_NAME) $(LIBS)

example/db/step-2-migrations/migrations: $(LIB_NAME) example/db/step-2-migrations/main.c
	$(CC) $(CFLAGS) -o $@ example/db/step-2-migrations/main.c $(LIB_NAME) $(LIBS)

example/db/step-4-json-insert/json-insert: $(LIB_NAME) example/db/step-4-json-insert/main.c
	$(CC) $(CFLAGS) -o $@ example/db/step-4-json-insert/main.c $(LIB_NAME) $(LIBS)

example/rps-showcase/rps-showcase: $(LIB_NAME) example/rps-showcase/main.c
	$(CC) $(CFLAGS) -o $@ example/rps-showcase/main.c $(LIB_NAME) $(LIBS)

# Micro examples
MICRO_BINS = example/micro/01-hello/hello \
             example/micro/02-routes/routes \
             example/micro/03-path-params/path-params \
             example/micro/04-query-params/query-params \
             example/micro/05-json/json \
             example/micro/06-orm-insert/orm-insert \
             example/micro/07-orm-query/orm-query \
             example/micro/08-orm-update-delete/orm-update-delete \
             example/micro/09-html-builder/html-builder \
             example/micro/10-static-files/static-files \
             example/micro/11-jwt-auth/jwt-auth \
             example/micro/12-middleware/middleware \
             example/micro/13-nuke-db/nuke-db \
             example/micro/14-websocket/websocket \
             example/micro/15-pqc-tls/pqc-tls \
             example/micro/16-rdbms-auto/rdbms-auto \
             example/micro/17-blog-crud/blog-crud

micro-examples: $(MICRO_BINS)

example/micro/01-hello/hello: $(LIB_NAME) example/micro/01-hello/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/01-hello/main.c $(LIB_NAME) $(LIBS)

example/micro/02-routes/routes: $(LIB_NAME) example/micro/02-routes/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/02-routes/main.c $(LIB_NAME) $(LIBS)

example/micro/03-path-params/path-params: $(LIB_NAME) example/micro/03-path-params/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/03-path-params/main.c $(LIB_NAME) $(LIBS)

example/micro/04-query-params/query-params: $(LIB_NAME) example/micro/04-query-params/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/04-query-params/main.c $(LIB_NAME) $(LIBS)

example/micro/05-json/json: $(LIB_NAME) example/micro/05-json/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/05-json/main.c $(LIB_NAME) $(LIBS)

example/micro/06-orm-insert/orm-insert: $(LIB_NAME) example/micro/06-orm-insert/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/06-orm-insert/main.c $(LIB_NAME) $(LIBS)

example/micro/07-orm-query/orm-query: $(LIB_NAME) example/micro/07-orm-query/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/07-orm-query/main.c $(LIB_NAME) $(LIBS)

example/micro/08-orm-update-delete/orm-update-delete: $(LIB_NAME) example/micro/08-orm-update-delete/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/08-orm-update-delete/main.c $(LIB_NAME) $(LIBS)

example/micro/09-html-builder/html-builder: $(LIB_NAME) example/micro/09-html-builder/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/09-html-builder/main.c $(LIB_NAME) $(LIBS)

example/micro/10-static-files/static-files: $(LIB_NAME) example/micro/10-static-files/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/10-static-files/main.c $(LIB_NAME) $(LIBS)

example/micro/11-jwt-auth/jwt-auth: $(LIB_NAME) example/micro/11-jwt-auth/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/11-jwt-auth/main.c $(LIB_NAME) $(LIBS)

example/micro/12-middleware/middleware: $(LIB_NAME) example/micro/12-middleware/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/12-middleware/main.c $(LIB_NAME) $(LIBS)

example/micro/13-nuke-db/nuke-db: $(LIB_NAME) example/micro/13-nuke-db/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/13-nuke-db/main.c $(LIB_NAME) $(LIBS)

example/micro/14-websocket/websocket: $(LIB_NAME) example/micro/14-websocket/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/14-websocket/main.c $(LIB_NAME) $(LIBS)

example/micro/15-pqc-tls/pqc-tls: $(LIB_NAME) example/micro/15-pqc-tls/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/15-pqc-tls/main.c $(LIB_NAME) $(LIBS)

example/micro/16-rdbms-auto/rdbms-auto: $(LIB_NAME) example/micro/16-rdbms-auto/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/16-rdbms-auto/main.c $(LIB_NAME) $(LIBS)

example/micro/17-blog-crud/blog-crud: $(LIB_NAME) example/micro/17-blog-crud/main.c
	$(CC) $(CFLAGS) -o $@ example/micro/17-blog-crud/main.c $(LIB_NAME) $(LIBS)

clean-examples:
	rm -f $(EXAMPLE_BINS) $(MICRO_BINS)

test_http_chunked: $(LIB_NAME) tests/test_http_chunked.c
	$(CC) $(CFLAGS) -o test_http_chunked tests/test_http_chunked.c $(LIB_NAME) $(LIBS)
	./test_http_chunked

test_static_and_range: $(LIB_NAME) tests/test_static_and_range.c
	$(CC) $(CFLAGS) -o test_static_and_range tests/test_static_and_range.c $(LIB_NAME) $(LIBS)
	./test_static_and_range

test_session: $(LIB_NAME) tests/test_session.c
	$(CC) $(CFLAGS) -o test_session tests/test_session.c $(LIB_NAME) $(LIBS)
	./test_session

test_csrf: $(LIB_NAME) tests/test_csrf.c
	$(CC) $(CFLAGS) -o test_csrf tests/test_csrf.c $(LIB_NAME) $(LIBS)
	./test_csrf

test_db_pool: $(LIB_NAME) tests/test_db_pool.c
	$(CC) $(CFLAGS) -o test_db_pool tests/test_db_pool.c $(LIB_NAME) $(LIBS)
	./test_db_pool

test_redis: $(LIB_NAME) tests/test_redis.c
	$(CC) $(CFLAGS) -o test_redis tests/test_redis.c $(LIB_NAME) $(LIBS)
	./test_redis

test_scheduler: $(LIB_NAME) tests/test_scheduler.c
	$(CC) $(CFLAGS) -o test_scheduler tests/test_scheduler.c $(LIB_NAME) $(LIBS)
	./test_scheduler

test_test_client: $(LIB_NAME) tests/test_test_client.c
	$(CC) $(CFLAGS) -o test_test_client tests/test_test_client.c $(LIB_NAME) $(LIBS)
	./test_test_client

test_multiport: $(LIB_NAME) tests/test_multiport.c
	$(CC) $(CFLAGS) -o test_multiport tests/test_multiport.c $(LIB_NAME) $(LIBS)
	./test_multiport
