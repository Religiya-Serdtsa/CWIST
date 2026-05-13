# AUTH.md — CWIST Query-Map Parser Migration Specification

> **DO NOT COMMIT THIS FILE.**
> This document is a one-shot AI prompt intended to align CWIST’s `cwist_query_map_parse` with the exact tokenization and decoding semantics of the flyboard legacy parser.

---

## 1. Executive Summary

flyboard’s legacy `parse_urlencoded` and CWIST’s current `cwist_query_map_parse` differ in two surface-visible dimensions:

1. **Decoding rules** — The legacy parser treats `+` as a space (`0x20`) and performs strict `%XY` hex decoding. The current CWIST implementation delegates this to `uriparser`, whose `+` semantics are opaque and whose empty-key handling deviates.
2. **Delimiter branch logic** — The legacy parser splits on `&`, mandates an `=` separator, and **skips** segments that lack one. It also lets the **first** occurrence of a duplicate key win. The current CWIST parser overwrites duplicates and materializes key-only segments as empty strings.

This document specifies, in algorithmic prose and tabular form, how to re-implement the *parsing front-end* of `cwist_query_map_parse` while preserving its SipHash-backed hash-map storage and O(1) average lookup complexity.

---

## 2. Legacy Parser Algorithm (Flyboard)

The following pseudocode is a direct abstraction of `src/utils/utils.c` (`parse_urlencoded` and `url_decode`).

```
function ParseUrlEncoded(body):
    if body is NULL:
        return empty_map

    map ← EmptyLinkedList()
    pairs ← SplitByDelimiter(body, "&")

    for each pair in pairs:
        eq ← IndexOfFirst(pair, "=")
        if eq is NULL:
            continue                         // Rule: no '=' → discard segment

        key_raw   ← Substring(pair, 0, eq)
        value_raw ← Substring(pair, eq + 1, end)

        key_decoded   ← UrlDecode(key_raw)
        value_decoded ← UrlDecode(value_raw)

        map.append(key_decoded, value_decoded)
        // Duplicates are appended; first insertion wins on linear search.
    end for

    return map
end function

function UrlDecode(src):
    len ← Length(src)
    out ← Allocate(len + 1)
    j ← 0

    for i from 0 to len - 1:
        if src[i] = '%' and i + 2 < len
                        and IsHexDigit(src[i + 1])
                        and IsHexDigit(src[i + 2]):
            out[j] ← HexToByte(src[i + 1], src[i + 2])
            j ← j + 1
            i ← i + 2
        else if src[i] = '+':
            out[j] ← 0x20                          // SPACE (U+0020)
            j ← j + 1
        else:
            out[j] ← src[i]
            j ← j + 1
        end if
    end for

    out[j] ← '\0'
    return out
end function
```

---

## 3. Divergence Analysis: Legacy vs. Current CWIST

| # | Behavior | Legacy (flyboard) | Current CWIST (`uriparser`) | Required Action |
|---|----------|-------------------|-----------------------------|-----------------|
| 1 | **Delimiter** | `&` only | `&` (via `uriparser`) | None |
| 2 | **Pair separator** | `=` is **mandatory** | `=` (via `uriparser`) | **Discard segment if `=` absent** |
| 3 | **`+` → space** | **Yes** (`0x20`) | Undefined / RFC-3986 only | **Explicitly decode `+` to `0x20`** |
| 4 | **`%XY` decoding** | `%` + two hex digits → byte | `uriparser` native | **Match exact hex-digit rule** |
| 5 | **Empty value (`k=`)** | Empty string `""` | Empty string `""` | None |
| 6 | **Missing `=` (`k`)** | **Skip entirely** | Stored as `k → ""` | **Skip entirely** |
| 7 | **Duplicate keys** | **First wins** (append, linear scan) | **Last wins** (overwrite) | **First wins** |
| 8 | **NULL / empty body** | Returns NULL | Early return | Return with no side effects |

---

## 4. Exact Character Substitution Table

| Input Sequence | Output Byte | Applicability |
|----------------|-------------|---------------|
| `+` | `0x20` (SP) | Unconditional |
| `%XY` | `0x{XY}` | Iff `X` and `Y` are hex digits (`[0-9A-Fa-f]`) |
| Any other byte `C` | `C` | Passthrough |

**Important:** Hex-digit validation must use `isxdigit((unsigned char)ch)` to avoid sign-extension bugs on platforms where `char` is signed.

---

## 5. AI Prompt: CWIST Implementation Directive

> **System Prompt — CWIST Query-Map Parser Refinement**
>
> You are modifying `cwist_query_map_parse` in `src/net/http/query.c`. The existing implementation delegates all tokenization and decoding to `uriparser` (`uriDissectQueryMallocA`). You must replace that delegation with a **manual, in-place parser** that preserves the SipHash-based hash-map storage (`cwist_query_map`, `cwist_query_map_set`, `cwist_query_map_get`) but changes the **tokenization and decoding logic** to match the flyboard legacy specification below **exactly**.
>
> ### Structural Constraints (DO NOT ALTER)
> - Retain `cwist_query_map_create`, `cwist_query_map_destroy`, `cwist_query_map_get`, `cwist_query_map_set`, and `cwist_query_map_clear` without modification.
> - Retain SipHash24 bucket indexing and collision chaining.
> - All heap operations must use `cwist_alloc` / `cwist_free` (or `cwist_strdup` where appropriate).
>
> ### Parsing Constraints (MUST IMPLEMENT)
> 1. **Delimiter**: Split the raw input strictly on `&`. Reject or ignore `;`.
> 2. **Pair validation**: If a segment does **not** contain `=`, discard the entire segment (`continue`).
> 3. **Key / Value extraction**:
>    - `key` = octets before the **first** `=`.
>    - `value` = octets after the **first** `=`.
> 4. **Decoding**: Apply the `UrlDecode` algorithm (Section 2) independently to both `key` and `value` **before** insertion.
> 5. **Duplicate-key policy**: If `cwist_query_map_get(map, key_decoded)` returns non-NULL, **do nothing** for that segment. The first insertion wins; do **not** overwrite.
> 6. **Empty input**: If `raw_query` is NULL or `strlen(raw_query) == 0`, return immediately with no mutations.
>
> ### Reference Pseudocode for the New `cwist_query_map_parse`
>
> ```
> procedure cwist_query_map_parse(map, raw_query):
>     if map = NULL or raw_query = NULL or strlen(raw_query) = 0:
>         return
>
>     buffer ← cwist_strdup(raw_query)          // mutable copy for strtok_r
>     save_ptr ← NULL
>     token ← strtok_r(buffer, "&", save_ptr)
>
>     while token ≠ NULL:
>         eq ← strchr(token, '=')
>         if eq = NULL:
>             token ← strtok_r(NULL, "&", save_ptr)
>             continue
>
>         *eq ← '\0'
>         key_raw   ← token
>         value_raw ← eq + 1
>
>         key_dec   ← url_decode(key_raw)
>         value_dec ← url_decode(value_raw)
>
>         if key_dec ≠ NULL and cwist_query_map_get(map, key_dec) = NULL:
>             cwist_query_map_set(map, key_dec, value_dec ≠ NULL ? value_dec : "")
>         end if
>
>         cwist_free(key_dec)
>         cwist_free(value_dec)
>         token ← strtok_r(NULL, "&", save_ptr)
>     end while
>
>     cwist_free(buffer)
> end procedure
> ```
>
> ### `url_decode` Implementation Contract
> Allocate an output buffer of `strlen(src) + 1` via `cwist_alloc`. Iterate with index `i`:
> - If `src[i] == '%'` and `i + 2 < len` and `isxdigit((unsigned char)src[i + 1])` and `isxdigit((unsigned char)src[i + 2])`:
>   - Decode `src[i + 1]` and `src[i + 2]` as a hexadecimal byte.
>   - Advance `i` by 2.
> - Else if `src[i] == '+'`:
>   - Emit `0x20` (space).
> - Else:
>   - Emit `src[i]` unchanged.
> - Null-terminate the output buffer.
>
> Do **not** link against `uriparser` for this function.
>
> ### Acceptance Test Vectors
> | Input | Expected Map State |
> |-------|-------------------|
> | `username=admin&password=secret%21` | `username → "admin"`, `password → "secret!"` |
> | `foo&bar=` | `foo` **absent**, `bar → ""` |
> | `a=1&a=2` | `a → "1"` (first wins) |
> | `key=hello+world` | `key → "hello world"` |
> | `empty` | (empty map; no `=`, so skipped) |
