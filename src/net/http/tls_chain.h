#ifndef CWIST_NET_HTTP_TLS_CHAIN_H
#define CWIST_NET_HTTP_TLS_CHAIN_H

#include <openssl/ssl.h>

int cwist_tls_autoload_intermediates(SSL_CTX *ssl_ctx);

#endif /* CWIST_NET_HTTP_TLS_CHAIN_H */
