#include <cwist/sys/err/cwist_err.h>

/**
 * @file error.c
 * @brief Minimal constructors for the framework's tagged error container.
 */

/**
 * @brief Initialize a cwist_error_t with the requested storage variant.
 * @param type Error payload discriminator to store in the result.
 * @return Error value with @p errtype set and the payload left for the caller to fill.
 */
cwist_error_t make_error(cwist_errtype_t type) {
    cwist_error_t err;
    err.errtype = type;
    return err;
}

void cwist_error_dispose(cwist_error_t *err) {
    if (!err) return;
    if (err->errtype == CWIST_ERR_JSON && err->error.err_json) {
        cJSON_Delete(err->error.err_json);
        err->error.err_json = NULL;
    }
}
