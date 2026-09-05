/**
 * @file test_static_and_range.c
 * @brief Test static file security (path traversal, symlink, URL decode)
 *        and HTTP Range request support.
 */

#include <cwist/sys/app/app.h>
#include <cwist/sys/app/shutdown.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <ctype.h>

#define TEST_PORT 19998
#define TEST_HOST "127.0.0.1"

/* strcasestr() is a GNU extension missing on macOS; local ASCII-only variant. */
static const char *test_strcasestr(const char *haystack, const char *needle) {
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n &&
               tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++;
            n++;
        }
        if (!*n) return haystack;
    }
    return NULL;
}

static int connect_to_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TEST_PORT),
    };
    inet_pton(AF_INET, TEST_HOST, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_request(int fd, const char *req) {
    size_t len = strlen(req);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, req + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int read_response(int fd, char *buf, size_t buf_size) {
    size_t total = 0;
    size_t want = 0; /* header end + Content-Length once known */
    while (total < buf_size - 1) {
        ssize_t n = recv(fd, buf + total, buf_size - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        /* Stop as soon as the full response (headers + declared body) has
         * arrived; the server keeps the connection alive, so waiting for
         * EOF would block forever. */
        if (want == 0) {
            char *end = strstr(buf, "\r\n\r\n");
            if (end) {
                size_t header_len = (size_t)(end + 4 - buf);
                char *cl = strcasestr(buf, "Content-Length:");
                if (cl && cl < end) {
                    want = header_len + (size_t)strtoul(cl + 15, NULL, 10);
                } else {
                    want = header_len;
                }
            }
        }
        if (want && total >= want) break;
    }
    buf[total] = '\0';
    return (int)total;
}

static bool response_has_status(const char *response, const char *status_line) {
    return strstr(response, status_line) != NULL;
}

static bool response_has_code(const char *response, const char *code) {
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "HTTP/1.1 %s", code);
    return strstr(response, prefix) != NULL;
}

static bool response_has_header(const char *response, const char *header) {
    return test_strcasestr(response, header) != NULL;
}

int main(void) {
    printf("Testing static file security and range requests...\n");

    char tmpdir[] = "/tmp/cwist_static_test_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        perror("mkdtemp");
        return 1;
    }

    char index_path[256];
    snprintf(index_path, sizeof(index_path), "%s/index.html", tmpdir);
    FILE *f = fopen(index_path, "w");
    if (!f) {
        perror("fopen");
        return 1;
    }
    fprintf(f, "Hello World");
    fclose(f);

    /* Create a symlink escape target */
    char secret_path[256];
    snprintf(secret_path, sizeof(secret_path), "%s/secret.txt", tmpdir);
    f = fopen(secret_path, "w");
    if (f) {
        fprintf(f, "TOPSECRET");
        fclose(f);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* Child: run server */
        setenv("CWIST_C1M_MODE", "false", 1);
        cwist_app *app = cwist_app_create();
        if (!app) {
            fprintf(stderr, "Failed to create app\n");
            _exit(1);
        }
        cwist_app_static(app, "/static", tmpdir);
        g_cwist_drain_timeout_sec = 1;
        int rc = cwist_app_listen(app, TEST_PORT);
        cwist_app_destroy(app);
        _exit(rc == 0 ? 0 : 1);
    }

    /* Parent: give server time to start */
    usleep(400000);

    char buf[4096];
    int fd;
    int failures = 0;

    /* Test 1: Normal static file request */
    fd = connect_to_server();
    if (fd >= 0) {
        send_request(fd, "GET /static/index.html HTTP/1.1\r\nHost: localhost\r\n\r\n");
        read_response(fd, buf, sizeof(buf));
        if (!response_has_code(buf, "200")) {
            fprintf(stderr, "FAIL: Expected 200 OK for normal request, got:\n%s\n", buf);
            failures++;
        } else if (!response_has_header(buf, "Accept-Ranges: bytes")) {
            fprintf(stderr, "FAIL: Missing Accept-Ranges header\n");
            failures++;
        }
        close(fd);
    } else {
        fprintf(stderr, "FAIL: Could not connect for normal request\n");
        failures++;
    }

    /* Test 2: Path traversal via .. */
    fd = connect_to_server();
    if (fd >= 0) {
        send_request(fd, "GET /static/../../../etc/passwd HTTP/1.1\r\nHost: localhost\r\n\r\n");
        read_response(fd, buf, sizeof(buf));
        if (!response_has_code(buf, "403")) {
            fprintf(stderr, "FAIL: Expected 403 for path traversal, got:\n%s\n", buf);
            failures++;
        }
        close(fd);
    } else {
        fprintf(stderr, "FAIL: Could not connect for traversal test\n");
        failures++;
    }

    /* Test 3: URL-encoded path traversal */
    fd = connect_to_server();
    if (fd >= 0) {
        send_request(fd, "GET /static/%2e%2e/%2e%2e/secret.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
        read_response(fd, buf, sizeof(buf));
        if (!response_has_code(buf, "403")) {
            fprintf(stderr, "FAIL: Expected 403 for URL-encoded traversal, got:\n%s\n", buf);
            failures++;
        }
        close(fd);
    } else {
        fprintf(stderr, "FAIL: Could not connect for encoded traversal test\n");
        failures++;
    }

    /* Test 4: Range request */
    fd = connect_to_server();
    if (fd >= 0) {
        send_request(fd, "GET /static/index.html HTTP/1.1\r\nHost: localhost\r\nRange: bytes=0-4\r\n\r\n");
        read_response(fd, buf, sizeof(buf));
        if (!response_has_code(buf, "206")) {
            fprintf(stderr, "FAIL: Expected 206 for range request, got:\n%s\n", buf);
            failures++;
        } else if (!response_has_header(buf, "Content-Range: bytes 0-4/11")) {
            fprintf(stderr, "FAIL: Missing or incorrect Content-Range header:\n%s\n", buf);
            failures++;
        } else if (!strstr(buf, "Hello")) {
            fprintf(stderr, "FAIL: Body does not contain 'Hello':\n%s\n", buf);
            failures++;
        }
        close(fd);
    } else {
        fprintf(stderr, "FAIL: Could not connect for range test\n");
        failures++;
    }

    /* Test 5: Suffix range request (last 5 bytes) */
    fd = connect_to_server();
    if (fd >= 0) {
        send_request(fd, "GET /static/index.html HTTP/1.1\r\nHost: localhost\r\nRange: bytes=-5\r\n\r\n");
        read_response(fd, buf, sizeof(buf));
        if (!response_has_code(buf, "206")) {
            fprintf(stderr, "FAIL: Expected 206 for suffix range request, got:\n%s\n", buf);
            failures++;
        } else if (!strstr(buf, "World")) {
            fprintf(stderr, "FAIL: Body does not contain 'World':\n%s\n", buf);
            failures++;
        }
        close(fd);
    } else {
        fprintf(stderr, "FAIL: Could not connect for suffix range test\n");
        failures++;
    }

    /* Cleanup */
    kill(pid, SIGTERM);
    int status;
    int waited = 0;
    while (waited < 50) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        usleep(100000);
        waited++;
    }
    if (waited >= 50) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }

    unlink(index_path);
    unlink(secret_path);
    rmdir(tmpdir);

    if (failures == 0) {
        printf("Passed static file security and range request tests.\n");
        return 0;
    }
    fprintf(stderr, "Failed %d test(s).\n", failures);
    return 1;
}
