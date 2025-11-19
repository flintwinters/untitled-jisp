# JISP Technical Manual

**Version:** 1.1  
**Date:** November 19, 2025  
**Context:** Embedded C JSON Runtime

## 1. Overview
JISP (JSON Instruction Stream Processor) is a stack-based virtual machine embedded within a single JSON document. It executes operations defined in the document to transform the document itself. It is built on top of the high-performance `yyjson` C library.

This manual describes the architecture, memory model, instruction set, and extension mechanisms.

## 2. Architecture

### 2.1. The Document as Memory
JISP does not have separate "RAM". The JSON document *is* the memory.
- **Root Object:** The execution context is the root object of the mutable `yyjson` document.
- **The Stack (`root["stack"]`):** A heterogeneous array used for passing arguments and return values.
- **Registers/Heap:** Any other key in the root object acts as a register or heap storage (e.g., `root["temp_sum"]`, `root["final_result"]`).

### 2.2. Execution Model
Execution is driven by processing an **Entrypoint Array** (`root["entrypoint"]`).
The interpreter iterates through this array, treating elements as instructions:
1.  **Literals:** Strings, Numbers, and Arrays are **deep-copied** and pushed onto the stack.
2.  **Directives:** Objects containing a special `.` key are treated as directives.
    - `{ ".": "op_name" }`: Invokes the opcode or macro named "op_name".
    - `{ ".": [...] }`: Executes the nested array as a subroutine.

## 3. Instruction Set (Opcodes)

Opcodes consume arguments from the top of `root["stack"]` (LIFO) and push results back.

### 3.1. Stack Manipulation
| Opcode | Stack (Before) -> (After) | Description |
| :--- | :--- | :--- |
| `duplicate_top` | `[..., A]` -> `[..., A, A']` | Duplicates the top value (deep copy). |
| `pop_and_store` | `[..., Val, Key]` -> `[...]` | Pops `Val` and `Key` (string). Stores `Val` at `root[Key]`. |

### 3.2. Arithmetic
| Opcode | Stack (Before) -> (After) | Description |
| :--- | :--- | :--- |
| `add_two_top` | `[..., A, B]` -> `[..., A+B]` | Pops two numbers, adds them, pushes result. |
| `calculate_final_result` | `[..., N]` -> `[..., N]` | Reads `root.temp_sum` and `root.temp_mult`, adds stack top `N`, writes to `root.final_result`. |

### 3.3. JSON Operations
| Opcode | Stack (Before) -> (After) | Description |
| :--- | :--- | :--- |
| `get` | `[..., Path]` -> `[..., Val]` | Resolves RFC 6901 `Path` (string) and pushes a deep copy of the value. |
| `set` | `[..., Val, Path]` -> `[...]` | Sets value at `Path` to `Val` (Scalar only). |
| `append` | `[..., Val, Path]` -> `[...]` | Appends `Val` to the array at `Path`. |

### 3.4. JSON Pointer Monad (JPM)
JISP maintains a separate, parallel **Pointer Stack** on the C side (not visible in JSON) for efficient manipulation of mutable values.

| Opcode | Stack (JSON) | Pointer Stack (C) | Description |
| :--- | :--- | :--- | :--- |
| `ptr_new` | `[..., Path]` -> `[...]` | `[...]` -> `[..., Ptr]` | Resolves `Path`, retains document, pushes handle to C stack. |
| `ptr_release` | `[...]` -> `[...]` | `[..., Ptr]` -> `[...]` | Pops and releases the top pointer handle. |
| `ptr_get` | `[...]` -> `[..., Val]` | `[..., Ptr]` (Peek) | Pushes a deep copy of the value at `Ptr` to the JSON stack. |
| `ptr_set` | `[..., Val]` -> `[...]` | `[..., Ptr]` (Peek) | Overwrites the value at `Ptr` with `Val` (Scalar only). |

### 3.5. Control Flow
| Opcode | Stack (Before) -> (After) | Description |
| :--- | :--- | :--- |
| `map_over` | `[..., DataArr, FuncArr]` -> `[..., ResArr]` | Applies `FuncArr` (entrypoint) to each item in `DataArr`. |

### 3.6. System / Debug
| Opcode | Description |
| :--- | :--- |
| `print_json` | Prints the current document state to stdout. |
| `undo_last_residual` | Reverses the last recorded mutation (see Section 5). |

### 3.5. Control Flow
| Opcode | Stack (Before) -> (After) | Description |
| :--- | :--- | :--- |
| `map_over` | `[..., DataArr, FuncArr]` -> `[..., ResArr]` | Applies `FuncArr` (entrypoint) to each item in `DataArr`. |
| `enter` | `[..., Target]` -> `[...]` | Transfers execution to `Target`. `Target` can be a path string (resolves to array) or an array literal. Pushes current frame path to `root["call_stack"]`. |
| `exit` | `[...]` -> `[...]` | Returns from the current execution frame. Pops from `root["call_stack"]`. |

### 3.6. Call Stack
JISP maintains a runtime call stack in `root["call_stack"]`.
- It is an array of strings (JSON Pointers to the execution frames).
- `process_entrypoint` and `enter` push to this stack.
- `exit` and function return pop from this stack.
- Useful for debugging and introspection.

### 3.7. System / Debug
| Opcode | Description |
| :--- | :--- |
| `print_json` | Prints the current document state to stdout. |
| `undo_last_residual` | Reverses the last recorded mutation (see Section 5). |

### 3.8. Testing
| Opcode | Stack (Before) -> (After) | Description |
| :--- | :--- | :--- |
| `test` | `[..., Prog, Expect]` -> `[..., "ERROR"?]` | Executes `Prog` (full JISP program doc) in a sandbox. Compares result against `Expect` (subset match). If mismatch, pushes "ERROR". |

## 4. Macro / Subroutine System
If an instruction is `{ ".": "name" }`:
1.  **Opcode Check:** Checks if "name" is a registered C opcode. If yes, executes it.
2.  **Macro Check:** Checks if `root["name"]` exists and is an array. If yes, executes that array as a subroutine.
3.  **Fallback:** If neither, treats the object as a literal and pushes it to the stack.

## 5. Residuals & Reversibility
JISP supports "Time Travel Debugging" (Undo) via JSON Patch (RFC 6902).

- **Activation:** Enabled if `root["is_reversible"]` is `true`.
- **Storage:** Patches are appended to `root["residual"]`.
- **Behavior:**
    - **Add/Append:** Recorded as `{"op": "add", ...}`.
    - **Remove:** Recorded as `{"op": "remove", ...}`. Value is captured for restoration.
    - **Replace:** Recorded as `{"op": "replace", ...}`.
- **Grouping:** Atomic operations (like `map_over`) group their patches so a single `undo_last_residual` reverts the entire logical step.

## 6. Example Program

```json
{
  "stack": [],
  "entrypoint": [
    "/stack/0",
    { ".": "ptr_new" },
    100,
    { ".": "ptr_set" },
    { ".": "ptr_release" },
    { ".": "print_json" }
  ]
}
```
**Execution:**
1. Push path string `"/stack/0"`.
2. `ptr_new`: Pops path, pushes pointer to `stack[0]` to C-stack.
3. Push `100`.
4. `ptr_set`: Pops `100`, overwrites value at pointer (stack[0]) with 100.
5. `ptr_release`: Clean up pointer.
6. `print_json`: Show result.

## 7. Error Handling & Diagnostics
JISP provides robust error reporting for both parsing and runtime failures.

- **Fatal Errors:** Aborts execution with a descriptive message and a JSON state snapshot.
- **Parse Errors:** Reports the exact byte offset, line, and column of invalid JSON input.
- **Stack Traces:** On fatal errors (assertion failures, segfaults, or explicit `jisp_fatal` calls), JISP prints a C-level stack trace using `bfd` (Binary File Descriptor) and `backtrace`. This resolves memory addresses to function names, source files, and line numbers for easier debugging.

## 8. Build & Dependencies
JISP depends on:
- `yyjson` (included)
- `libbfd` (binutils-dev) - For symbol resolution.
- `libdl` - For dynamic linking support.

To build:
```bash
python3 build.py
```