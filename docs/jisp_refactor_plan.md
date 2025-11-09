Refactor plan: implicit-arg, stack-based JISP with token union (no jisp_instruction)

Understanding
- Eliminate jisp_instruction { op, args_json } and the per-instruction JSON parsing.
- Introduce a lightweight C token union to represent an instruction stream: tokens can be one of:
  - OP: a jisp_op function pointer
  - INT/REAL/STR: literal values
  - VAL: a const yyjson_val* literal to be deep-copied into the doc and pushed
- Interpreter semantics:
  - Literal tokens push a corresponding yyjson value onto root["stack"].
  - OP tokens invoke the function with signature void jisp_op(yyjson_mut_doc *doc).
  - All op arguments are implicitly consumed from the top of root["stack"] (LIFO). Ops push results back on the stack when applicable.
- Entry-point JSON (runtime-provided) remains supported:
  - string "op_name" → call op
  - number → push number
  - array → push the array value as a single literal onto the stack (no implicit call)
  - This removes explicit args plumbing; ops read their own args from the stack.

API/Code changes
1) Change op signature and registry
   - typedef void (*jisp_op)(yyjson_mut_doc *doc);  // remove args parameter
   - Update all existing ops: push_value will be removed; pop_and_store, duplicate_top, add_two_top, calculate_final_result, print_json adapted to stack-args.

2) Introduce token union and interpreter for C-authored sequences
   - enum jisp_tok_kind { JISP_TOK_OP, JISP_TOK_INT, JISP_TOK_REAL, JISP_TOK_STR, JISP_TOK_VAL };
   - struct jisp_tok { kind; union { jisp_op op; int64_t i; double d; const char *s; const yyjson_val *v; } as; };
   - void run_tokens(yyjson_mut_doc *doc, const jisp_tok *toks, size_t count);
   - Behavior:
     - INT → yyjson_mut_arr_add_sint
     - REAL → yyjson_mut_arr_add_real
     - STR → duplicate into doc and push as string
     - VAL → deep-copy referenced yyjson_val into doc and push
     - OP → call function

3) Rewrite process_entrypoint(doc)
   - Iterate root["entrypoint"].
   - If elem is string → resolve op by name and invoke it.
   - If elem is number → push it.
   - If elem is array → deep-copy the array and push it onto the stack (treat as a literal; do not invoke).
   - Remove args JSON building/serialization.

4) Update ops to use stack for args/results
   - pop_and_store expects: [value, key] on stack top; pops key (string), pops value, sets root[key] = value.
   - duplicate_top expects: [x]; pops x, pushes x, pushes deep copy of x.
   - add_two_top expects: [a, b]; pops b, pops a; both numeric; pushes (a+b).
   - calculate_final_result: unchanged semantics but no args; it may pop from stack if needed (as current code already peeks/pops).
   - print_json: unchanged semantics; ignores stack.

5) Remove legacy push_value op usage in C paths
   - Replace C-built instruction arrays with token arrays:
     • add_block: INT 10, INT 20, OP add_two_top, STR "temp_sum", OP pop_and_store
     • duplicate_and_store_block: INT 50, OP duplicate_top, STR "temp_mult", OP pop_and_store
     • main_part2: INT 10, OP calculate_final_result
   - Keep entrypoint backwards compatible for numbers and string op names; for inline op args use array form.

6) Error handling
   - If an op needs N args but fewer are on stack → jisp_fatal with descriptive message.
   - Type mismatches (e.g., key must be string, numbers for arithmetic) → jisp_fatal.
   - Maintain existing pretty JSON dump on fatal.

7) Tests/Asserts adjustment
   - Remove references to args_json and jisp_instruction.
   - Validate stack sizes before/after ops as appropriate.
   - Keep JPM tests unchanged (orthogonal).

Migration impact
- C-defined instruction sequences change to token arrays.
- Entrypoint JSON still works for existing simple cases (strings and numbers). For ops needing explicit args within entrypoint, use ["op_name", ...args] form.

Next steps (upon approval)
- Implement typedef/signature changes and registry updates.
- Add token union and run_tokens().
- Rewrite ops to use the stack for inputs.
- Replace process_functions() with run_tokens() and refactor process_entrypoint().
- Update main() call sites to use token arrays.
- Remove push_value from the registry (or keep as transition alias that reads one literal from the stack if needed temporarily).

Please confirm this plan or indicate any deviation (e.g., keep push_value as compatibility op, support additional literal types in entrypoint, or different arg ordering on the stack).
