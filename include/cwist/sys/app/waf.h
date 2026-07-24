/** @file waf.h @brief Deterministic request firewall and output sanitization. */
#ifndef __CWIST_WAF_H__
#define __CWIST_WAF_H__

#include <stdbool.h>
#include <stddef.h>
#include <cwist/sys/app/app.h>

#ifdef __cplusplus
extern "C" {
#endif

cwist_middleware_func cwist_mw_waf_lite(void);
bool cwist_waf_is_safe(const char *input, size_t length);
char *cwist_sanitize_html(const char *input);

#ifdef __cplusplus
}
#endif
#endif
