#include <cwist/app.h>
#include <cwist/net/http/session.h>

static void handle_login(cwist_http_request *req, cwist_http_response *res) {
    cwist_session_t *sess = cwist_session_start(NULL, req, res);
    if (sess) {
        cwist_session_set(sess, "user_id", "42");
        cwist_session_set(sess, "username", "alice");
        cwist_session_commit(sess, res);
        cwist_session_destroy(sess);
    }
    cwist_sstring_assign(res->body, "{\"status\":\"logged_in\",\"user_id\":\"42\"}");
}

static void handle_profile(cwist_http_request *req, cwist_http_response *res) {
    cwist_session_t *sess = cwist_session_start(NULL, req, res);
    const char *username = sess ? cwist_session_get(sess, "username") : NULL;
    char body[256];
    if (username) {
        snprintf(body, sizeof(body), "{\"status\":\"ok\",\"user\":\"%s\"}", username);
    } else {
        snprintf(body, sizeof(body), "{\"status\":\"error\",\"message\":\"unauthorized\"}");
    }
    if (sess) cwist_session_destroy(sess);
    cwist_sstring_assign(res->body, body);
}

int main(void) {
    cwist_app *app = cwist_app_create();
    cwist_app_use_session(app, "super-secret-session-key-32bytes!!");
    cwist_app_post(app, "/login", handle_login);
    cwist_app_get(app, "/profile", handle_profile);
    cwist_app_listen(app, 8080);
    cwist_app_destroy(app);
    return 0;
}
