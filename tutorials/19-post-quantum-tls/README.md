# Tutorial 19: Post-Quantum Hybrid TLS (PQC)

Enable hybrid post-quantum key exchange (X25519MLKEM768) powered by BoringSSL.

## Key Concepts
- One-line PQC layer activation using `cwist_app_use_pqc_layer(app, true)`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut19
```
