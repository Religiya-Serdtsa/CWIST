# HTTPS API

*Header:* `<cwist/net/http/https.h>`

Secure transport layer utilizing BoringSSL with optional post-quantum cryptography (PQC).

### `cwist_https_init_context`
```c
cwist_error_t cwist_https_init_context(cwist_https_context **ctx, const char *cert_path, const char *key_path);
```
Initializes the TLS context and loads certificates.
The context enforces TLS 1.3+ and disables TLS-level compression.
When the PQC layer is enabled via `cwist_app_use_pqc_layer()`, the context additionally restricts key exchange to the hybrid group `X25519MLKEM768:X25519:P-256`, combining the NIST-standard ML-KEM-768 (Kyber) with classical X25519 ECDH for quantum-resistant transport.

### `cwist_https_init_context_with_options`
```c
cwist_error_t cwist_https_init_context_with_options(cwist_https_context **ctx,
                                                    const char *cert_path,
                                                    const char *key_path,
                                                    const cwist_https_options *options);
```
Builds the TLS context with explicit transport options.
If `options->enable_http2` is true, CWIST merges HTTP/2-oriented TLS defaults such as ALPN negotiation and stricter cipher preferences into the legacy HTTPS setup.

### `cwist_https_accept`
```c
cwist_error_t cwist_https_accept(cwist_https_context *ctx, int client_fd, cwist_https_connection **conn);
```
Performs the SSL handshake on an accepted TCP socket.

### `cwist_https_send_response`
```c
cwist_error_t cwist_https_send_response(cwist_https_connection *conn, cwist_http_response *res);
```
Serializes and encrypts the response, sending it to the client.

### `cwist_https_connection_uses_http2`
```c
bool cwist_https_connection_uses_http2(const cwist_https_connection *conn);
```
Returns whether ALPN negotiated `h2` on the TLS connection.

### `cwist_http2_serve_connection`
```c
cwist_error_t cwist_http2_serve_connection(cwist_https_connection *conn,
                                           void *user_ctx,
                                           cwist_http2_request_handler_func handler);
```
Consumes the HTTP/2 client preface and frame stream, decodes request headers/body, invokes the app handler, and emits the response as HTTP/2 `HEADERS`/`DATA` frames.
