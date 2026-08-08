#include <cwist/app.h>
#include <cwist/sys/app/big_dumb_reply.h>

int main(void) {
    cwist_bdr_t *bdr = cwist_bdr_create();
    if (bdr) {
        const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!";
        cwist_bdr_put(bdr, "GET", "/static", resp, strlen(resp));

        size_t out_len = 0;
        const void *cached = cwist_bdr_get(bdr, "GET", "/static", &out_len);
        if (cached) {
            printf("BDR Cache Hit (%zu bytes)\n", out_len);
        }
        cwist_bdr_destroy(bdr);
    }
    return 0;
}
