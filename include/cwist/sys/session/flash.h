/**
 * @file flash.h
 * @brief One-time flash messages (ala Flask flash/get_flashed_messages).
 */

#ifndef __CWIST_FLASH_H__
#define __CWIST_FLASH_H__

#include <cwist/net/http/http.h>

/**
 * @brief Set a flash message on the request.
 */
void cwist_flash_set(cwist_http_request *req, const char *key, const char *value);

/**
 * @brief Get a flash message and consume it (one-time read).
 * @return The message value, or NULL if not found.
 */
const char *cwist_flash_get(cwist_http_request *req, const char *key);

/**
 * @brief Peek at a flash message without consuming it.
 * @return The message value, or NULL if not found.
 */
const char *cwist_flash_peek(cwist_http_request *req, const char *key);

/**
 * @brief Serialize all flash messages to a JSON object string.
 * Caller must free the returned string.
 * @return JSON string like {"key":"value",...} or NULL if empty.
 */
char *cwist_flash_pop_all_json(cwist_http_request *req);

#endif
