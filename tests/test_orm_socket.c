#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <cwist/core/orm/orm_socket.h>

int main(void) {
    int sv[2] = {-1, -1};
    int res = cwist_orm_socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(res == 0);
    assert(sv[0] >= 0);
    assert(sv[1] >= 0);

    int flags0 = fcntl(sv[0], F_GETFD);
    assert(flags0 != -1);
    assert((flags0 & FD_CLOEXEC) != 0);

    int flags1 = fcntl(sv[1], F_GETFD);
    assert(flags1 != -1);
    assert((flags1 & FD_CLOEXEC) != 0);

    const char *msg = "test_payload";
    ssize_t w = write(sv[0], msg, strlen(msg));
    assert(w == (ssize_t)strlen(msg));

    char buf[32] = {0};
    ssize_t r = read(sv[1], buf, sizeof(buf));
    assert(r == (ssize_t)strlen(msg));
    assert(strcmp(buf, msg) == 0);

    close(sv[0]);
    close(sv[1]);

    printf("cwist_orm_socketpair_cloexec unit test passed.\n");
    return 0;
}
