# Tutorial 22: Throughput Rate Limiting Middleware

Protect sensitive API endpoints from Denial-of-Service (DoS) flooding using rate-limiting middleware.

## Key Concepts
- Attaching token bucket or window rate limiters via `cwist_middleware_rate_limit`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut22
```
