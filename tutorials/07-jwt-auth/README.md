# Tutorial 07: JSON Web Token (JWT) Authentication

Sign and verify JWT tokens for stateless API authorization using the built-in CWIST JWT engine.

## Key Concepts
- Construct token payload claims using `cJSON`.
- Generate signed JWT string with `cwist_jwt_sign(payload, secret)`.
- Release the returned token with `CWIST_DEFER_FREE` (`cwist/core/mem/alloc.h`)
  instead of a manual `free()` call: attach it to the pointer's declaration
  and it releases automatically on every return path out of the enclosing
  block. Only safe for a pointer that never escapes that block — `token`
  here doesn't, since `cwist_sstring_assign()` copies its bytes into the
  response body rather than taking ownership of the pointer.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut07
```

Test with `curl`:
```bash
curl http://127.0.0.1:8086/token
```
