# Tutorial 09: Full-Duplex WebSocket Server

Handle bidirectional full-duplex WebSocket connections and frame messaging.

## Key Concepts
- Register WebSocket endpoint route handlers using `cwist_app_ws(app, "/ws", on_message_fn)`.
- Send text frames back to clients using `cwist_websocket_send_text(ws, message)`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut09
```
