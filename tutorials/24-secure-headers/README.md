# Tutorial 24: Hardened HTTP Security Headers

Inject security hardening headers (`X-Frame-Options`, `X-Content-Type-Options`) across all outgoing responses.

## Key Concepts
- Applying security header hardening middleware with `cwist_middleware_secure_headers`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut24
```
