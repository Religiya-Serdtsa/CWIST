/**
 * @file shutdown.h
 * @brief Unified Graceful Shutdown support for CWIST.
 *
 * Provides a global atomic 'running' flag and signal handling for SIGTERM/SIGINT.
 * All protocol server loops (HTTP/1.1, HTTP/2, HTTP/3) check this flag.
 */

#ifndef __CWIST_SHUTDOWN_H__
#define __CWIST_SHUTDOWN_H__

#include <stdatomic.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Global running flag. Checked by all server event loops. */
extern atomic_int g_cwist_running;

/** @brief Registered TCP listening socket (for signal-handler closure). */
extern int g_cwist_listen_fd;

/** @brief Registered UDP socket for HTTP/3 (for signal-handler closure). */
extern int g_cwist_udp_fd;

/** @brief Drain timeout in seconds before process exits. */
extern int g_cwist_drain_timeout_sec;

/**
 * @brief Install SIGTERM and SIGINT handlers for graceful shutdown.
 */
void cwist_shutdown_install_handlers(void);

/**
 * @brief Reset shutdown state (useful in test suites).
 */
void cwist_shutdown_reset(void);

#ifdef __cplusplus
}
#endif

#endif
