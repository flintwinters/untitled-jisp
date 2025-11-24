# Untitled JISP Language

**Version:** 1.2  
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
- **Registers/Heap:** Any other key in the root object acts as a register or heap storage.

### 2.2. Execution Model
Execution is driven by processing an **Entrypoint Array** (`root["entrypoint"]`).
The interpreter iterates through this array, treating elements as instructions:
1.  **Literals:** Strings, Numbers, and Arrays are **deep-copied** and pushed onto the stack.
2.  **Directives:** Objects containing a special `.` key are treated as directives.
    - `{ ".": "op_name" }`: Invokes the opcode or macro named "op_name".
    - `{ ".": [...] }`: Executes the nested array as a subroutine.

### 2.3. Execution Structure
The execution structure of a JISP program is inherently hierarchical, mirroring the JSON document structure. While the main execution loop processes a linear array of instructions (the "Stream"), control flow operations like `enter` or macro invocations can push new arrays onto the execution context. This creates a natural mapping between JSON nesting and the runtime call stack. Unlike traditional flat assembly languages that rely on `JMP` instructions and labels for control flow, JISP leverages structural nesting (arrays within arrays) to define scopes and subroutines. This design ensures that the program structure is visual, self-contained, and directly manipulable as data.

### 2.4. Design Philosophy
- **Unified State:** Code, data, stack, and registers reside in one serializable JSON object. This allows `print_json` to produce a complete, restorable snapshot of the machine state at any point, facilitating "Time Travel Debugging".
- **Safety & Diagnostics:** JISP prioritizes fail-fast behavior. Invalid paths or types trigger fatal errors with C-level stack traces (via `libbfd`) and JSON state dumps, rather than undefined behavior.
- **Hidden Pointer Stack (JPM):** To efficiently manipulate mutable values without constant path resolution, JISP employs a hidden C-side pointer stack (`ptr_new`, `ptr_get`, `ptr_set`). This "pins" values for efficient modification.

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
| `add_two_top` | `[..., A, B]` -> `[..., A+B]` | Pops two numbers, adds them, pushes result. Supports residual grouping. |

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
| `map_over` | `[..., DataArr, FuncArr]` -> `[..., ResArr]` | Applies `FuncArr` (entrypoint) to each item in `DataArr`. Pushes result array. |
| `enter` | `[..., Target]` -> `[...]` | Transfers execution to `Target`. `Target` can be a path string (resolves to array) or an array literal. Pushes current frame path to `root["call_stack"]`. |
| `exit` | `[...]` -> `[...]` | Returns from the current execution frame. Pops from `root["call_stack"]`. |

### 3.6. System / Debug
| Opcode | Stack (Before) -> (After) | Description |
| :--- | :--- | :--- |
| `print_json` | `[...]` -> `[...]` | Prints the current document state to stdout. |
| `print_error` | `[..., ErrObj]` -> `[...]` | Pretty-prints a JISP error object (popped from stack). |
| `undo_last_residual` | `[...]` -> `[...]` | Reverses the last recorded mutation. |

### 3.7. Testing
| Opcode | Stack (Before) -> (After) | Description |
| :--- | :--- | :--- |
| `test` | `[..., Prog, Expect]` -> `[...]` or `[..., Err]` | Executes `Prog` in a sandbox. Compares result against `Expect` (subset match). If mismatch, pushes an Error Object. |

### 3.8. I/O Operations
| Opcode | Stack (Before) -> (After) | Description |
| :--- | :--- | :--- |
| `load` | `[..., Path]` -> `[..., JSONVal]` | Reads JSON file at `Path` and pushes its root value. Fatals on error. |
| `store` | `[..., Val, Path]` -> `[...]` | Writes `Val` to a JSON file at `Path`. Fatals on error. |

## 4. Macro / Subroutine System
If an instruction is `{ ".": "name" }`:
1.  **Opcode Check:** Checks if "name" is a registered C opcode. If yes, executes it.
2.  **Macro Check:** Checks if `root["name"]` exists and is an array. If yes, executes that array as a subroutine.
3.  **Fallback:** If neither, treats the object as a literal and pushes it to the stack.

# 5. Residuals & Reversibility
JISP supports "Time Travel Debugging" (Undo) via JSON Patch (RFC 6902).

- **Activation:** Enabled if `root["is_reversible"]` is `true`.
- **Storage:** Patches are appended to `root["residual"]`.
- **Behavior:**
    - **Add/Append:** Recorded as `{"op": "add", ...}`.
    - **Remove:** Recorded as `{"op": "remove", ...}`. Value is captured for restoration.
    - **Replace:** Recorded as `{"op": "replace", ...}`.
- **Grouping:** Operations like `map_over` or `add_two_top` group their patches. A single `undo_last_residual` reverts the entire logical step (e.g., reversing the entire map operation or the arithmetic).

## 6. Call Stack
The Call Stack is a critical component of JISP's introspection capabilities, maintained as a JSON array of strings at `root["call_stack"]`. Unlike opaque internal stacks in traditional VMs, this structure is fully visible and serializable within the document. Each entry is a JSON Pointer that identifies the path of the active execution frame (e.g., `/entrypoint`, `/my_macro`, or `/nested/logic/0/.`). When a new instruction array is entered—whether through the main entrypoint, a macro expansion, or the `enter` opcode—its path is pushed to this list. When the frame completes or an `exit` opcode is encountered, the path is popped. This design ensures that a `print_json` or fatal error dump provides an immediate, human-readable trace of the program's execution flow up to that point.

## 7. Example Program

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

## 8. Error Handling & Diagnostics
JISP provides robust error reporting for both parsing and runtime failures.

- **Fatal Errors:** Aborts execution with a descriptive message and a JSON state snapshot.
- **Parse Errors:** Reports the exact byte offset, line, and column of invalid JSON input.
- **Stack Traces:** On fatal errors, JISP prints a C-level stack trace using `bfd` and `backtrace`. This resolves memory addresses to function names, source files, and line numbers.

## 9. Build & Dependencies
JISP depends on:
- `yyjson` (included)
- `libbfd` (binutils-dev) - For symbol resolution.
- `libdl` - For dynamic linking support.

# To build:
```bash
python3 build.py
```

## 10. TODO:
- general polishing
- improve web ui, add undo, redo, inputs, json editing etc.
- add actually usable op codes, branching, conditionals, more math ops etc.
- improve compatibility with https://jqlang.org/ , advertise to current users.
- improve testing methodology - this will be absolutely imperative for AI coders.
- integrate AI tool calls so AI coding assistants can use the debugging system
- improve reversibility/undo debugging robustness
- package manager for jisp

- custom AI coder, vector db + RAG of all functions with documentation descriptions.
  - Most reminiscent of the Aider AI coder.
  - The coder will automatically manage git commits and branches, and try to achieve a lot of the heavy listing with RAG/vectordb and finetuning specifically for jisp, rather than the bullshit guesswork a lot of agents currently do.

- a vscode plugin to enable embedded syntax highlighting, as a superset of json syntax.
  - `"sql_query": "sql SELECT * FROM table_name "` (note the `sql` after the quote, acting as a markdown codewell) would enable syntax highlighting for SQL within the string.
  - Languages to add syntax support for: `python`, `shell`, `sqlite`, `markdown`, `json`, `c`.

- expose LLVM C API as jisp ops, letting jisp code build IR.

- expose raylib bindings to make games

### License

I maintain the MIT license from the original yyjson project.

I add GPL-2.0 for robust open source support.