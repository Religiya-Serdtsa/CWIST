#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

int cwist_orm_socketpair(int sv[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == -1) {
        return -1;
    }
#if SOCK_CLOEXEC == 0
    int flags = fcntl(sv[0], F_GETFD);
    if (flags != -1) {
        fcntl(sv[0], F_SETFD, flags | FD_CLOEXEC);
    }
    flags = fcntl(sv[1], F_GETFD);
    if (flags != -1) {
        fcntl(sv[1], F_SETFD, flags | FD_CLOEXEC);
    }
#endif
    return 0;
}
