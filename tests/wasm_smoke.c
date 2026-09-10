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
#include <cwist/wasm/typedarray.h>
#include <stdio.h>
#include <string.h>

static const int32_t g_samples[] = { 10, -20, 30, 40 };
CWIST_WASM_EXPOSE_I32(samples, g_samples, 4)
CWIST_WASM_INSTALL_VIEWS()

/* JS reads the response through a HEAPU8 view and the exposed struct array
 * through cwistView.i32 — any mismatch throws and node exits non-zero. */
EM_JS(void, js_verify, (const char *res_ptr, int res_len), {
    const bytes = Module.cwistView.u8(res_ptr, res_len);
    const text = new TextDecoder().decode(bytes);
    if (!text.startsWith("HTTP/1.1 200 OK\r\n") || !text.endsWith("hello-wasm")) {
        throw new Error("bad response via TypedArray: " + JSON.stringify(text));
    }
    const samples = Module.cwistView.i32(_samples_ptr(), _samples_len());
    const total = Array.from(samples).reduce((a, b) => a + b, 0);
    if (total !== 60 || samples.length !== 4) {
        throw new Error("bad i32 view: " + Array.from(samples).join(","));
    }
    console.log("wasm_smoke: JS TypedArray views verified (" +
                res_len + " response bytes, samples sum " + total + ")");
});

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "hello-wasm");
    cwist_http_header_add(&res->headers, "Content-Type", "text/plain");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    if (!app) return 1;
    cwist_app_get(app, "/hello", hello_handler);
    cwist_wasm_install_views();

    static const char req[] = "GET /hello HTTP/1.1\r\nHost: wasm\r\n\r\n";
    size_t res_len = 0;
    const char *res_buf = cwist_wasm_dispatch_memory(app, req, sizeof(req) - 1,
                                                     &res_len);
    if (!res_buf) {
        fprintf(stderr, "wasm_smoke: dispatch failed\n");
        cwist_app_destroy(app);
        return 2;
    }

    int ok = strncmp(res_buf, "HTTP/1.1 200 OK\r\n", 17) == 0 &&
             strstr(res_buf, "Content-Type: text/plain") != NULL &&
             res_len >= 10 &&
             memcmp(res_buf + res_len - 10, "hello-wasm", 10) == 0;
    if (!ok) {
        fprintf(stderr, "wasm_smoke: bad response:\n%.*s\n", (int)res_len, res_buf);
        cwist_wasm_free((void *)res_buf);
        cwist_app_destroy(app);
        return 3;
    }

    /* Zero-copy handoff: JS reads the same heap bytes as a Uint8Array. */
    js_verify(res_buf, (int)res_len);
    cwist_wasm_free((void *)res_buf);
    cwist_app_destroy(app);
    printf("wasm_smoke: OK\n");
    return 0;
}
