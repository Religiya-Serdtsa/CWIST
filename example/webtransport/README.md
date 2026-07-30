# CWIST WebTransport Example

This example includes a minimal WebTransport server built on CWIST, a small
browser client, and a native C client available on the `dev` branch.

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

## Native C client (dev)

The development branch pins [LSQUIC PR #629](https://github.com/litespeedtech/lsquic/pull/629).
Build and run the native client against the server above:

```sh
make -C example/webtransport/native-client
./example/webtransport/native-client/webtransport-native-client localhost /wt
```

The client performs Extended CONNECT, upgrades it through LSQUIC's
WebTransport session API, sends a datagram, and pumps QUIC I/O until closed.
