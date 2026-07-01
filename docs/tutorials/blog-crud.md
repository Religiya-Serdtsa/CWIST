# Build a CRUD blog with CWIST

This guide builds a small JSON blog API backed by CWIST's SQLite integration.
It intentionally keeps SQL explicit so that the request, validation, and
response boundaries are easy to inspect.

## 1. Create the application

```c
#include <cwist/sys/app/app.h>
#include <cwist/core/db/sql.h>
#include <cwist/net/http/http.h>

static void list_posts(cwist_http_request *req, cwist_http_response *res) {
    (void)req;
    /* Query through your existing cwist_db handle and serialize rows as JSON. */
    cwist_sstring_assign(res->body, "[]");
    cwist_http_header_add(&res->headers, "Content-Type", "application/json");
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use_db(app, "blog.db");
    cwist_app_get(app, "/posts", list_posts);
    int rc = cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return rc;
}
```

Create the table during application startup with a migration:

```sql
CREATE TABLE IF NOT EXISTS posts (
  id INTEGER PRIMARY KEY,
  title TEXT NOT NULL,
  body TEXT NOT NULL,
  created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
```

## 2. Define the routes

Use one handler per operation and parameterized routes for a post identifier.

| Method | Path | Action |
| --- | --- | --- |
| GET | `/posts` | List posts |
| GET | `/posts/:id` | Read one post |
| POST | `/posts` | Create a post |
| PATCH | `/posts/:id` | Update title/body |
| DELETE | `/posts/:id` | Delete a post |

```c
cwist_app_get(app, "/posts", list_posts);
cwist_app_get(app, "/posts/:id", get_post);
cwist_app_post(app, "/posts", create_post);
cwist_app_patch(app, "/posts/:id", update_post);
cwist_app_delete(app, "/posts/:id", delete_post);
```

Read the route parameter with `cwist_query_map_get(req->path_params, "id")`.
Treat it as untrusted input: parse the whole integer, reject non-positive values,
and bind it to a prepared statement. Never interpolate JSON fields or route
parameters into SQL.

## 3. Validate JSON and return HTTP semantics

Require `title` and `body` strings on create. Return `201 Created` after an
insert, `204 No Content` after a successful delete, `400 Bad Request` for a
malformed request, and `404 Not Found` when an id has no row. Always set
`Content-Type: application/json` for JSON responses.

For browser clients, add CORS only for the origins you control and use CSRF
middleware when cookie authentication is enabled. The existing WAF middleware
is a useful additional boundary, but it does not replace parameterized SQL.

## 4. Test the API

Use `cwist_test_client` to dispatch requests without binding a port. Cover the
entire lifecycle: create, list, fetch, patch, delete, then fetch again and
assert 404. Run the same tests against `:memory:` for fast isolation and a
temporary on-disk database to exercise migrations.
