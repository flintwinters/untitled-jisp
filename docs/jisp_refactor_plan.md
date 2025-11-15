# JISP architecture and refactor status (token union, implicit-arg, JPM, yyjson)

This document reflects the current, implemented state of the JISP runtime in C (see jisp.c) and the remaining TODOs. The original “refactor plan” has been largely completed and the runtime now runs on:
- A token-union instruction stream with an implicit-argument stack in JSON.
- A small set of stack-based ops.
- A JSON Pointer handle (JPM) for pointer semantics with document-level reference counting.

The sections below describe what is implemented, current semantics, and what remains.

## Implemented

1) Token union and interpreter
- Types: enum jisp_tok_kind { JISP_TOK_OP, JISP_TOK_INT, JISP_TOK_REAL, JISP_TOK_STR, JISP_TOK_JPM } and struct jisp_tok { kind; union { jisp_op op; int64_t i; double d; const char *s; const jpm_ptr *ptr; } as; }.
- Interpreter: void run_tokens(yyjson_mut_doc *doc, const jisp_tok *toks, size_t count).
- Behavior today:
  - INT → yyjson_mut_arr_add_sint(root["stack"], i).
  - REAL → yyjson_mut_arr_add_real(root["stack"], d).
  - STR → push a string into root["stack"] (copied into the doc).
  - OP → call function pointer jisp_op(doc).
  - JPM → not yet supported in run_tokens; attempting to run a JPM token is a fatal error (by design for now).

2) Op signature and registry
- Signature: typedef void (*jisp_op)(yyjson_mut_doc *doc).
- Registry: a small JSON document maps op names to numeric IDs; jisp_op_from_id(int) returns the function pointer. Global init/fini helpers are present.

3) Stack-based ops (arguments/results are on root["stack"])
- pop_and_store:
  - Requires at least two stack items [value, key] (topmost is key).
  - Key must be a string; value is stored to root[key].
- duplicate_top:
  - Pops the top, pushes it back, then pushes a deep-copy of it; net +1 element.
- add_two_top:
  - Pops two numeric values, pushes their sum as a real.
- calculate_final_result:
  - Reads temp_sum, temp_mult from root if present, optionally pops a numeric from stack, and writes root["final_result"].
- print_json:
  - Pretty-prints the document to stdout.

All ops validate inputs and use jisp_fatal(...) on errors. Fatal output includes a pretty-printed snapshot of the current JSON state.

4) Entry-point processing (process_entrypoint)
- Input: root["entrypoint"] array.
- Semantics:
  - String elements are treated as string literals and pushed on the stack (no op dispatch).
  - Numeric elements are pushed as numbers.
  - Array elements are deep-copied and pushed as array literals (no implicit call).
  - Object elements are pushed as literals, and additionally:
    - If the object contains a field ".":
      • If "." is an array: that array is treated as a nested entrypoint and is processed recursively.
      • If "." is a string: it is treated as an op name; if found in the registry, the op is invoked immediately; if not found, the object remains just a literal.
    - Other cases are rejected with a fatal error.
- Note: This differs from the original plan which proposed “string → call op.” The current implementation treats plain strings as literals; op dispatch via entrypoint is only through an object with a "." field that is a string.

5) Deep-copy helper
- jisp_mut_deep_copy(doc, val) deep-copies a yyjson_mut_val within the same document; used by duplicate_top and entrypoint literal handling.

6) JSON Pointer Handle (JPM)
- Data types: jpm_ptr { doc, val, path }, jpm_status enum; no combinators.
- Path resolution: jpm_return(doc, "/rfc6901/path", &out)
  - Uses yyjson_mut_ptr_get for RFC 6901 semantics, including "~0" and "~1" decoding.
  - On success, retains the document and returns a handle to the target yyjson_mut_val.
- Lifecycle: jpm_ptr_release(&p) decrements the refcount and nulls the handle.
- Helpers: jpm_is_valid, jpm_path, jpm_value.
- Tests in main() exercise success and failure cases, including nested objects, arrays, and escape sequences.

7) Document reference counting via root["ref"]
- Helpers: jpm_doc_retain / jpm_doc_release manipulate an integer field root["ref"] and free the document when it reaches zero.
- jpm_return retains on success; jpm_ptr_release releases.
- Current program end still uses yyjson_mut_doc_free(doc) directly; see TODOs.

8) Error handling and diagnostics
- jisp_fatal(...) and jisp_fatal_parse(...) print descriptive messages and a JSON snapshot; source position reporting is included for input parse failures.

9) End-to-end flow in main
- Reads JSON from file, parses to immutable yyjson_doc, copies to yyjson_mut_doc as root, runs token sequences, processes entrypoint, prints final JSON, performs assertions for retain/release and JPM, then frees resources.

## Behavioral differences from the original plan

- Entry-point strings:
  - Original plan: string "op_name" → call op.
  - Current implementation: string is treated as a literal; only objects with a "." string dispatch ops.
- JPM tokens:
  - Plan: interpreter pushes a shallow pointer handle to a pointer stack.
  - Current implementation: run_tokens does not support JPM tokens; encountering one is a fatal error.

## Remaining work / TODOs

- JPM tokens in run_tokens:
  - Add a pointer stack (C-side) and support JISP_TOK_JPM in run_tokens.
  - Decide on API surface for mixing pointer-stack values with JSON-stack ops (e.g., dedicated ops for ptr_get/ptr_set).
- Entry-point semantics:
  - Decide whether to also support plain string → op dispatch (backwards-compatible extension) or keep the current “strings are literals; only object '.' dispatches ops” rule.
- Reference counting cleanup:
  - Replace direct yyjson_mut_doc_free(...) with jpm_doc_release(...) at final shutdown, and ensure creation sites perform balanced retains if needed.
- Optional residual patch logging:
  - The separate plan in docs/json_patch_residual_plan.md can be implemented to record minimal JSON Patch operations when root["is_reversible"] is true.
- Additional tests:
  - JPM tokens (once implemented), more negative-path tests for entrypoint object "." cases, and refcount behavior across failures.

## Migration/usage notes

- No jisp_instruction objects are used anymore; token arrays are the C-side authoring mechanism.
- Ops consume and produce values on root["stack"] only; argument order is LIFO.
- Raw jpm_ptr handles are not serialized; keep them on the C side only.
- Deep copies are performed when pushing literals to preserve isolation from later mutations.

This document supersedes the older refactor plan and describes the authoritative current behavior and the short list of open tasks.
