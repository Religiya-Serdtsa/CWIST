# HTTPS API

*Header:* `<cwist/net/http/https.h>`

Secure transport layer utilizing OpenSSL.

### `cwist_https_init_context`
```c
cwist_error_t cwist_https_init_context(cwist_https_context **ctx, const char *cert_path, const char *key_path);
```
Initializes OpenSSL context and loads certificates.
The context enforces TLS 1.2+ and disables TLS-level compression.

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
