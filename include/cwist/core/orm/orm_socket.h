#ifndef CWIST_CORE_ORM_SOCKET_H
#define CWIST_CORE_ORM_SOCKET_H

#include <cwist/core/orm/orm.h>

#ifdef __cplusplus
extern "C" {
#endif

int cwist_db_transfer_sqlite_to_socket(const char *path);
cwist_orm_t *cwist_orm_open_socket(int sock);
void cwist_orm_close_socket(cwist_orm_t *orm);

int cwist_orm_socketpair_cloexec(int domain, int type, int protocol, int sv[2]);

#ifdef __cplusplus
}
#endif

#endif /* CWIST_CORE_ORM_SOCKET_H */
