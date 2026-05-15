/**
 * @file orm_socket.h
 * @brief SQLite-to-Unix-Socket bridge helpers.
 *
 * This module isolates SQLite3 behind a local Unix socket stream so that
 * the ORM layer never directly references sqlite3.h.  A background worker
 * opens the real database and speaks a tiny binary protocol over the
 * socket pair.
 */

#ifndef __CWIST_ORM_SOCKET_H__
#define __CWIST_ORM_SOCKET_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Spawn a background SQLite worker attached to a Unix socket pair.
 *
 * Internally creates an @c AF_UNIX @c SOCK_STREAM socket pair, opens
 * @p db_path with SQLite3 in a detached worker thread, and returns the
 * client-side file descriptor.  Every SQL statement sent to this fd is
 * executed by the worker and results are streamed back.
 *
 * @param db_path Path to the SQLite database file, or @c ":memory:".
 * @return A valid socket fd on success, or @c -1 if the socket pair
 *         could not be created or the worker thread could not be spawned.
 *
 * @note The caller owns the returned fd and must close it (preferably via
 *       cwist_orm_close_socket()) when done.
 * @warning The background thread is detached; there is no explicit
 *          join point.  Closing the client fd causes the worker to
 *          shut down gracefully on the next read failure.
 */
int cwist_db_transfer_sqlite_to_socket(const char *db_path);

#ifdef __cplusplus
}
#endif

#endif /* __CWIST_ORM_SOCKET_H__ */
