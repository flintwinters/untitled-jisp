# JSON Patch Residual Tracking Plan (yyjson)

Goal
- Optionally track every mutation caused by a JISP op or by implicit pushes in `process_entrypoint`, using JSON Patch (RFC 6902) objects stored directly in the global JSON document.
- Two new root-level fields:
  - "residual": an array of JSON Patch operations (objects).
  - "is_reversible": boolean; when true, mutations append patch objects to "residual", when false, no tracking is recorded.

Why JSON Patch
- JSON Patch is a compact, standardized format for expressing changes to a JSON document.
- yyjson already includes JSON Patch utilities; we rely only on the JSON structure (no immediate need to call an applier/patcher at runtime for logging).
- Forward patches allow auditing and optional replay on another document. Inversion (undo) can be supported later with minimal additions.

Data model
- root["residual"]: array of patch objects like:
  - { "op": "add", "path": "/stack/-", "value": 10 }
  - { "op": "replace", "path": "/temp_sum", "value": 30 }
  - { "op": "remove", "path": "/user/profile/age" }
- root["is_reversible"]: boolean gate to enable or disable residual logging.

Recording guidelines
- Only record when root["is_reversible"] is true.
- The actor (op/interpreter) knows the exact edit; write a minimal patch object that describes the edit performed.
- Use RFC 6901 paths for "path" and "from" fields.
- Use "add" to append/push (e.g., "/stack/-"), or to create a new key in an object.
- Use "replace" when overwriting an existing key or array element.
- Use "remove" when deleting an existing key or array element.
- Optional future: support "move", "copy", "test" for richer semantics when needed.

Integration points
1) process_entrypoint(doc)
   - When pushing a literal number/string/array onto the stack:
     - Append a patch: { "op": "add", "path": "/stack/-", "value": <deep-copied literal> }.
   - If in the future entrypoint can trigger ops by string names inline, those ops will record their own patches.

2) Opcodes
   - pop_and_store:
     - Determine if root has an existing key with that name.
       • If absent: record { "op": "add", "path": "/<key>", "value": <value> }.
       • If present: record { "op": "replace", "path": "/<key>", "value": <value> }.
   - duplicate_top:
     - Net effect: one new top value added to stack.
       • Record { "op": "add", "path": "/stack/-", "value": <deep-copied top> }.
     - (Internal pop/push reordering is implementation detail; only record the observable addition.)
   - add_two_top:
     - Pops two numeric values and pushes their sum.
       • Record the push: { "op": "add", "path": "/stack/-", "value": <sum> }.
       • Optionally also record the removals if needed for full operation trace:
         { "op": "remove", "path": "/stack/<idx>" } twice, before the add.
         For a minimal audit, the final push is often sufficient; see “Exactness vs. minimality” below.
   - calculate_final_result:
     - Writes/overwrites root["final_result"]:
       • { "op": "add" or "replace", "path": "/final_result", "value": <number> }.

Helper functions (to be implemented)
- ensure_residual_array(doc):
  - Ensures root["residual"] exists and is an array; creates if missing.
  - Returns the array node or NULL on failure.
- is_reversible_enabled(doc):
  - Reads root["is_reversible"] (defaults to false if not present).
- record_patch_add(doc, path, value_mut):
  - If is_reversible_enabled(doc):
    - Append {"op":"add","path":path,"value":deep_copy(value_mut)} to root["residual"].
- record_patch_replace(doc, path, value_mut):
  - If enabled, append {"op":"replace","path":path,"value":deep_copy(value_mut)}.
- record_patch_remove(doc, path):
  - If enabled, append {"op":"remove","path":path}.
- record_patch_add_number/real/string convenience overloads where helpful.

Path building
- Use RFC 6901-encoded paths:
  - Encode "~" as "~0" and "/" as "~1" in keys.
  - For stack pushes, prefer "/stack/-" to append to end.
- For object keys held in mutable values, duplicate/encode key strings before building the JSON Patch object.

Exactness vs. minimality
- Minimal approach (first phase):
  - Log only the externally visible net effects:
    • Adds to stack or document fields.
    • Replacements for overwritten keys.
  - Skips intermediate pops/pushes that do not persist in the final state.
- Exact approach (optional second phase):
  - Log every atomic step as it occurs (including array element removals by index).
  - Enables more precise replay and potential inversion but adds verbosity.
- The plan starts with the minimal approach for clarity; exact logging can be enabled behind a flag later if needed.

Reversibility considerations (future)
- Forward JSON Patch alone may not be sufficient to undo without pre-change values (e.g., for "remove" and "replace").
- Extensions:
  - Store “prev” snapshots on replace/remove: {"op":"replace",...,"prev":<old>} or record an "undo" companion patch next to each forward patch.
  - Or, concurrently accumulate an “undo_residual” array with inverse patches.
- For now, the "is_reversible" flag enables forward patches only; undo is a planned extension.

Examples
- Pushing 10 via entrypoint:
  - residual += { "op": "add", "path": "/stack/-", "value": 10 }
- Storing sum under key "temp_sum":
  - If previously absent: residual += { "op": "add", "path": "/temp_sum", "value": 30 }
  - If present: residual += { "op": "replace", "path": "/temp_sum", "value": 30 }

Operational notes
- Recording should not mutate user data except for creating/initializing root["residual"] when needed.
- Deep-copy values into the patch entry to avoid aliasing pointers that may later change.
- Logging must be best-effort and never crash the process; on OOM, skip recording rather than failing the op.

Next steps
1) Add helpers for residual array management and patch object construction.
2) Wire the helpers into:
   - process_entrypoint pushes.
   - pop_and_store, duplicate_top, add_two_top, calculate_final_result.
3) Add tests:
   - When is_reversible=false: residual remains absent or unchanged.
   - When is_reversible=true: residual collects the expected patches.
4) Optional: add a debug flag to also print residual after execution for inspection.

Out-of-scope (for now)
- Automatic application of recorded patches.
- Undo/inversion logic.
- Concurrent mutation tracking across multiple documents.
