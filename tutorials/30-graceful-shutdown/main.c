#include <cwist/app.h>
#include <cwist/core/mem/gc.h>

static void handle_home(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "{\"status\":\"ok\",\"message\":\"Graceful shutdown ready\"}");
}

int main(void) {
    /* cwist_full_gc(true): the automatic half of graceful shutdown. A
     * worker thread that exits (or a process that ends) without running
     * the explicit cwist_app_destroy() path below still gets its
     * connections closed and its cwist_alloc() blocks reclaimed -- a TLS
     * destructor sweeps per-thread on thread exit, and an atexit hook
     * sweeps whatever is left on process exit. This is a one-shot,
     * process-wide toggle (first call wins, every later call is a no-op)
     * and it is off by default: the explicit model below runs unchanged
     * at zero cost if you never call this. See docs/GC.md.
     *
     * It complements cwist_app_destroy(), it does not replace it: call
     * both, same as here. The explicit path stays the primary, deterministic
     * shutdown; full-GC is the safety net for the paths that skip it (a
     * worker thread that panics, a signal handler that exits early, ...). */
    cwist_full_gc(true);

    cwist_app *app = cwist_app_create();
    cwist_app_get(app, "/", handle_home);
    printf("Starting server. Send SIGTERM/SIGINT to initiate graceful shutdown.\n");
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
