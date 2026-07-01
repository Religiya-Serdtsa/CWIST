/**
 * @file openapi.h
 * @brief Source-level OpenAPI annotations consumed by `cwist openapi`.
 *
 * Put Doxygen tags immediately before a CWIST_OPENAPI_* route declaration:
 * @code
 * /** @openapi.summary List users
 *     @openapi.description Returns all active users.
 *     @openapi.tags users,public
 *     @openapi.response 200 application/json User collection. *\/
 * CWIST_OPENAPI_GET(app, "/users", list_users);
 * @endcode
 */
#ifndef CWIST_OPENAPI_H
#define CWIST_OPENAPI_H

#include <cwist/sys/app/app.h>

/* These route declarations preserve normal runtime routing while leaving an
 * unambiguous, parseable declaration in C source for documentation tooling. */
#define CWIST_OPENAPI_GET(app, path, handler) cwist_app_get((app), (path), (handler))
#define CWIST_OPENAPI_POST(app, path, handler) cwist_app_post((app), (path), (handler))
#define CWIST_OPENAPI_PUT(app, path, handler) cwist_app_put((app), (path), (handler))
#define CWIST_OPENAPI_DELETE(app, path, handler) cwist_app_delete((app), (path), (handler))
#define CWIST_OPENAPI_PATCH(app, path, handler) cwist_app_patch((app), (path), (handler))

#endif
