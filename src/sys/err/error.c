#include <cwist/sys/err/cwist_err.h>

cwist_error_t make_error(cwist_errtype_t type) {
    cwist_error_t err;
    err.errtype = type;
    return err;
}
