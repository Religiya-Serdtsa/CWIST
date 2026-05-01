#define _GNU_SOURCE
#include <cwist/net/nats/cwist_nats.h>
#include <cwist/core/mem/alloc.h>
#include <nats.h>
#include <stdlib.h>
#include <string.h>

struct cwist_nats {
    natsConnection *conn;
    natsSubscription *sub;
    cwist_nats_msg_cb user_cb;
    void *user_ctx;
};

static void cwist_nats_adapter(natsConnection *nc, natsSubscription *sub, natsMsg *msg, void *closure) {
    (void)nc;
    (void)sub;
    cwist_nats_t *nats = (cwist_nats_t *)closure;
    if (nats && nats->user_cb) {
        const char *subject = natsMsg_GetSubject(msg);
        const char *data = natsMsg_GetData(msg);
        int len = natsMsg_GetDataLength(msg);
        nats->user_cb(subject, data, (size_t)len, nats->user_ctx);
    }
    natsMsg_Destroy(msg);
}

cwist_error_t cwist_nats_connect(cwist_nats_t **nats, const char *url) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!nats) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }
    cwist_nats_t *obj = (cwist_nats_t *)cwist_alloc(sizeof(cwist_nats_t));
    if (!obj) {
        err.error.err_i16 = CWIST_ERROR_NOMEM;
        return err;
    }
    memset(obj, 0, sizeof(*obj));
    natsStatus s = natsConnection_ConnectTo(&obj->conn, url ? url : NATS_DEFAULT_URL);
    if (s != NATS_OK) {
        cwist_free(obj);
        err.error.err_i16 = (int16_t)s;
        return err;
    }
    *nats = obj;
    err.error.err_i16 = 0;
    return err;
}

cwist_error_t cwist_nats_subscribe(cwist_nats_t *nats, const char *subject, cwist_nats_msg_cb cb, void *ctx) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!nats || !nats->conn || !subject) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }
    if (nats->sub) {
        natsSubscription_Destroy(nats->sub);
        nats->sub = NULL;
    }
    nats->user_cb = cb;
    nats->user_ctx = ctx;
    natsStatus s = natsConnection_Subscribe(&nats->sub, nats->conn, subject, cwist_nats_adapter, nats);
    if (s != NATS_OK) {
        err.error.err_i16 = (int16_t)s;
        return err;
    }
    err.error.err_i16 = 0;
    return err;
}

cwist_error_t cwist_nats_publish_string(cwist_nats_t *nats, const char *subject, const char *data) {
    cwist_error_t err = make_error(CWIST_ERR_INT16);
    if (!nats || !nats->conn || !subject) {
        err.error.err_i16 = CWIST_ERROR_INVALID_PARAM;
        return err;
    }
    natsStatus s = natsConnection_PublishString(nats->conn, subject, data ? data : "");
    if (s == NATS_OK) {
        s = natsConnection_Flush(nats->conn);
    }
    err.error.err_i16 = (int16_t)s;
    return err;
}

void cwist_nats_dispatch(cwist_nats_t *nats) {
    if (nats && nats->conn) {
        natsConnection_Flush(nats->conn);
    }
}

void cwist_nats_destroy(cwist_nats_t *nats) {
    if (!nats) return;
    if (nats->sub) {
        natsSubscription_Destroy(nats->sub);
    }
    if (nats->conn) {
        natsConnection_Destroy(nats->conn);
    }
    cwist_free(nats);
}
