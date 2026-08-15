#include <cwist/net/http/http.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

void test_security_headers_present(void) {
    printf("Testing security headers...\n");

    cwist_http_response *res = cwist_http_response_create();
    assert(res != NULL);
    cwist_http_response_add_security_headers(res);

    // Verify headers added by cwist_http_response_add_security_headers()
    assert(cwist_http_header_get(res->headers, "X-Frame-Options") != NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "X-Frame-Options"), "DENY") == 0);

    assert(cwist_http_header_get(res->headers, "X-Content-Type-Options") != NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "X-Content-Type-Options"), "nosniff") == 0);

    assert(cwist_http_header_get(res->headers, "Referrer-Policy") != NULL);

    assert(cwist_http_header_get(res->headers, "Content-Security-Policy") != NULL);

    assert(cwist_http_header_get(res->headers, "Cross-Origin-Resource-Policy") != NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "Cross-Origin-Resource-Policy"), "same-origin") == 0);

    // HSTS should now be present (added as part of secure-headers hardening)
    assert(cwist_http_header_get(res->headers, "Strict-Transport-Security") != NULL);
    assert(strcmp(cwist_http_header_get(res->headers, "Strict-Transport-Security"),
                  "max-age=31536000; includeSubDomains") == 0);

    cwist_http_response_destroy(res);
    printf("Passed security headers.\n");
}

void test_security_headers_not_duplicated(void) {
    printf("Testing security headers deduplication...\n");

    cwist_http_response *res = cwist_http_response_create();
    assert(res != NULL);

    // Manually add an existing header with a different value
    cwist_http_header_add(&res->headers, "X-Frame-Options", "SAMEORIGIN");

    // Re-run security header injection
    cwist_http_response_add_security_headers(res);

    // The manually-added value should remain (first one wins)
    assert(strcmp(cwist_http_header_get(res->headers, "X-Frame-Options"), "SAMEORIGIN") == 0);

    cwist_http_response_destroy(res);
    printf("Passed deduplication.\n");
}

int main(void) {
    test_security_headers_present();
    test_security_headers_not_duplicated();
    printf("All secure-headers tests passed.\n");
    return 0;
}
