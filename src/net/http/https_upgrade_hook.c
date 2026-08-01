/* Default no-op implementation of the optional HTTPS upgrade hook.
 *
 * Applications that need to take ownership of a TLS-upgraded connection
 * (e.g. reverse-session hijacking) provide their own strong definition of
 * cwist_https_upgrade_handler(). This stub lives in its own translation
 * unit so the static linker only extracts it from libcwist.a when the
 * application does not define the symbol; the weak attribute additionally
 * keeps --whole-archive style links from failing with a duplicate
 * definition (both ELF and Mach-O support weak definitions). This is more
 * portable than a weak *declaration*: Mach-O has no ELF-style weak
 * undefined symbols, and weak_import did not satisfy the Darwin linker.
 *
 * Return true to detach the fd/ssl from cwist so they are not closed after
 * the response is sent. The default returns false (keep cwist ownership).
 */
#include <cwist/net/http/https.h>

__attribute__((weak)) bool cwist_https_upgrade_handler(cwist_https_connection *conn, cwist_http_request *req, cwist_http_response *res) {
    (void)conn;
    (void)req;
    (void)res;
    return false;
}
