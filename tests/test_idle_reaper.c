#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <cwist/net/http/writer_fast.h>
#include <cwist/net/http/http.h>

static void test_monotonic_sec(void) {
    printf("Testing cwist_fast_monotonic_sec... ");
    uint32_t t1 = cwist_fast_monotonic_sec();
    assert(t1 > 0);
    usleep(10000); /* 10ms */
    uint32_t t2 = cwist_fast_monotonic_sec();
    assert(t2 >= t1);
    printf("OK (t1=%u, t2=%u)\n", t1, t2);
}

static void test_idle_reaper_logic(void) {
    printf("Testing idle timeout detection logic... ");
    cwist_http_async_conn_t conn = {0};
    conn.last_active_sec = cwist_fast_monotonic_sec() - 20; /* 20 seconds ago */

    uint32_t now = cwist_fast_monotonic_sec();
    uint32_t timeout_sec = 15; /* 15s limit */

    bool is_expired = (conn.last_active_sec > 0 && (now - conn.last_active_sec) > timeout_sec);
    assert(is_expired == true);

    conn.last_active_sec = now;
    is_expired = (conn.last_active_sec > 0 && (now - conn.last_active_sec) > timeout_sec);
    assert(is_expired == false);
    printf("OK\n");
}

int main(void) {
    test_monotonic_sec();
    test_idle_reaper_logic();
    printf("All idle reaper tests passed!\n");
    return 0;
}
