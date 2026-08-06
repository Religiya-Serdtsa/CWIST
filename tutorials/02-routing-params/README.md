# Tutorial 02: Dynamic Routing & Path Parameters

Extract dynamic route variables using CWIST dynamic path matching.

## Key Concepts
- Define parameterized route patterns like `/users/:id`.
- Extract route variables inside handler logic with `cwist_http_request_param(req, "id")`.

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut02
```

Test with `curl`:
```bash
curl http://127.0.0.1:8081/users/42
# Output: User Profile ID: 42
```
