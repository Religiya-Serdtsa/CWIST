/**
 * @file grpc_channel.c
 * @brief gRPC client channel: name resolution (doc/naming.md), client-side
 *        load balancing (doc/load-balancing.md), connection backoff
 *        (doc/connection-backoff.md), and the gRFC A6 retry engine.
 *
 * Layout mirrors gRPC: channel -> resolver -> subchannels (one per backend
 * address) -> LB policy -> per-call pick.  Retries live between the channel
 * and the LB policy so every attempt can use a different subchannel (A6).
 *
 * Retry engine notes (all gRFC A6):
 * - The call deadline spans every attempt; each attempt's grpc-timeout
 *   header carries the remaining time.
 * - A failed LB pick (TRANSIENT_FAILURE) is handled by the configured retry
 *   policy; with wait_for_ready the call waits instead of failing.
 * - An RPC that never left the client is transparently retried until the
 *   deadline; an RPC refused before reaching server application logic
 *   (RST REFUSED_STREAM or GOAWAY last-stream-id below ours) gets one
 *   immediate transparent retry, then the configured policy.  Transparent
 *   retries never count against maxAttempts nor the retry throttle.
 * - Once Response-Headers arrive the RPC is committed and no retry happens.
 * - Throttle bookkeeping: every failed attempt with a retryable-coded
 *   status (or a do-not-retry pushback) costs one token; a successful call
 *   refunds token_ratio once.  Transparent retries are not counted.
 */

#define _POSIX_C_SOURCE 200809L
#include <cwist/net/grpc/grpc_channel.h>
#include <cwist/core/mem/alloc.h>
#include <cjson/cJSON.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define GRPC_CHANNEL_MAX_ADDRS 64
#define GRPC_CHANNEL_DEFAULT_BUFFER_LIMIT (256u * 1024u)
#define GRPC_CHANNEL_MAX_ATTEMPTS_CAP 5

/* doc/connection-backoff.md parameters */
#define GRPC_CHANNEL_CONN_BACKOFF_INITIAL_MS 1000.0
#define GRPC_CHANNEL_CONN_BACKOFF_MULTIPLIER 1.6
#define GRPC_CHANNEL_CONN_BACKOFF_MAX_MS 120000.0
#define GRPC_CHANNEL_CONN_BACKOFF_JITTER 0.2

typedef struct cwist_grpc_subchannel {
    char *host;                     /* literal/numeric address to dial */
    uint16_t port;
    cwist_grpc_client *client;      /* NULL when not connected */
    int inflight;                   /* calls handed out */
    uint64_t next_connect_at;       /* monotonic ms; 0 = dialable now */
    double backoff_ms;              /* current connection backoff */
} cwist_grpc_subchannel;

typedef struct grpc_method_config {
    char *service;                  /* "" matches every service */
    char *method;                   /* NULL matches every method of the service */
    int has_policy;
    cwist_grpc_retry_policy policy;
    int has_wfr;
    int wait_for_ready;
    int has_timeout;
    uint64_t timeout_ms;
    struct grpc_method_config *next;
} grpc_method_config;

struct cwist_grpc_channel {
    pthread_mutex_t mu;             /* subchannels, LB state, RNG */
    pthread_mutex_t tmu;            /* throttle token count */
    cwist_grpc_subchannel *subs;
    size_t nsubs;
    char *authority;                /* :authority for every subchannel */
    char *tls_name;                 /* SNI hostname (dns name, not the IP) */
    cwist_grpc_client_options copt; /* base transport options */
    cwist_grpc_lb_policy lb;
    size_t rr_next;                 /* round_robin cursor */
    size_t pf_index;                /* pick_first sticky index */
    int has_policy;
    cwist_grpc_retry_policy policy;
    int has_throttle;
    cwist_grpc_retry_throttle throttle;
    double tokens;
    uint64_t buffer_limit;
    int wait_for_ready;
    uint64_t rng;
    grpc_method_config *mcs;        /* JSON service config method configs */
};

struct cwist_grpc_channel_call {
    cwist_grpc_channel *ch;
    cwist_grpc_subchannel *sub;     /* NULL for synthetic (pre-dispatch) results */
    cwist_grpc_call *call;          /* NULL for synthetic results */
    uint32_t attempts;              /* policy attempts (transparent excluded) */
    int accounted;                  /* throttle end-of-call bookkeeping done */
    uint32_t acct_mask;             /* retryable mask for end-of-call accounting */
    int acct_has_policy;
    cwist_grpc_status_t syn_status; /* synthetic result (call == NULL) */
    char *syn_message;              /* owned */
};

static uint64_t channel_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void channel_sleep_ms(uint64_t ms) {
    if (!ms) return;
    struct timespec ts = { (time_t)(ms / 1000), (long)((ms % 1000) * 1000000L) };
    nanosleep(&ts, NULL);
}

static uint64_t channel_rand(cwist_grpc_channel *ch) {
    uint64_t x = ch->rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    ch->rng = x;
    return x;
}

/* Uniform in [0.8, 1.2): the gRFC A6 / connection-backoff jitter factor. */
static double channel_jitter(cwist_grpc_channel *ch) {
    pthread_mutex_lock(&ch->mu);
    double u = (double)(channel_rand(ch) >> 11) * (1.0 / 9007199254740992.0);
    pthread_mutex_unlock(&ch->mu);
    return 0.8 + 0.4 * u;
}

/* --- throttling (gRFC A6 retryThrottling) --- */

static int channel_throttle_allows(cwist_grpc_channel *ch) {
    if (!ch->has_throttle) return 1;
    pthread_mutex_lock(&ch->tmu);
    int ok = ch->tokens > ch->throttle.max_tokens / 2.0;
    pthread_mutex_unlock(&ch->tmu);
    return ok;
}

static void channel_throttle_failure(cwist_grpc_channel *ch) {
    if (!ch->has_throttle) return;
    pthread_mutex_lock(&ch->tmu);
    ch->tokens -= 1.0;
    if (ch->tokens < 0) ch->tokens = 0;
    pthread_mutex_unlock(&ch->tmu);
}

static void channel_throttle_success(cwist_grpc_channel *ch) {
    if (!ch->has_throttle) return;
    pthread_mutex_lock(&ch->tmu);
    ch->tokens += ch->throttle.token_ratio;
    if (ch->tokens > ch->throttle.max_tokens) ch->tokens = ch->throttle.max_tokens;
    pthread_mutex_unlock(&ch->tmu);
}

static void channel_account(cwist_grpc_channel *ch, cwist_grpc_status_t status,
                            int has_policy, uint32_t mask) {
    if (status == CWIST_GRPC_OK) {
        channel_throttle_success(ch);
    } else if (has_policy && (mask & CWIST_GRPC_STATUS_BIT(status))) {
        channel_throttle_failure(ch);
    }
}

/* --- subchannels --- */

/* Close a dead/GOAWAY connection so the next acquire dials fresh. */
static void channel_recycle_locked(cwist_grpc_channel *ch, cwist_grpc_subchannel *sub) {
    (void)ch;
    if (sub->client && cwist_grpc_client_dead(sub->client)) {
        cwist_grpc_client_close(sub->client);
        sub->client = NULL;
    }
}

/* Dial unless connected; applies doc/connection-backoff.md between failures.
 * Caller holds ch->mu. */
static int sub_ensure_connected(cwist_grpc_channel *ch, cwist_grpc_subchannel *sub) {
    channel_recycle_locked(ch, sub);
    if (sub->client) return 0;
    uint64_t now = channel_now_ms();
    if (sub->next_connect_at && now < sub->next_connect_at) return -1;
    cwist_grpc_client_options o = ch->copt;
    o.authority = ch->authority;
    o.tls_server_name = ch->tls_name;
    cwist_grpc_client *client = cwist_grpc_client_connect(sub->host, sub->port, &o);
    if (!client) {
        double jitter = 1.0 - GRPC_CHANNEL_CONN_BACKOFF_JITTER +
                        2.0 * GRPC_CHANNEL_CONN_BACKOFF_JITTER *
                            ((double)(channel_rand(ch) >> 11) * (1.0 / 9007199254740992.0));
        sub->next_connect_at = now + (uint64_t)(sub->backoff_ms * jitter);
        sub->backoff_ms *= GRPC_CHANNEL_CONN_BACKOFF_MULTIPLIER;
        if (sub->backoff_ms > GRPC_CHANNEL_CONN_BACKOFF_MAX_MS)
            sub->backoff_ms = GRPC_CHANNEL_CONN_BACKOFF_MAX_MS;
        return -1;
    }
    /* The SETTINGS handshake completed: reset the backoff (connection-backoff.md). */
    sub->client = client;
    sub->backoff_ms = GRPC_CHANNEL_CONN_BACKOFF_INITIAL_MS;
    sub->next_connect_at = 0;
    return 0;
}

/* Caller holds ch->mu. */
static cwist_grpc_subchannel *channel_pick_locked(cwist_grpc_channel *ch) {
    if (ch->lb == CWIST_GRPC_LB_ROUND_ROBIN) {
        for (size_t k = 0; k < ch->nsubs; k++) {
            size_t i = (ch->rr_next + k) % ch->nsubs;
            cwist_grpc_subchannel *s = &ch->subs[i];
            if (s->client && !s->inflight && !cwist_grpc_client_dead(s->client)) {
                ch->rr_next = (i + 1) % ch->nsubs;
                return s;
            }
        }
        return NULL;
    }
    /* pick_first: stick to the connected subchannel (doc/load-balancing.md). */
    if (ch->pf_index < ch->nsubs) {
        cwist_grpc_subchannel *s = &ch->subs[ch->pf_index];
        if (s->client && !s->inflight && !cwist_grpc_client_dead(s->client))
            return s;
    }
    return NULL;
}

/* Acquire a READY subchannel (connected, idle), dialing on demand per LB
 * policy.  On success sub->inflight is incremented; the caller returns it
 * with channel_release().  NULL = TRANSIENT_FAILURE right now. */
static cwist_grpc_subchannel *channel_acquire(cwist_grpc_channel *ch) {
    pthread_mutex_lock(&ch->mu);
    cwist_grpc_subchannel *s = channel_pick_locked(ch);
    if (s) {
        s->inflight++;
        pthread_mutex_unlock(&ch->mu);
        return s;
    }
    if (ch->lb == CWIST_GRPC_LB_ROUND_ROBIN) {
        for (size_t k = 0; k < ch->nsubs; k++) {
            size_t i = (ch->rr_next + k) % ch->nsubs;
            cwist_grpc_subchannel *c = &ch->subs[i];
            if (c->inflight) continue;
            if (sub_ensure_connected(ch, c) == 0) {
                ch->rr_next = (i + 1) % ch->nsubs;
                c->inflight++;
                pthread_mutex_unlock(&ch->mu);
                return c;
            }
        }
    } else {
        /* pick_first: dial the addresses one at a time, in order. */
        for (size_t i = 0; i < ch->nsubs; i++) {
            cwist_grpc_subchannel *c = &ch->subs[i];
            if (c->inflight) continue;
            if (sub_ensure_connected(ch, c) == 0) {
                ch->pf_index = i;
                c->inflight++;
                pthread_mutex_unlock(&ch->mu);
                return c;
            }
        }
    }
    pthread_mutex_unlock(&ch->mu);
    return NULL;
}

static void channel_release(cwist_grpc_channel *ch, cwist_grpc_subchannel *sub) {
    pthread_mutex_lock(&ch->mu);
    if (sub->inflight > 0) sub->inflight--;
    pthread_mutex_unlock(&ch->mu);
}

static void channel_recycle(cwist_grpc_channel *ch, cwist_grpc_subchannel *sub) {
    pthread_mutex_lock(&ch->mu);
    channel_recycle_locked(ch, sub);
    pthread_mutex_unlock(&ch->mu);
}

/* --- method configs (JSON service config) --- */

/* "/package.Service/Method" -> service/method slices (borrowed). */
static void channel_split_method(const char *path, const char **service, size_t *service_len,
                                 const char **method) {
    const char *start = path;
    if (*start == '/') start++;
    const char *slash = strrchr(start, '/');
    if (!slash) {
        *service = start;
        *service_len = strlen(start);
        *method = NULL;
        return;
    }
    *service = start;
    *service_len = (size_t)(slash - start);
    *method = slash + 1;
}

/* Most specific match wins: service+method > service > wildcard. */
static const grpc_method_config *channel_find_mc(cwist_grpc_channel *ch, const char *path) {
    const char *service, *method;
    size_t service_len;
    channel_split_method(path, &service, &service_len, &method);
    const grpc_method_config *service_match = NULL, *wild_match = NULL;
    for (const grpc_method_config *mc = ch->mcs; mc; mc = mc->next) {
        if (mc->service[0] == '\0') {
            if (!wild_match) wild_match = mc;
            continue;
        }
        if (strlen(mc->service) != service_len ||
            memcmp(mc->service, service, service_len) != 0)
            continue;
        if (!mc->method) {
            if (!service_match) service_match = mc;
            continue;
        }
        if (method && strcmp(mc->method, method) == 0) return mc; /* exact */
    }
    return service_match ? service_match : wild_match;
}

static void channel_free_mcs(cwist_grpc_channel *ch) {
    grpc_method_config *mc = ch->mcs;
    while (mc) {
        grpc_method_config *next = mc->next;
        cwist_free(mc->service);
        cwist_free(mc->method);
        cwist_free(mc);
        mc = next;
    }
    ch->mcs = NULL;
}

/* --- synthetic (pre-dispatch) call results --- */

static char *channel_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *copy = cwist_alloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

static cwist_grpc_channel_call *channel_synthetic(cwist_grpc_channel *ch,
                                                  cwist_grpc_status_t status,
                                                  const char *message,
                                                  uint32_t attempts,
                                                  int has_policy, uint32_t mask) {
    cwist_grpc_channel_call *cc = cwist_alloc(sizeof(*cc));
    if (!cc) return NULL;
    memset(cc, 0, sizeof(*cc));
    cc->ch = ch;
    cc->attempts = attempts;
    cc->syn_status = status;
    cc->syn_message = channel_strdup(message);
    cc->acct_has_policy = has_policy;
    cc->acct_mask = mask;
    return cc;
}

/* Hand a live attempt call over to the user.  On allocation failure the
 * attempt is torn down and the subchannel released. */
static cwist_grpc_channel_call *channel_wrap(cwist_grpc_channel *ch,
                                             cwist_grpc_subchannel *sub,
                                             cwist_grpc_call *call,
                                             uint32_t attempts,
                                             int has_policy, uint32_t mask,
                                             int accounted) {
    cwist_grpc_channel_call *cc =
        channel_synthetic(ch, CWIST_GRPC_OK, NULL, attempts, has_policy, mask);
    if (!cc) {
        if (call) cwist_grpc_call_destroy(call);
        if (sub) channel_release(ch, sub);
        return NULL;
    }
    cc->sub = sub;
    cc->call = call;
    cc->accounted = accounted;
    return cc;
}

/* --- retry engine --- */

cwist_grpc_channel_call *cwist_grpc_channel_call_start(cwist_grpc_channel *ch,
                                                       const char *method,
                                                       const void *request,
                                                       size_t request_len,
                                                       uint64_t timeout_ms) {
    if (!ch || !method || (request_len && !request)) return NULL;

    const cwist_grpc_retry_policy *policy = ch->has_policy ? &ch->policy : NULL;
    int wfr = ch->wait_for_ready;
    uint64_t eff_timeout = timeout_ms;
    const grpc_method_config *mc = channel_find_mc(ch, method);
    if (mc) {
        if (mc->has_policy) policy = &mc->policy;
        if (mc->has_wfr) wfr = mc->wait_for_ready;
        if (mc->has_timeout && (eff_timeout == 0 || mc->timeout_ms < eff_timeout))
            eff_timeout = mc->timeout_ms;
    }
    uint64_t deadline = eff_timeout ? channel_now_ms() + eff_timeout : 0;
    int buffer_ok = request_len <= ch->buffer_limit;
    uint32_t max_attempts = 1;
    uint32_t mask = 0;
    if (policy && buffer_ok) {
        max_attempts = policy->max_attempts > GRPC_CHANNEL_MAX_ATTEMPTS_CAP
                           ? GRPC_CHANNEL_MAX_ATTEMPTS_CAP
                           : policy->max_attempts;
        if (max_attempts < 1) max_attempts = 1;
        mask = policy->retryable_status_mask;
    }
    int has_policy = policy != NULL;

    uint32_t policy_attempts = 0;  /* counts against maxAttempts (A6) */
    uint32_t wire_attempts = 0;    /* grpc-previous-rpc-attempts header */
    int case3_used = 0;            /* one immediate transparent retry (A6) */
    double backoff = policy ? (double)policy->initial_backoff_ms : 0.0;

    for (;;) {
        uint64_t now = channel_now_ms();
        if (deadline && now >= deadline)
            return channel_synthetic(ch, CWIST_GRPC_DEADLINE_EXCEEDED,
                                     "deadline exceeded", policy_attempts,
                                     has_policy, mask);

        cwist_grpc_subchannel *sub = channel_acquire(ch);
        if (!sub) {
            /* A6 failure case 1: the RPC fails at the load balancing step. */
            if (wfr) {
                channel_sleep_ms(20);
                continue;
            }
            policy_attempts++;
            int coded = (mask & CWIST_GRPC_STATUS_BIT(CWIST_GRPC_UNAVAILABLE)) != 0;
            if (coded) channel_throttle_failure(ch); /* failed attempt (A6) */
            if (has_policy && buffer_ok && coded &&
                policy_attempts < max_attempts && channel_throttle_allows(ch)) {
                uint64_t delay = (uint64_t)(backoff * channel_jitter(ch));
                backoff *= policy->backoff_multiplier;
                if (backoff > (double)policy->max_backoff_ms)
                    backoff = (double)policy->max_backoff_ms;
                if (deadline) {
                    uint64_t left = deadline - channel_now_ms();
                    if (delay > left) delay = left;
                }
                channel_sleep_ms(delay);
                continue;
            }
            cwist_grpc_channel_call *cc =
                channel_synthetic(ch, CWIST_GRPC_UNAVAILABLE,
                                  "no ready subchannel", policy_attempts,
                                  has_policy, mask);
            if (cc) cc->accounted = 1; /* per-attempt accounting already done */
            return cc;
        }

        policy_attempts++;
        wire_attempts++;
        uint64_t remaining = 0;
        if (deadline) {
            now = channel_now_ms();
            remaining = now < deadline ? deadline - now : 1;
        }
        cwist_grpc_call *call = cwist_grpc_call_start_ex(sub->client, method,
                                                         request, request_len,
                                                         remaining,
                                                         wire_attempts - 1);
        if (!call) {
            /* A6 case 2: the RPC never left the client.  Transparent retry,
             * uncounted, until the deadline passes. */
            channel_recycle(ch, sub);
            channel_release(ch, sub);
            policy_attempts--;
            wire_attempts--;
            continue;
        }

        if (cwist_grpc_call_await_headers(call) == 0) {
            /* Response-Headers arrived: the RPC is committed (A6). */
            return channel_wrap(ch, sub, call, policy_attempts, has_policy, mask, 0);
        }

        cwist_grpc_status_t status = cwist_grpc_call_status(call);
        if (status == CWIST_GRPC_OK) {
            /* Trailers-Only OK: successful empty response, hand it over. */
            cwist_grpc_channel_call *cc =
                channel_wrap(ch, sub, call, policy_attempts, has_policy, mask, 1);
            if (cc) channel_account(ch, CWIST_GRPC_OK, has_policy, mask);
            return cc;
        }

        if (cwist_grpc_call_refused(call) && !case3_used) {
            /* A6 case 3: never seen by server application logic.  One
             * immediate transparent retry, uncounted. */
            case3_used = 1;
            cwist_grpc_call_destroy(call);
            channel_recycle(ch, sub);
            channel_release(ch, sub);
            policy_attempts--;
            continue;
        }

        int32_t pushback = 0;
        int has_pushback = cwist_grpc_call_retry_pushback_ms(call, &pushback);
        int coded_retryable = (mask & CWIST_GRPC_STATUS_BIT(status)) != 0;
        /* Throttle bookkeeping for this failed attempt (A6). */
        if (coded_retryable || (has_pushback && pushback < 0))
            channel_throttle_failure(ch);

        if (has_pushback && pushback < 0) {
            /* Pushback: do not retry. */
            return channel_wrap(ch, sub, call, policy_attempts, has_policy, mask, 1);
        }

        if (!has_policy || !buffer_ok || !coded_retryable ||
            policy_attempts >= max_attempts || !channel_throttle_allows(ch)) {
            return channel_wrap(ch, sub, call, policy_attempts, has_policy, mask, 1);
        }

        uint64_t delay;
        if (has_pushback) {
            /* Retry after exactly the pushback delay; the backoff progression
             * restarts from initialBackoff afterwards (A6). */
            delay = (uint64_t)pushback;
            backoff = (double)policy->initial_backoff_ms;
        } else {
            delay = (uint64_t)(backoff * channel_jitter(ch));
            backoff *= policy->backoff_multiplier;
            if (backoff > (double)policy->max_backoff_ms)
                backoff = (double)policy->max_backoff_ms;
        }
        cwist_grpc_call_destroy(call);
        channel_release(ch, sub);
        if (deadline) {
            uint64_t left = deadline - channel_now_ms();
            if (delay > left) delay = left;
        }
        channel_sleep_ms(delay);
    }
}

int cwist_grpc_channel_call_recv(cwist_grpc_channel_call *cc, cwist_grpc_message *out) {
    if (!cc || !out) return -1;
    if (!cc->call) return 0; /* synthetic failure: no messages */
    return cwist_grpc_call_recv(cc->call, out);
}

cwist_grpc_status_t cwist_grpc_channel_call_finish(cwist_grpc_channel_call *cc,
                                                   const char **message) {
    if (!cc) {
        if (message) *message = NULL;
        return CWIST_GRPC_INTERNAL;
    }
    cwist_grpc_status_t status;
    if (cc->call) {
        status = cwist_grpc_call_finish(cc->call, message);
    } else {
        status = cc->syn_status;
        if (message) *message = cc->syn_message;
    }
    if (!cc->accounted) {
        channel_account(cc->ch, status, cc->acct_has_policy, cc->acct_mask);
        cc->accounted = 1;
    }
    return status;
}

void cwist_grpc_channel_call_cancel(cwist_grpc_channel_call *cc) {
    if (cc && cc->call) cwist_grpc_call_cancel(cc->call);
}

void cwist_grpc_channel_call_destroy(cwist_grpc_channel_call *cc) {
    if (!cc) return;
    if (!cc->accounted) {
        cwist_grpc_status_t status = cc->call ? cwist_grpc_call_status(cc->call)
                                              : cc->syn_status;
        channel_account(cc->ch, status, cc->acct_has_policy, cc->acct_mask);
        cc->accounted = 1;
    }
    if (cc->call) cwist_grpc_call_destroy(cc->call);
    if (cc->sub) {
        channel_recycle(cc->ch, cc->sub);
        channel_release(cc->ch, cc->sub);
    }
    cwist_free(cc->syn_message);
    cwist_free(cc);
}

uint32_t cwist_grpc_channel_call_attempts(const cwist_grpc_channel_call *cc) {
    return cc ? cc->attempts : 0;
}

cwist_grpc_status_t cwist_grpc_channel_unary(cwist_grpc_channel *ch,
                                             const char *method,
                                             const void *request, size_t request_len,
                                             uint64_t timeout_ms,
                                             uint8_t **response, size_t *response_len,
                                             char **status_message) {
    if (response) *response = NULL;
    if (response_len) *response_len = 0;
    if (status_message) *status_message = NULL;
    cwist_grpc_channel_call *cc =
        cwist_grpc_channel_call_start(ch, method, request, request_len, timeout_ms);
    if (!cc) return CWIST_GRPC_INTERNAL;

    uint8_t *copy = NULL;
    size_t copy_len = 0;
    cwist_grpc_message msg;
    if (cwist_grpc_channel_call_recv(cc, &msg) == 1) {
        copy = cwist_alloc(msg.len ? msg.len : 1);
        if (copy && msg.len) memcpy(copy, msg.data, msg.len);
        copy_len = copy ? msg.len : 0;
        while (cwist_grpc_channel_call_recv(cc, &msg) == 1)
            ; /* unary: ignore trailing messages */
    }
    const char *sm = NULL;
    cwist_grpc_status_t status = cwist_grpc_channel_call_finish(cc, &sm);
    if (status_message && sm) *status_message = channel_strdup(sm);
    cwist_grpc_channel_call_destroy(cc);

    if (status != CWIST_GRPC_OK) {
        cwist_free(copy);
        copy = NULL;
        copy_len = 0;
    }
    if (response) *response = copy;
    else cwist_free(copy);
    if (response_len) *response_len = copy_len;
    return status;
}

/* --- name resolution (doc/naming.md) --- */

/* Split "host[:port]" or "[v6][:port]"; bare multi-colon names are IPv6
 * literals without a port.  Default port 443 (naming.md). */
static int channel_split_host_port(const char *s, size_t len,
                                   char *host, size_t host_cap, uint16_t *port) {
    *port = 443;
    if (!s || !len) return -1;
    if (s[0] == '[') {
        const char *close = memchr(s, ']', len);
        if (!close) return -1;
        size_t hlen = (size_t)(close - s - 1);
        if (!hlen || hlen >= host_cap) return -1;
        memcpy(host, s + 1, hlen);
        host[hlen] = '\0';
        size_t used = (size_t)(close - s) + 1;
        if (used < len) {
            if (close[1] != ':' || used + 1 == len) return -1;
            long p = strtol(close + 2, NULL, 10);
            if (p <= 0 || p > 65535) return -1;
            *port = (uint16_t)p;
        }
        return 0;
    }
    const char *first_colon = memchr(s, ':', len);
    const char *last_colon = NULL;
    for (size_t i = 0; i < len; i++)
        if (s[i] == ':') last_colon = s + i;
    if (first_colon && first_colon != last_colon) {
        /* several colons: bare IPv6 literal, no port */
        if (len >= host_cap) return -1;
        memcpy(host, s, len);
        host[len] = '\0';
        return 0;
    }
    if (last_colon) {
        size_t hlen = (size_t)(last_colon - s);
        if (!hlen || hlen >= host_cap) return -1;
        long p = strtol(last_colon + 1, NULL, 10);
        if (p <= 0 || p > 65535) return -1;
        memcpy(host, s, hlen);
        host[hlen] = '\0';
        *port = (uint16_t)p;
        return 0;
    }
    if (len >= host_cap) return -1;
    memcpy(host, s, len);
    host[len] = '\0';
    return 0;
}

static int channel_add_addr(cwist_grpc_channel *ch, const char *host, uint16_t port) {
    for (size_t i = 0; i < ch->nsubs; i++)
        if (ch->subs[i].port == port && strcmp(ch->subs[i].host, host) == 0)
            return 0; /* duplicate */
    if (ch->nsubs >= GRPC_CHANNEL_MAX_ADDRS) return -1;
    cwist_grpc_subchannel *sub = &ch->subs[ch->nsubs];
    memset(sub, 0, sizeof(*sub));
    sub->host = channel_strdup(host);
    if (!sub->host) return -1;
    sub->port = port;
    sub->backoff_ms = GRPC_CHANNEL_CONN_BACKOFF_INITIAL_MS;
    ch->nsubs++;
    return 0;
}

static int channel_resolve_dns(cwist_grpc_channel *ch, const char *name) {
    char host[256];
    uint16_t port;
    if (channel_split_host_port(name, strlen(name), host, sizeof(host), &port) != 0)
        return -1;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;
    int rc = -1;
    for (struct addrinfo *it = res; it; it = it->ai_next) {
        char numeric[INET6_ADDRSTRLEN];
        if (getnameinfo(it->ai_addr, it->ai_addrlen, numeric, sizeof(numeric),
                        NULL, 0, NI_NUMERICHOST) != 0)
            continue;
        if (channel_add_addr(ch, numeric, port) == 0) rc = 0;
    }
    freeaddrinfo(res);
    if (rc == 0 && !ch->tls_name) ch->tls_name = channel_strdup(host);
    if (rc == 0 && !ch->authority) {
        char buf[300];
        snprintf(buf, sizeof(buf), "%s:%u", host, (unsigned)port);
        ch->authority = channel_strdup(buf);
    }
    return rc;
}

static int channel_resolve_literal(cwist_grpc_channel *ch, const char *list,
                                   int v6) {
    char *copy = channel_strdup(list);
    if (!copy) return -1;
    int rc = -1;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char host[256];
        uint16_t port;
        if (channel_split_host_port(tok, strlen(tok), host, sizeof(host), &port) != 0)
            continue;
        struct in_addr a4;
        struct in6_addr a6;
        if (v6 ? inet_pton(AF_INET6, host, &a6) != 1
               : inet_pton(AF_INET, host, &a4) != 1)
            continue;
        if (channel_add_addr(ch, host, port) == 0) rc = 0;
    }
    cwist_free(copy);
    if (rc == 0 && !ch->tls_name && ch->nsubs)
        ch->tls_name = channel_strdup(ch->subs[0].host);
    if (rc == 0 && !ch->authority && ch->nsubs) {
        char buf[300];
        snprintf(buf, sizeof(buf), "%s:%u", ch->subs[0].host,
                 (unsigned)ch->subs[0].port);
        ch->authority = channel_strdup(buf);
    }
    return rc;
}

/* --- channel lifecycle --- */

cwist_grpc_channel *cwist_grpc_channel_connect(const char *target,
                                               const cwist_grpc_channel_options *options) {
    if (!target || !*target) return NULL;
    cwist_grpc_channel *ch = cwist_alloc(sizeof(*ch));
    if (!ch) return NULL;
    memset(ch, 0, sizeof(*ch));
    pthread_mutex_init(&ch->mu, NULL);
    pthread_mutex_init(&ch->tmu, NULL);
    ch->subs = cwist_alloc(sizeof(*ch->subs) * GRPC_CHANNEL_MAX_ADDRS);
    if (!ch->subs) {
        cwist_free(ch);
        return NULL;
    }
    ch->lb = options ? options->lb_policy : CWIST_GRPC_LB_PICK_FIRST;
    if (options && options->retry_policy) {
        ch->policy = *options->retry_policy;
        ch->has_policy = 1;
    }
    if (options && options->retry_throttle) {
        ch->throttle = *options->retry_throttle;
        ch->has_throttle = 1;
        ch->tokens = ch->throttle.max_tokens; /* starts at maxTokens (A6) */
    }
    ch->buffer_limit = options && options->per_rpc_buffer_limit
                           ? options->per_rpc_buffer_limit
                           : GRPC_CHANNEL_DEFAULT_BUFFER_LIMIT;
    ch->wait_for_ready = options ? options->wait_for_ready : 0;
    if (options) {
        ch->copt.use_tls = options->use_tls;
        ch->copt.verify_peer = options->verify_peer;
        ch->copt.connect_timeout_ms = options->connect_timeout_ms;
        if (options->authority) ch->authority = channel_strdup(options->authority);
    }
    ch->rng = (uint64_t)channel_now_ms() ^ ((uintptr_t)ch >> 4) ^
              ((uint64_t)getpid() << 32);
    if (!ch->rng) ch->rng = 0x9e3779b97f4a7c15ULL;

    int rc;
    if (strncmp(target, "dns:", 4) == 0) {
        const char *name = target + 4;
        if (name[0] == '/' && name[1] == '/') {
            /* dns://authority/name — the default resolver ignores the
             * authority, like the gRPC C-core dns resolver. */
            name += 2;
            const char *slash = strchr(name, '/');
            if (!slash) goto fail;
            name = slash + 1;
        }
        rc = channel_resolve_dns(ch, name);
    } else if (strncmp(target, "ipv4:", 5) == 0) {
        rc = channel_resolve_literal(ch, target + 5, 0);
    } else if (strncmp(target, "ipv6:", 5) == 0) {
        rc = channel_resolve_literal(ch, target + 5, 1);
    } else if (strncmp(target, "unix:", 5) == 0 ||
               strncmp(target, "unix-abstract:", 14) == 0 ||
               strncmp(target, "vsock:", 6) == 0) {
        goto fail; /* transports the client does not implement */
    } else {
        rc = channel_resolve_dns(ch, target); /* no scheme = dns (naming.md) */
    }
    if (rc != 0 || ch->nsubs == 0) goto fail;
    if (!ch->authority) goto fail;

    /* round_robin connects to every address up front and keeps watching
     * them (load-balancing.md); pick_first stays IDLE until the first RPC. */
    if (ch->lb == CWIST_GRPC_LB_ROUND_ROBIN) {
        pthread_mutex_lock(&ch->mu);
        for (size_t i = 0; i < ch->nsubs; i++)
            (void)sub_ensure_connected(ch, &ch->subs[i]);
        pthread_mutex_unlock(&ch->mu);
    }
    return ch;

fail:
    cwist_grpc_channel_close(ch);
    return NULL;
}

void cwist_grpc_channel_close(cwist_grpc_channel *ch) {
    if (!ch) return;
    /* Calls handed out must be destroyed before closing the channel. */
    for (size_t i = 0; i < ch->nsubs; i++) {
        if (ch->subs[i].client) cwist_grpc_client_close(ch->subs[i].client);
        cwist_free(ch->subs[i].host);
    }
    cwist_free(ch->subs);
    channel_free_mcs(ch);
    cwist_free(ch->authority);
    cwist_free(ch->tls_name);
    pthread_mutex_destroy(&ch->mu);
    pthread_mutex_destroy(&ch->tmu);
    cwist_free(ch);
}

cwist_grpc_channel_state cwist_grpc_channel_get_state(cwist_grpc_channel *ch) {
    if (!ch) return CWIST_GRPC_CHANNEL_SHUTDOWN;
    uint64_t now = channel_now_ms();
    int any_ready = 0, any_dialable = 0;
    pthread_mutex_lock(&ch->mu);
    for (size_t i = 0; i < ch->nsubs; i++) {
        cwist_grpc_subchannel *s = &ch->subs[i];
        if (s->client && !cwist_grpc_client_dead(s->client)) {
            any_ready = 1;
            break;
        }
        if (!s->next_connect_at || now >= s->next_connect_at) any_dialable = 1;
    }
    pthread_mutex_unlock(&ch->mu);
    /* load-balancing.md aggregation: READY if any subchannel is READY,
     * else IDLE while any subchannel may dial, else TRANSIENT_FAILURE. */
    if (any_ready) return CWIST_GRPC_CHANNEL_READY;
    if (any_dialable) return CWIST_GRPC_CHANNEL_IDLE;
    return CWIST_GRPC_CHANNEL_TRANSIENT_FAILURE;
}

/* --- JSON service config (doc/service_config.md subset, gRFC A6) --- */

static const char *const grpc_status_names[] = {
    "OK", "CANCELLED", "UNKNOWN", "INVALID_ARGUMENT", "DEADLINE_EXCEEDED",
    "NOT_FOUND", "ALREADY_EXISTS", "PERMISSION_DENIED", "RESOURCE_EXHAUSTED",
    "FAILED_PRECONDITION", "ABORTED", "OUT_OF_RANGE", "UNIMPLEMENTED",
    "INTERNAL", "UNAVAILABLE", "DATA_LOSS", "UNAUTHENTICATED"
};

static int grpc_status_from_json(const cJSON *item, uint32_t *bit) {
    if (cJSON_IsNumber(item)) {
        int code = (int)item->valuedouble;
        if (code < 0 || code > 16) return -1;
        *bit = CWIST_GRPC_STATUS_BIT(code);
        return 0;
    }
    if (cJSON_IsString(item)) {
        for (int i = 0; i <= 16; i++) {
            if (strcasecmp(item->valuestring, grpc_status_names[i]) == 0) {
                *bit = CWIST_GRPC_STATUS_BIT(i);
                return 0;
            }
        }
    }
    return -1;
}

/* proto3 JSON Duration: a decimal number with an "s" suffix. */
static int grpc_json_duration_ms(const cJSON *item, uint64_t *out_ms) {
    if (!cJSON_IsString(item)) return -1;
    const char *s = item->valuestring;
    size_t len = strlen(s);
    if (len < 2 || s[len - 1] != 's') return -1;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end != s + len - 1 || v <= 0) return -1;
    *out_ms = (uint64_t)(v * 1000.0 + 0.5);
    if (*out_ms == 0) *out_ms = 1;
    return 0;
}

/* A6 retryPolicy validation: maxAttempts integer > 1 (clamped to 5),
 * positive backoffs/multiplier, non-empty retryableStatusCodes. */
static int grpc_json_retry_policy(const cJSON *obj, cwist_grpc_retry_policy *out) {
    const cJSON *max_attempts = cJSON_GetObjectItemCaseSensitive(obj, "maxAttempts");
    const cJSON *initial = cJSON_GetObjectItemCaseSensitive(obj, "initialBackoff");
    const cJSON *max = cJSON_GetObjectItemCaseSensitive(obj, "maxBackoff");
    const cJSON *mult = cJSON_GetObjectItemCaseSensitive(obj, "backoffMultiplier");
    const cJSON *codes = cJSON_GetObjectItemCaseSensitive(obj, "retryableStatusCodes");
    if (!cJSON_IsNumber(max_attempts) || max_attempts->valuedouble < 2 ||
        max_attempts->valuedouble != (double)(int)max_attempts->valuedouble)
        return -1;
    uint64_t initial_ms, max_ms;
    if (grpc_json_duration_ms(initial, &initial_ms) != 0 ||
        grpc_json_duration_ms(max, &max_ms) != 0)
        return -1;
    if (!cJSON_IsNumber(mult) || mult->valuedouble <= 0) return -1;
    if (!cJSON_IsArray(codes) || cJSON_GetArraySize((cJSON *)codes) == 0) return -1;
    uint32_t mask = 0;
    const cJSON *code;
    cJSON_ArrayForEach(code, codes) {
        uint32_t bit;
        if (grpc_status_from_json(code, &bit) != 0) return -1;
        mask |= bit;
    }
    out->max_attempts = (uint32_t)max_attempts->valuedouble;
    if (out->max_attempts > GRPC_CHANNEL_MAX_ATTEMPTS_CAP)
        out->max_attempts = GRPC_CHANNEL_MAX_ATTEMPTS_CAP; /* A6: no validation error */
    out->initial_backoff_ms = initial_ms;
    out->max_backoff_ms = max_ms;
    out->backoff_multiplier = mult->valuedouble;
    out->retryable_status_mask = mask;
    return 0;
}

static void grpc_json_free_mc_list(grpc_method_config *list) {
    while (list) {
        grpc_method_config *next = list->next;
        cwist_free(list->service);
        cwist_free(list->method);
        cwist_free(list);
        list = next;
    }
}

int cwist_grpc_channel_apply_service_config_json(cwist_grpc_channel *ch, const char *json) {
    if (!ch || !json) return -1;
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }

    int lb_set = 0;
    cwist_grpc_lb_policy lb = ch->lb;
    int throttle_set = 0;
    cwist_grpc_retry_throttle throttle;
    grpc_method_config *mcs = NULL, **mcs_tail = &mcs;

    /* loadBalancingConfig: [{"round_robin":{}}, {"pick_first":{}}] — the
     * first supported policy wins (service_config.md). */
    const cJSON *lbc = cJSON_GetObjectItemCaseSensitive(root, "loadBalancingConfig");
    if (cJSON_IsArray(lbc)) {
        const cJSON *entry;
        cJSON_ArrayForEach(entry, lbc) {
            if (!cJSON_IsObject(entry)) continue;
            if (cJSON_GetObjectItemCaseSensitive(entry, "round_robin")) {
                lb = CWIST_GRPC_LB_ROUND_ROBIN;
                lb_set = 1;
                break;
            }
            if (cJSON_GetObjectItemCaseSensitive(entry, "pick_first")) {
                lb = CWIST_GRPC_LB_PICK_FIRST;
                lb_set = 1;
                break;
            }
            /* unsupported policy: try the next entry */
        }
    }
    if (!lb_set) {
        /* Deprecated loadBalancingPolicy string form. */
        const cJSON *lbp = cJSON_GetObjectItemCaseSensitive(root, "loadBalancingPolicy");
        if (cJSON_IsString(lbp)) {
            if (strcasecmp(lbp->valuestring, "ROUND_ROBIN") == 0) {
                lb = CWIST_GRPC_LB_ROUND_ROBIN;
                lb_set = 1;
            } else if (strcasecmp(lbp->valuestring, "PICK_FIRST") == 0) {
                lb = CWIST_GRPC_LB_PICK_FIRST;
                lb_set = 1;
            }
        }
    }

    const cJSON *rt = cJSON_GetObjectItemCaseSensitive(root, "retryThrottling");
    if (rt) {
        const cJSON *max_tokens = cJSON_GetObjectItemCaseSensitive(rt, "maxTokens");
        const cJSON *ratio = cJSON_GetObjectItemCaseSensitive(rt, "tokenRatio");
        if (!cJSON_IsNumber(max_tokens) || max_tokens->valuedouble <= 0 ||
            max_tokens->valuedouble > 1000 ||
            !cJSON_IsNumber(ratio) || ratio->valuedouble <= 0)
            goto invalid;
        throttle.max_tokens = max_tokens->valuedouble;
        /* A6: decimal places beyond 3 are ignored */
        throttle.token_ratio = ((double)(int)(ratio->valuedouble * 1000.0)) / 1000.0;
        throttle_set = 1;
    }

    const cJSON *mc_array = cJSON_GetObjectItemCaseSensitive(root, "methodConfig");
    if (mc_array && !cJSON_IsArray(mc_array)) goto invalid;
    if (cJSON_IsArray(mc_array)) {
        const cJSON *entry;
        cJSON_ArrayForEach(entry, mc_array) {
            if (!cJSON_IsObject(entry)) goto invalid;
            const cJSON *names = cJSON_GetObjectItemCaseSensitive(entry, "name");
            if (!cJSON_IsArray(names) || cJSON_GetArraySize((cJSON *)names) == 0)
                goto invalid;

            int has_policy = 0;
            cwist_grpc_retry_policy policy;
            const cJSON *rp = cJSON_GetObjectItemCaseSensitive(entry, "retryPolicy");
            const cJSON *hp = cJSON_GetObjectItemCaseSensitive(entry, "hedgingPolicy");
            if (rp && hp) goto invalid; /* A6: one policy per RPC, never both */
            if (rp) {
                if (grpc_json_retry_policy(rp, &policy) != 0) goto invalid;
                has_policy = 1;
            }
            /* hedgingPolicy is not implemented (as in gRPC C-core): the
             * entry simply carries no retry policy. */

            int has_wfr = 0, wfr = 0;
            const cJSON *wfr_item = cJSON_GetObjectItemCaseSensitive(entry, "waitForReady");
            if (wfr_item) {
                if (!cJSON_IsBool(wfr_item)) goto invalid;
                has_wfr = 1;
                wfr = cJSON_IsTrue(wfr_item);
            }
            int has_timeout = 0;
            uint64_t timeout_ms = 0;
            const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(entry, "timeout");
            if (timeout) {
                if (grpc_json_duration_ms(timeout, &timeout_ms) != 0) goto invalid;
                has_timeout = 1;
            }

            /* One methodConfig entry per name; first match wins at lookup,
             * so list order is preserved. */
            const cJSON *name;
            cJSON_ArrayForEach(name, names) {
                if (!cJSON_IsObject(name)) goto invalid;
                const cJSON *service = cJSON_GetObjectItemCaseSensitive(name, "service");
                const cJSON *method = cJSON_GetObjectItemCaseSensitive(name, "method");
                if (!cJSON_IsString(service)) goto invalid;
                if (method && !cJSON_IsString(method)) goto invalid;
                grpc_method_config *mc = cwist_alloc(sizeof(*mc));
                if (!mc) goto invalid;
                memset(mc, 0, sizeof(*mc));
                mc->service = channel_strdup(service->valuestring);
                mc->method = method ? channel_strdup(method->valuestring) : NULL;
                if (!mc->service || (method && !mc->method)) {
                    cwist_free(mc->service);
                    cwist_free(mc->method);
                    cwist_free(mc);
                    goto invalid;
                }
                mc->has_policy = has_policy;
                mc->policy = policy;
                mc->has_wfr = has_wfr;
                mc->wait_for_ready = wfr;
                mc->has_timeout = has_timeout;
                mc->timeout_ms = timeout_ms;
                *mcs_tail = mc;
                mcs_tail = &mc->next;
            }
        }
    }

    /* Everything validated: swap it in. */
    pthread_mutex_lock(&ch->mu);
    if (lb_set) ch->lb = lb;
    channel_free_mcs(ch);
    ch->mcs = mcs;
    if (ch->lb == CWIST_GRPC_LB_ROUND_ROBIN) {
        /* round_robin watches every subchannel (load-balancing.md): dial
         * them all so the READY set rotates strictly. */
        for (size_t i = 0; i < ch->nsubs; i++)
            (void)sub_ensure_connected(ch, &ch->subs[i]);
    }
    pthread_mutex_unlock(&ch->mu);
    if (throttle_set) {
        pthread_mutex_lock(&ch->tmu);
        int had = ch->has_throttle;
        ch->throttle = throttle;
        ch->has_throttle = 1;
        if (!had) ch->tokens = throttle.max_tokens;
        pthread_mutex_unlock(&ch->tmu);
    }
    cJSON_Delete(root);
    return 0;

invalid:
    grpc_json_free_mc_list(mcs);
    cJSON_Delete(root);
    return -1;
}
