#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cwist/core/orm/orm_socket.h>

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

static void set_cloexec(int fd) {
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFD);
        if (flags != -1) {
            fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
        }
    }
}

int cwist_orm_socketpair_cloexec(int domain, int type, int protocol, int sv[2]) {
    int res = -1;
#if defined(SOCK_CLOEXEC) && SOCK_CLOEXEC != 0
    res = socketpair(domain, type | SOCK_CLOEXEC, protocol, sv);
#endif
    if (res != 0) {
        res = socketpair(domain, type & ~SOCK_CLOEXEC, protocol, sv);
        if (res == 0) {
            set_cloexec(sv[0]);
            set_cloexec(sv[1]);
        }
    } else {
        set_cloexec(sv[0]);
        set_cloexec(sv[1]);
    }
    return res;
}
