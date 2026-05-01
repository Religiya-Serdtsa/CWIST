#ifndef __CWIST_ECH_H__
#define __CWIST_ECH_H__

#include <cwist/sys/app/app.h>
#include <cwist/sys/err/cwist_err.h>

#ifdef __cplusplus
extern "C" {
#endif

cwist_error_t cwist_app_use_ech(cwist_app *app, const char *ech_key, const char *ech_dir);

#ifdef __cplusplus
}
#endif

#endif
