/**
 * @file test_shutdown.c
 * @brief Test graceful shutdown via SIGTERM/SIGINT.
 */

#include <cwist/sys/app/app.h>
#include <cwist/sys/app/shutdown.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>

static void hello_handler(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    cwist_sstring_assign(res->body, "hello");
    res->status_code = CWIST_HTTP_OK;
}

int main(void) {
    printf("Testing graceful shutdown...\n");

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* Child: run server */
        cwist_app *app = cwist_app_create();
        if (!app) {
            fprintf(stderr, "Failed to create app\n");
            _exit(1);
        }
        cwist_app_get(app, "/", hello_handler);
        g_cwist_drain_timeout_sec = 1;
        /* Use a high port to avoid conflicts */
        int rc = cwist_app_listen(app, 19999);
        cwist_app_destroy(app);
        _exit(rc == 0 ? 0 : 1);
    }

    /* Parent: give server time to start */
    usleep(300000);

    /* Send SIGTERM */
    if (kill(pid, SIGTERM) < 0) {
        perror("kill");
        kill(pid, SIGKILL);
        return 1;
    }

    /* Wait up to 5 seconds for child to exit */
    int status;
    int waited = 0;
    while (waited < 50) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        usleep(100000);
        waited++;
    }

    if (waited >= 50) {
        fprintf(stderr, "Server did not exit in time, sending SIGKILL\n");
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return 1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("Passed graceful shutdown test.\n");
        return 0;
    }

    fprintf(stderr, "Server exited abnormally (status=%d)\n", status);
    return 1;
}
