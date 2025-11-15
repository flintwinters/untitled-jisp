# Raw C Pointer JSON Handle Spec (yyjson-based)

This document specifies a lightweight “pointer monad” that represents references to JSON values inside a yyjson mutable document via raw C pointers. It provides a safe-ish and composable way to resolve human-readable JSON Pointer paths (RFC 6901, e.g., "/path/to/obj") into raw C pointers and to sequence computations over those pointers in a monadic style.

The “monadic return” operator converts a JSON Pointer path string into a raw C pointer (yyjson_mut_val *) within a given yyjson_mut_doc *. The path string is the human-readable identity of the JSON value; the raw pointer is the efficient operational handle.

Scope:
- Primary use within a single yyjson_mut_doc lifetime.
- Works on mutable values (yyjson_mut_val *).
- Integrates with C code (no dynamic dispatching required).
- No monadic combinators; resolve paths explicitly when needed.

Non-goals:
- Serialization of raw pointers.
- Cross-document or cross-process pointer portability.
- Garbage-collection or automatic lifetime extension.

## Terminology

- JSON Pointer: RFC 6901 string such as "/a/b/0" (with "~0" = "~", "~1" = "/").
- Target value: The yyjson_mut_val * resolved from a path within a document.
- Pointer handle value: A small struct that holds (doc, val) and optional metadata.

## Invariants and Safety

- A pointer handle’s yyjson_mut_val * is only valid while its associated yyjson_mut_doc remains alive.
- The document’s root JSON object carries an integer field "ref" for reference counting; creation increments it and release decrements it.
- When the "ref" count reaches zero, the implementation frees the underlying yyjson_mut_doc; pointers become invalid immediately thereafter.
- Operations that allocate more values (e.g., adding keys/elements) should not invalidate existing pointers in yyjson’s arena-based allocator, but the implementation must not assume relocations can never occur outside yyjson guarantees.
- Thread-safety: Pointer monad is not thread-safe. If used across threads, external synchronization of the underlying document is required.

## JSON Pointer Syntax Support

- RFC 6901 semantics:
  - Leading "/" denotes the root-based pointer traversal.
  - Reference tokens separated by "/".
  - "~1" decodes to "/", "~0" decodes to "~".
  - Object members addressed by key string tokens.
  - Array elements addressed by base-10 index tokens.
- Examples:
  - "/stack/0"
  - "/user/profile/name"
  - "/settings/features/experimental"

## Data Types

```c
/* The base yyjson_mut_doc is reference-counted via root object's "ref" field. */
typedef struct jpm_ptr {
    yyjson_mut_doc *doc;    /* underlying yyjson document; nonnull when valid */
    yyjson_mut_val *val;    /* target value pointer; nullable when invalid */
    const char     *path;   /* optional, may be NULL; original RFC 6901 path; owned by caller or separate allocator */
} jpm_ptr;

typedef enum jpm_status {
    JPM_OK = 0,
    JPM_ERR_INVALID_ARG,
    JPM_ERR_NOT_FOUND,
    JPM_ERR_TYPE,
    JPM_ERR_RANGE,
    JPM_ERR_INTERNAL
} jpm_status;

/* Function type used with bind/map-like combinators. */
typedef jpm_status (*jpm_fn)(jpm_ptr in, jpm_ptr *out, void *ctx);
```

Notes:
- `path` is optional and only for diagnostics/round-tripping to a human-readable identity. Raw pointers cannot be serialized; when serialization is needed, the path must be stored and used to re-resolve later.
- Implementations may copy the path into a separate allocator if persistence beyond the caller’s buffer is needed.

## API

Resolve:
```c
/* Resolve a JSON Pointer path into a pointer monad (doc + val).
   On success, retains the document (increments refcount). */
jpm_status jpm_return(yyjson_mut_doc *doc, const char *rfc6901_path, jpm_ptr *out);
```
- Preconditions: `doc != NULL`, `rfc6901_path != NULL`, `out != NULL`.
- Postconditions: On success, `out->doc == doc` and `out->val` points to the resolved target; `out->path` may point to `rfc6901_path` or NULL (implementation defined).
- Lifecycle: `jpm_return` increments the root object's "ref" field; callers must eventually call `jpm_ptr_release(...)` on the returned pointer to balance (which may free the document when the count reaches zero).
- Errors: `JPM_ERR_NOT_FOUND` if path does not exist; `JPM_ERR_INVALID_ARG` if inputs are NULL or path invalid.

Bind:
Removed. This design does not provide monadic combinators; resolve paths explicitly via jpm_return or dedicated helpers.

Map:
Removed. See above.

Destruction/release:
```c
/* Release a pointer monad; decrements the doc's refcount and NULLs fields.
   When the refcount reaches zero, frees the underlying yyjson_mut_doc. */
void jpm_ptr_release(jpm_ptr *p);

/* Optional explicit doc retain/release helpers. */
yyjson_mut_doc *jpm_doc_retain(yyjson_mut_doc *d);
void jpm_doc_release(yyjson_mut_doc *d);
```
- jpm_ptr is a small handle; copying by value does not change refcount. Only creation via jpm_return (and other factory APIs) retains, and jpm_ptr_release releases.
- Implementations should be idempotent when releasing a NULL/invalid pointer (no-op).

Validation/introspection:
```c
/* Check whether a monad currently references a concrete JSON value. */
bool jpm_is_valid(jpm_ptr p);

/* Optionally return the stored path (may be NULL). */
const char *jpm_path(jpm_ptr p);

/* Access raw C pointer; valid only while `p.doc` is alive. */
yyjson_mut_val *jpm_value(jpm_ptr p);
```

Re-encoding to a path (optional):
```c
/* If `p.path` is not tracked, implementations may attempt to derive a path; otherwise return JPM_ERR_NOT_FOUND. */
jpm_status jpm_to_path(jpm_ptr p, char *buf, size_t buf_len);
```
- Deriving a canonical JSON Pointer from an arbitrary yyjson_mut_val * is not always feasible without parent links; this function is optional and may rely on application-maintained indices/paths. If unsupported, return `JPM_ERR_NOT_FOUND`.

## Path Resolution Strategy

Preferred: Use yyjson’s JSON Pointer utilities.
- Resolve with mutable-pointer API when available, e.g., `yyjson_mut_ptr_get(root_val, "/a/b")`.
- If only document-level helpers exist, start from `yyjson_mut_doc_get_root(doc->doc)`.

Fallback: Manual RFC 6901 traversal.
- Tokenize by "/" (skip leading "").
- Decode "~0" and "~1" within tokens.
- For objects: lookup by key.
- For arrays: parse unsigned index and bounds-check.
- If a segment is missing or of incompatible type, return `JPM_ERR_NOT_FOUND` or `JPM_ERR_TYPE`.

## Error Semantics

- Functions must set `out->doc = doc_or_null`, `out->val = NULL` on error.
- Return the most specific error available:
  - Invalid arguments → `JPM_ERR_INVALID_ARG`
  - Path not found → `JPM_ERR_NOT_FOUND`
  - Type mismatch → `JPM_ERR_TYPE`
  - Index out of range → `JPM_ERR_RANGE`
  - Unexpected internal conditions → `JPM_ERR_INTERNAL`

## Interactions with Mutations

- If a target value is removed from the document (e.g., array pop, object key removal), any monads referencing that value become invalid; `jpm_is_valid()` should return false if detectably invalid.
- Adding siblings or unrelated allocations should not invalidate existing pointers in yyjson.
- Resolve as late as possible (via `jpm_return`) and keep pointer lifetimes short.

## Example Usage

Resolve and increment a numeric value at a path:
```c
jpm_ptr p;
if (jpm_return(doc, "/metrics/count", &p) == JPM_OK && jpm_is_valid(p)) {
    yyjson_mut_val *v = jpm_value(p);
    double cur = yyjson_mut_get_real(v);
    (void)cur;
    /* Replace or update `v` according to yyjson mutation APIs. */
}
jpm_ptr_release(&p);
```

Bind chaining to derive a sibling pointer:
```c
jpm_status select_sibling(jpm_ptr in, jpm_ptr *out, void *ctx) {
    (void)ctx;
    /* Example: from "/user" produce "/user/profile" */
    return jpm_return(in.doc, "/user/profile", out);
}

jpm_ptr start;
if (jpm_return(doc, "/user", &start) == JPM_OK) {
    jpm_ptr profile;
    if (jpm_bind(start, select_sibling, NULL, &profile) == JPM_OK) {
        /* use `profile` */
        jpm_ptr_release(&profile);
    }
    jpm_ptr_release(&start);
}
```

## Integration with JISP (optional guidance)

- Add an opcode `ptr_return` that:
  - Args: JSON array with one string RFC 6901 path, e.g., ["/user/profile"].
  - Effect: resolves to `yyjson_mut_val *` and pushes a pointer handle into a C-side pointer stack (separate from the JSON array-based stack), or attaches metadata to the JSON stack entry to mark it as a pointer handle (implementation-specific).
  - On failure: push a null pointer handle or signal error.

- Add opcodes for dereference and mutation:
  - `ptr_get`: reads the pointed value and pushes a deep copy into the JSON stack.
  - `ptr_set`: pops a JSON value and writes it into the pointed location (replacing target).

- Keep raw pointers out of serialized JSON; when printing, show the original path (if tracked), e.g., {"$ptr":"/user/profile"} for debugging only.

## Testing Strategy

- Positive:
  - Resolve nested object and array paths.
  - Chain binds across multiple path resolutions.
- Negative:
  - Non-existent keys and out-of-range indices.
  - Type mismatches.
  - Paths with "~0" and "~1" decoding.
- Lifetime:
  - Verify pointers remain valid across unrelated allocations.
  - Verify invalidation after explicit removal of the target value.

## Future Extensions

- Optional persistent association of `path` with `jpm_ptr` for round-trip printing/logging.
- Helper to compute a JSON Pointer for a value when parents are available (maintain parent links stack in app-level).
- Transactional semantics (batch operations with rollback).
- Const variants that use `yyjson_val *` for read-only docs.

## Minimal Header Sketch (non-binding)

```c
#ifndef JPM_PTR_H
#define JPM_PTR_H
#include "yyjson.h"
#include <stdbool.h>
#include <stddef.h>

/* Reference counting is tracked on the document's root object under key "ref". */
typedef struct jpm_ptr {
    yyjson_mut_doc *doc;
    yyjson_mut_val *val;
    const char     *path; /* optional; lifetime managed by caller or separate allocator */
} jpm_ptr;

typedef enum jpm_status {
    JPM_OK = 0,
    JPM_ERR_INVALID_ARG,
    JPM_ERR_NOT_FOUND,
    JPM_ERR_TYPE,
    JPM_ERR_RANGE,
    JPM_ERR_INTERNAL
} jpm_status;

/* Monadic combinators removed in the handle model. */

bool jpm_is_valid(jpm_ptr p);
const char *jpm_path(jpm_ptr p);
yyjson_mut_val *jpm_value(jpm_ptr p);

/* Retain/release APIs (manipulate root["ref"]) */
yyjson_mut_doc *jpm_doc_retain(yyjson_mut_doc *d);
void jpm_doc_release(yyjson_mut_doc *d);
void jpm_ptr_release(jpm_ptr *p);

/* Handle API */
jpm_status jpm_return(yyjson_mut_doc *doc, const char *rfc6901_path, jpm_ptr *out);

#endif /* JPM_PTR_H */
```

This spec defines the semantics and API surface to build a raw C pointer JSON monad over yyjson. The concrete implementation may live in a small C module (e.g., `jpm_ptr.c`) and optionally expose a JISP opcode layer to integrate with your existing stack-based execution model.
