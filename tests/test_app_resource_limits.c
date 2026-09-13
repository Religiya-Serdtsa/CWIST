#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

/* Use hooks for system errors. Use child processes for real file limits. */
static int test_getrlimit(int, struct rlimit *);
static int test_setrlimit(int, const struct rlimit *);
#define getrlimit test_getrlimit
#define setrlimit test_setrlimit
#undef _DEFAULT_SOURCE
#include "../src/sys/app/app.c"
#undef getrlimit
#undef setrlimit

#define TEST_REQUIRE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Check failed at %s:%d\n", __FILE__, __LINE__); \
        abort(); \
    } \
} while (0)

static bool mock_mode, read_error, write_error;
static struct rlimit current_limit;
static int reads, writes;

static int test_getrlimit(int resource, struct rlimit *out) {
    if (!mock_mode) return getrlimit(resource, out);
    TEST_REQUIRE(resource == RLIMIT_NOFILE);
    reads++;
    if (read_error) { errno = EIO; return -1; }
    *out = current_limit;
    return 0;
}

static int test_setrlimit(int resource, const struct rlimit *in) {
    if (!mock_mode) return setrlimit(resource, in);
    TEST_REQUIRE(resource == RLIMIT_NOFILE);
    writes++;
    if (write_error || in->rlim_max > current_limit.rlim_max) {
        errno = EPERM;
        return -1;
    }
    if (in->rlim_cur > in->rlim_max) { errno = EINVAL; return -1; }
    current_limit = *in;
    return 0;
}

static void check_mock(rlim_t soft, rlim_t hard, rlim_t want, int calls,
                       bool fail_read, bool fail_write) {
    mock_mode = true;
    read_error = fail_read;
    write_error = fail_write;
    reads = writes = 0;
    current_limit = (struct rlimit){soft, hard};
    cwist_app_tune_system();
    TEST_REQUIRE(reads == 1 && writes == calls);
    TEST_REQUIRE(current_limit.rlim_cur == want);
    TEST_REQUIRE(current_limit.rlim_max == hard);
}

/* CWIST_FD_LIMIT_TARGET override: same mocked getrlimit/setrlimit as
 * check_mock, plus the env var set for the duration of the call. NULL means
 * unset (falsy checks that clearing it restores default behavior). */
static void check_mock_env(const char *env_value, rlim_t soft, rlim_t hard,
                           rlim_t want, int calls) {
    if (env_value) {
        TEST_REQUIRE(setenv("CWIST_FD_LIMIT_TARGET", env_value, 1) == 0);
    } else {
        TEST_REQUIRE(unsetenv("CWIST_FD_LIMIT_TARGET") == 0);
    }
    check_mock(soft, hard, want, calls, false, false);
    TEST_REQUIRE(unsetenv("CWIST_FD_LIMIT_TARGET") == 0);
}

static void check_child(rlim_t soft, rlim_t hard) {
    pid_t pid = fork();
    TEST_REQUIRE(pid >= 0);
    if (pid == 0) {
        mock_mode = false;
        struct rlimit before, after;
        TEST_REQUIRE(getrlimit(RLIMIT_NOFILE, &before) == 0);
        if (hard > before.rlim_max) hard = before.rlim_max;
        if (soft > hard) soft = hard;
        struct rlimit small = {soft, hard};
        TEST_REQUIRE(setrlimit(RLIMIT_NOFILE, &small) == 0);
        cwist_app_tune_system();
        TEST_REQUIRE(getrlimit(RLIMIT_NOFILE, &after) == 0);
        if (after.rlim_cur != hard || after.rlim_max != hard) _exit(1);
        _exit(0);
    }
    int status;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    TEST_REQUIRE(waited == pid);
    TEST_REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void) {
    struct rlimit before, after;
    TEST_REQUIRE(getrlimit(RLIMIT_NOFILE, &before) == 0);
    check_child(64, 512);
    check_child(128, 2048);
    check_child(512, 512);
    check_mock(64, 512, 512, 1, false, false);
    check_mock(512, 512, 512, 0, false, false);
    check_mock(2100000, 4200000, 2100000, 0, false, false);
    check_mock(64, RLIM_INFINITY, 1050000, 1, false, false);
    check_mock(RLIM_INFINITY, RLIM_INFINITY, RLIM_INFINITY, 0, false, false);
    check_mock(0, 0, 0, 0, false, false);
    check_mock(64, 512, 64, 0, true, false);
    check_mock(64, 512, 64, 1, false, true);

    /* CWIST_FD_LIMIT_TARGET: valid override below the default, still capped
     * by the hard limit like the default target is. */
    check_mock_env("2000", 64, 1000000, 2000, 1);
    check_mock_env("2000", 64, 500, 500, 1);
    /* A target below the current soft limit is a no-op, same as the
     * default-target no-op case above. */
    check_mock_env("100", 2000, 500000, 2000, 0);
    /* Invalid values (unparsable, trailing garbage, zero, negative, empty)
     * all fall back to the untunable default rather than a bad rlim_t. */
    check_mock_env("not-a-number", 64, RLIM_INFINITY, 1050000, 1);
    check_mock_env("123abc", 64, RLIM_INFINITY, 1050000, 1);
    check_mock_env("0", 64, RLIM_INFINITY, 1050000, 1);
    check_mock_env("-5", 64, RLIM_INFINITY, 1050000, 1);
    check_mock_env("", 64, RLIM_INFINITY, 1050000, 1);
    /* Unset behaves identically to the pre-existing default-target cases. */
    check_mock_env(NULL, 64, RLIM_INFINITY, 1050000, 1);

    TEST_REQUIRE(getrlimit(RLIMIT_NOFILE, &after) == 0);
    TEST_REQUIRE(before.rlim_cur == after.rlim_cur && before.rlim_max == after.rlim_max);
    puts("Resource limit tests passed.");
    return 0;
}