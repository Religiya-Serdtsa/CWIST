# CWIST lsquic / WebTransport Development Status

This note tracks the current state of CWIST's lsquic-backed HTTP/3 and
WebTransport integration on this branch.

## Current lsquic Baseline

CWIST is currently using the in-tree `lib/lsquic` submodule at:

```text
68b46927c8f2a8c8343b02880691214bc1523766
```

This is the draft full WebTransport implementation proposed by
`@dtikhonov`.  The branch exposes the public WebTransport API via
`lsquic_wt.h`, including:

- `lsquic_wt_accept()` / `lsquic_wt_reject()`
- `lsquic_wt_open_uni()` / `lsquic_wt_open_bidi()`
- `lsquic_wt_send_datagram()`
- `lsquic_wt_close()`
- WebTransport session, stream, and datagram callbacks

The local lsquic static archive has been rebuilt with WebTransport support and
now exports the expected `lsquic_wt_*` symbols.

## Platform Portability

The HTTP/3 server and native client configure their UDP sockets as non-blocking
before lsquic sends packets. The Linux server loop uses `epoll`; macOS and BSD
use the portable polling path. BSD-derived socket interfaces do not all expose
`MSG_DONTWAIT`, ECN receive options, or ancillary-data macros, so CWIST treats
those as optional capabilities. HTTP/3 continues to build and run without ECN
metadata when a platform omits them.

## Implemented in CWIST

CWIST's HTTP/3 server now uses the new lsquic WebTransport API instead of the
previous CONNECT-stream placeholder path.

Implemented pieces:

- WebTransport HTTP/3 CONNECT detection using `:protocol=webtransport`.
- WebTransport session acceptance through `lsquic_wt_accept()`.
- WebTransport session callback wiring through `struct lsquic_webtransport_if`.
- Application WebTransport handler invocation after lsquic opens the session.
- New peer-initiated WT unidirectional and bidirectional stream callbacks.
- Existing CWIST new-stream callback delivery for WT data streams.
- Server-initiated WT stream creation through:
  - `cwist_webtransport_open_bidi_stream()`
  - `cwist_webtransport_open_uni_stream()`
- WT datagram send support through:
  - `cwist_webtransport_send_datagram()`
  - `cwist_webtransport_max_datagram_size()`
- WT session close support through:
  - `cwist_webtransport_close_session()`
- WT datagram receive delivery through the existing HTTP/3 datagram callback.
- Strict CWIST-owned opaque WebTransport handles with runtime magic/version/kind
  validation.
- Required lsquic settings for WebTransport:
  - `es_webtransport = 1`
  - `es_http_datagrams = 1`
  - `es_datagrams = 1`
  - `es_reset_stream_at = 1`
  - `es_max_webtransport_sessions = 1`

## API Notes

The third argument passed to `cwist_webtransport_handler_func` is now an opaque
CWIST WebTransport session handle, not a raw CONNECT stream pointer and not a
raw `lsquic_wt_session_t *`.

Applications should pass this session handle to:

- `cwist_webtransport_open_bidi_stream()`
- `cwist_webtransport_open_uni_stream()`
- `cwist_webtransport_send_datagram()`
- `cwist_webtransport_max_datagram_size()`
- `cwist_webtransport_close_session()`

WT data stream I/O still uses stream handles:

- `cwist_webtransport_read()`
- `cwist_webtransport_write()`
- `cwist_webtransport_flush()`
- `cwist_webtransport_close_stream()`

## Breaking Change

WebTransport handles are now strict CWIST-owned opaque handles.

Applications must not cast WebTransport session or stream handles to lsquic
types.  The following patterns are no longer valid:

```c
lsquic_wt_session_t *sess = session;
lsquic_wt_open_bidi(sess);

lsquic_stream_t *s = stream;
lsquic_stream_write(s, data, len);
```

Use only the CWIST WebTransport API with handles received from CWIST callbacks:

```c
cwist_webtransport_open_bidi_stream(session);
cwist_webtransport_send_datagram(session, data, len);
cwist_webtransport_write(stream, data, len);
```

This is a source-level compatibility break only for applications that depended
on the old raw lsquic pointer behavior.  Applications that already treated the
handles as opaque and used `cwist_webtransport_*()` APIs should only need to
rebuild against the matching CWIST headers and library.

## Verified

The following checks were run successfully:

```text
cmake --build lib/lsquic/build --target lsquic
make test_webtransport
```

`make test_webtransport` passes and confirms:

- app-level WT handler registration
- HTTP/3 context-level WT handler registration
- WT new-stream handler registration
- HTTP/3 server loop startup/shutdown with WT enabled
- null-safety for public WT I/O functions
- app-level WT handler persistence across HTTP/3 context refresh

## Not Yet Verified

The current tests are infrastructure-level tests.  They do not yet perform a
real WebTransport client/server handshake over QUIC.

Still needs verification:

- Browser or native client WebTransport handshake against CWIST.
- CONNECT response behavior with real peer SETTINGS negotiation.
- Incoming client-initiated WT bidirectional stream payload echo/read/write.
- Incoming client-initiated WT unidirectional stream payload read.
- Server-initiated WT uni/bidi stream creation observed by a real peer.
- WT datagram send and receive over both QUIC DATAGRAM and lsquic's HTTP
  Datagram fallback path.
- Session close capsule behavior and application error-code propagation.
- Stream reset and STOP_SENDING behavior.
- Multiple concurrent sessions.  CWIST currently configures one WT session per
  QUIC connection because this lsquic branch rejects higher values.

## Known Limitations

- The public CWIST WebTransport API is still `void *` based, but handles are
  now runtime-validated using CWIST-owned magic/version/kind metadata.  This
  catches session/stream mixups at runtime, but C still cannot enforce the
  distinction at compile time.
- The WebTransport handler is called after `lsquic_wt_accept()` opens the
  session.  There is not yet an application-level pre-accept policy hook for
  origin/path validation or custom non-2xx rejection.
- Extra WebTransport CONNECT response headers are not exposed through the CWIST
  API yet.
- The existing `cwist_http3_send_datagram()` remains a raw QUIC datagram helper
  using the older single pending datagram slot.  WebTransport applications
  should use `cwist_webtransport_send_datagram()` instead.
- The WT stream read callback currently re-delivers the stream handle through
  the existing new-stream callback to notify the application.  A dedicated
  readable callback API would be cleaner for high-throughput WT stream handling.
- No end-to-end interop test is checked in yet.

## Next Work

Recommended next steps:

1. Add a real WebTransport integration test using a known client.
2. Add an application-level accept/reject callback before `lsquic_wt_accept()`.
3. Add typed CWIST wrapper structs for WT sessions and WT streams.
4. Add dedicated WT datagram receive and WT stream readable callbacks.
5. Add examples for echo over WT streams and unreliable datagrams.
6. Revisit `es_max_webtransport_sessions` when the lsquic branch supports more
   than one session per connection.
