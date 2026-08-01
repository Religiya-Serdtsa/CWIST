# Build a WebTransport server with CWIST

CWIST exposes WebTransport over its HTTP/3 server. WebTransport requires TLS
and QUIC; browsers also require a certificate they trust during development.

> **Platform note:** the HTTP/3 listener uses `epoll` on Linux and a portable
> polling path on macOS/BSD. ECN receive metadata is optional, so missing BSD
> socket extensions do not block a WebTransport build or listener startup.

## 1. Configure HTTP/3 and a session handler

```c
#include <cwist/sys/app/app.h>

static void on_webtransport(cwist_http_request *req,
                            cwist_http_response *res,
                            void *session) {
    (void)req;
    (void)session;
    res->status_code = CWIST_HTTP_OK; /* Accept the CONNECT session. */
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use_https3(app, true);       /* Or configure certificate/key first. */
    cwist_app_use_webtransport(app, on_webtransport);
    int rc = cwist_app_listen(app, 4433);
    cwist_app_destroy(app);
    return rc;
}
```

`cwist_app_use_http3(app, true)` is suitable for local experimentation because
it creates an ephemeral certificate. For a browser deployment, use a real TLS
certificate and make the public endpoint reachable over UDP.

## 2. Design the session protocol

WebTransport gives an application reliable streams and optional datagrams. Put
a small versioned envelope around each application message, cap message sizes,
and authenticate the session before accepting commands. Use streams for CRUD,
chat history, and acknowledgements; use datagrams only for replaceable state
such as cursor positions or game snapshots.

Do not assume datagrams arrive, arrive once, or arrive in order. Include a
monotonic sequence number and discard stale updates. For reliable stream data,
CWIST's sequence helpers can reassemble application-level fragmented payloads
when a protocol intentionally splits large messages.

## 3. Operate safely

Set per-session limits for concurrent streams, buffered bytes, and idle time.
Reject unsupported CONNECT paths before allocating a session. Log the request
identifier, negotiated origin, close reason, and transport errors, but never
log credentials or application payloads. Exercise the handler with the
WebTransport regression test before enabling it on a public listener.
