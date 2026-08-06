# Tutorial 07: JSON Web Token (JWT) Authentication

Sign and verify JWT tokens for stateless API authorization using the built-in CWIST JWT engine.

## Key Concepts
- Construct token payload claims using `cJSON`.
- Generate signed JWT string with `cwist_jwt_sign(payload, secret)`.

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
