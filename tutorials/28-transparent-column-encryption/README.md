# Tutorial 28: Transparent Column-Level Database Encryption

Encrypt and decrypt sensitive database fields transparently at column boundaries (`db_crypt`).

## Key Concepts
- Automatic AES/ChaCha20 field encryption for compliance and data privacy.
- `sealed`/`opened` are released via `CWIST_DEFER_FREE` rather than manual
  `free()` calls — since both stay local to `main()`, attaching the macro to
  each declaration releases it automatically at the end of whichever block
  it was declared in (see `cwist/core/mem/alloc.h`), instead of a `free()`
  that has to be kept paired with every return path by hand.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut28
```
