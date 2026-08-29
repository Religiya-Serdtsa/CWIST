/* Minimal native WebTransport client for the LSQUIC PR #629 dev build. */
#include <cwist/net/http/http3_client.h>

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "localhost";
    const char *path = argc > 2 ? argv[2] : "/wt";

    cwist_http3_client *client = cwist_http3_client_create();
    if (!client || cwist_http3_client_set_server(client, host, 9443) != 0) {
        fprintf(stderr, "could not create WebTransport client\n");
        return 1;
    }
    /* The bundled server sample uses a self-signed development certificate. */
    cwist_http3_client_set_insecure(client, 1);

    cwist_webtransport_client_session *session = NULL;
    cwist_error_t result = cwist_http3_client_webtransport_connect(
        client, path, NULL, &session);
    if (result.error.err_i16 != 0) {
        fprintf(stderr, "WebTransport CONNECT failed\n");
        cwist_http3_client_destroy(client);
        return 1;
    }

    const char message[] = "hello from CWIST native WebTransport client";
    if (cwist_webtransport_client_send_datagram(session, message,
                                                sizeof(message) - 1) < 0)
        fprintf(stderr, "datagram was not queued\n");

    for (int i = 0; i < 20 && cwist_webtransport_client_is_open(session); ++i)
        cwist_webtransport_client_poll(client, 100);

    cwist_webtransport_client_close(session, 0, "done");
    cwist_http3_client_destroy(client);
    return 0;
}
