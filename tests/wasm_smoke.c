/* WASM smoke test for the in-memory dispatcher (not wired into CI — CI has
 * no Emscripten).  Manual build & run:
 *
 *   make wasm EMCC=/workspace/emsdk/upstream/emscripten/emcc \
 *             EMAR=/workspace/emsdk/upstream/emscripten/emar
 *   /workspace/emsdk/upstream/emscripten/emcc -std=c17 -O2 -I./include -I./lib \
 *       -o wasm_smoke.js tests/wasm_smoke.c libcwist_wasm.a
 *   node wasm_smoke.js
 */
#include <cwist/sys/app/app.h>
#include <cwist/core/mem/alloc.h>
#include <stdio.h>
#include <string.h>

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "hello-wasm");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    if (!app) return 1;
    cwist_app_get(app, "/hello", hello_handler);

    static const char req[] = "GET /hello HTTP/1.1\r\nHost: wasm\r\n\r\n";
    char *res_buf = NULL;
    size_t res_len = 0;
    if (cwist_app_dispatch_memory(app, req, sizeof(req) - 1,
                                  &res_buf, &res_len) != 0) {
        fprintf(stderr, "wasm_smoke: dispatch failed\n");
        cwist_app_destroy(app);
        return 2;
    }

    int ok = res_buf != NULL &&
             strncmp(res_buf, "HTTP/1.1 200 OK\r\n", 17) == 0 &&
             strstr(res_buf, "Content-Type: text/plain") != NULL &&
             res_len >= 10 &&
             memcmp(res_buf + res_len - 10, "hello-wasm", 10) == 0;
    if (!ok) {
        fprintf(stderr, "wasm_smoke: bad response:\n%.*s\n", (int)res_len, res_buf);
        cwist_free(res_buf);
        cwist_app_destroy(app);
        return 3;
    }
    cwist_free(res_buf);
    cwist_app_destroy(app);
    printf("wasm_smoke: OK\n");
    return 0;
}
