#ifndef __CWIST_NET_HTTP_ASYNC_SERVER_H__
#define __CWIST_NET_HTTP_ASYNC_SERVER_H__

#include <cwist/sys/err/cwist_err.h>
#include <cwist/sys/app/app.h>

#ifdef __cplusplus
extern "C" {
#endif

cwist_error_t cwist_async_server_loop(int server_fd, cwist_app *app);

#ifdef __cplusplus
}
#endif

#endif
