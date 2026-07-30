# Integrate NATS with CWIST

CWIST includes an in-tree NATS integration point for event-driven services.
Use NATS for asynchronous work and fan-out; keep HTTP handlers responsible for
validating the request and returning a bounded response promptly.

## 1. Separate request and worker responsibilities

An HTTP handler should validate a JSON command, assign a request id, and
publish a compact event such as:

```json
{"type":"post.created","post_id":42,"request_id":"..."}
```

Workers subscribe to `blog.post.created`, persist derived work, and publish a
result or failure event. Use subject names that encode ownership and version,
for example `blog.v1.post.created`.

## 2. Publish from a route

Initialize the NATS client once during application startup, then share the
long-lived connection rather than reconnecting per request. A handler should
only acknowledge success after the chosen durability boundary: successful
publish for fire-and-forget work, or a database transaction/outbox record for
work that must survive a broker outage.

For request/reply, enforce a short deadline and translate timeout or unavailable
errors to `503 Service Unavailable`. Do not keep an HTTP worker blocked on an
unbounded NATS subscription.

## 3. Reliability checklist

- Make consumers idempotent; messages can be redelivered.
- Include a message id and request id in every event for deduplication and tracing.
- Bound payload sizes and validate schemas before publishing or consuming.
- Use durable JetStream consumers when delivery across worker restarts matters.
- Reconnect with backoff and expose connection state through health checks.

The same patterns apply when bridging NATS messages to SSE or WebTransport:
serialize at the transport edge, retain backpressure limits, and drop or
coalesce only explicitly non-critical realtime updates.
