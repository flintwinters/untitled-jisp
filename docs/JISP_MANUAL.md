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
