# SString API

*Header:* `<cwist/core/sstring/sstring.h>`

Safe, dynamic string manipulation library.

### `cwist_sstring_create`
```c
cwist_sstring *cwist_sstring_create(void);
```
Creates a dynamic string.

### `cwist_sstring_assign_len`
```c
cwist_error_t cwist_sstring_assign_len(cwist_sstring *str, const char *data, size_t len);
```
Binary-safe assignment. Copies exactly `len` bytes (plus a trailing NUL for convenience) so HTTP handlers can store POST bodies that contain `\0`.

### `cwist_sstring_append`
```c
cwist_error_t cwist_sstring_append(cwist_sstring *str, const char *suffix);
```
Safe concatenation. Returns `cwist_error_t`.

### `cwist_sstring_append_len`
```c
cwist_error_t cwist_sstring_append_len(cwist_sstring *str, const char *data, size_t len);
```
Binary-safe append. Used by the HTTP response serializer to stream exact `Content-Length` bytes.

### `cwist_sstring_destroy`
```c
void cwist_sstring_destroy(cwist_sstring *str);
```
Frees the string memory.
