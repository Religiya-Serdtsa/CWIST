/**
 * @file shutdown.c
 * @brief Graceful shutdown implementation.
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/sys/app/shutdown.h>
#include <unistd.h>
#include <stdio.h>

atomic_int g_cwist_running = 1;
int g_cwist_listen_fd = -1;
int g_cwist_udp_fd = -1;
int g_cwist_drain_timeout_sec = 5;

static void cwist_shutdown_handler(int sig) {
    (void)sig;
    atomic_store(&g_cwist_running, 0);
    int fd = g_cwist_listen_fd;
    if (fd >= 0) {
        close(fd);
    }
    int udp = g_cwist_udp_fd;
    if (udp >= 0) {
        close(udp);
    }
}

void cwist_shutdown_install_handlers(void) {
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = cwist_shutdown_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
}

void cwist_shutdown_reset(void) {
    atomic_store(&g_cwist_running, 1);
    g_cwist_listen_fd = -1;
    g_cwist_udp_fd = -1;
}
