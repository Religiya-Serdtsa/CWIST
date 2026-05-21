/**
 * @file rdbms_auto_mount.c
 * @brief Automatic RDBMS provider detection and runtime mounting.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include <cwist/sys/app/app.h>
#include <cwist/core/log.h>

/**
 * @brief Send a minimal PostgreSQL StartupMessage and check for 'R' response.
 *
 * @param sock Connected socket.
 * @return true if server responds with an AuthenticationRequest ('R').
 */
static bool probe_postgresql(int sock)
{
    char startup[64];
    memset(startup, 0, sizeof(startup));

    /* "user\0postgres\0\0" payload after version */
    const char *user_key = "user";
    const char *user_val = "postgres";
    size_t payload = strlen(user_key) + 1 + strlen(user_val) + 1 + 1; /* +1 terminator */
    uint32_t len = htonl((uint32_t)(4 + 4 + payload));
    uint32_t version = htonl(196608); /* 3.0 */

    memcpy(startup, &len, 4);
    memcpy(startup + 4, &version, 4);
    strcpy(startup + 8, user_key);
    strcpy(startup + 8 + strlen(user_key) + 1, user_val);
    /* trailing null already present from memset */

    size_t total = 8 + payload;
    if (send(sock, startup, total, 0) != (ssize_t)total)
        return false;

    char resp = 0;
    ssize_t n = recv(sock, &resp, 1, 0);
    return (n == 1 && resp == 'R');
}

/**
 * @brief Inspect a MySQL/MariaDB handshake initiation packet.
 *
 * @param buf  Received bytes.
 * @param len  Number of valid bytes in @p buf.
 * @return Detected provider (MYSQL or MARIADB) or UNKNOWN.
 */
static cwist_rdbms_provider_t classify_mysql(const char *buf, size_t len)
{
    if (len < 5)
        return CWIST_RDBMS_NONE;

    unsigned char protocol = (unsigned char)buf[4];
    if (protocol != 0x0a)
        return CWIST_RDBMS_NONE;

    /* Look for "MariaDB" in the server version string that follows */
    for (size_t i = 5; i + 6 < len; ++i) {
        if (memcmp(&buf[i], "MariaDB", 7) == 0)
            return CWIST_RDBMS_MARIADB;
    }
    return CWIST_RDBMS_MYSQL;
}

cwist_rdbms_provider_t cwist_rdbms_probe_port(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        CWIST_LOG_ERROR("RDBMS probe: socket creation failed: %s", strerror(errno));
        return CWIST_RDBMS_NONE;
    }

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        CWIST_LOG_ERROR("RDBMS probe: connect to 127.0.0.1:%d failed: %s", port, strerror(errno));
        close(sock);
        return CWIST_RDBMS_NONE;
    }

    char buf[512];
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    if (n > 0) {
        cwist_rdbms_provider_t p = classify_mysql(buf, (size_t)n);
        if (p != CWIST_RDBMS_NONE) {
            close(sock);
            return p;
        }
        /* Data was received but did not look like MySQL; could be noise.
         * Close and re-probe as PostgreSQL below. */
        close(sock);
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
            return CWIST_RDBMS_NONE;
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(sock);
            return CWIST_RDBMS_NONE;
        }
    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        CWIST_LOG_ERROR("RDBMS probe: recv error on port %d: %s", port, strerror(errno));
        close(sock);
        return CWIST_RDBMS_NONE;
    }

    /* PostgreSQL speaks only after the client sends a StartupMessage */
    if (probe_postgresql(sock)) {
        close(sock);
        return CWIST_RDBMS_POSTGRES;
    }

    close(sock);
    return CWIST_RDBMS_NONE;
}

bool cwist_rdbms_mount_runtime(cwist_app *app, cwist_rdbms_provider_t provider, int port)
{
    if (!app)
        return false;

    if (app->rdbms) {
        CWIST_LOG_INFO("RDBMS runtime already mounted");
        return true;
    }

    struct cwist_rdbms_runtime *rt = (struct cwist_rdbms_runtime *)malloc(sizeof(*rt));
    if (!rt) {
        CWIST_LOG_ERROR("RDBMS mount: out of memory");
        return false;
    }

    rt->provider = provider;
    rt->port = port;
    rt->ready = true;

    app->rdbms = rt;
    CWIST_LOG_INFO("RDBMS runtime mounted: provider=%d port=%d", (int)provider, port);
    return true;
}

bool cwist_app_auto_rdbms(cwist_app *app, int port)
{
    if (!app) {
        CWIST_LOG_ERROR("RDBMS auto-mount: app is NULL");
        return false;
    }

    cwist_rdbms_provider_t provider = cwist_rdbms_probe_port(port);
    if (provider == CWIST_RDBMS_NONE) {
        CWIST_LOG_ERROR("RDBMS auto-detection failed on port %d", port);
        return false;
    }

    return cwist_rdbms_mount_runtime(app, provider, port);
}
