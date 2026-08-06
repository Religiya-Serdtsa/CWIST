# Tutorial 25: Big Dumb Reply (BDR) Cache

Bypass heavy application allocation for static responses using the Big Dumb Reply zero-copy response engine.

## Key Concepts
- Returning pre-cached static buffers with `cwist_bdr_respond_static_ok`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut25
```
