# CWIST WebTransport Example

This example includes a minimal WebTransport server built on CWIST and a small
browser client for interactive testing.

## Build

```sh
make example/webtransport/server/webtransport-server
```

## Run

```sh
./example/webtransport/server/webtransport-server 9443
```

The server listens on `https://localhost:9443/wt` by default.

## Client

Open `example/webtransport/client/index.html` in a browser with WebTransport
support and connect to the server URL above.

The example uses the test certificate at `example/othello-web/server.crt` and
`example/othello-web/server.key`. Because it is self-signed, the browser must
trust it or you need to use a local development certificate flow.
