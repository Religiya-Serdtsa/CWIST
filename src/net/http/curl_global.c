#define _POSIX_C_SOURCE 200809L

#include "curl_global.h"

#include <curl/curl.h>
#include <pthread.h>

static pthread_mutex_t g_curl_global_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_curl_global_ref = 0;

void cwist_curl_global_acquire(void) {
    pthread_mutex_lock(&g_curl_global_mtx);
    if (g_curl_global_ref == 0) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    g_curl_global_ref++;
    pthread_mutex_unlock(&g_curl_global_mtx);
}

void cwist_curl_global_release(void) {
    pthread_mutex_lock(&g_curl_global_mtx);
    if (g_curl_global_ref > 0) {
        g_curl_global_ref--;
        if (g_curl_global_ref == 0) {
            curl_global_cleanup();
        }
    }
    pthread_mutex_unlock(&g_curl_global_mtx);
}
