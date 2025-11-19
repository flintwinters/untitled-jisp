### 3.8. Testing
| Opcode | Stack (Before) -> (After) | Description |
| :--- | :--- | :--- |
| `test` | `[..., Prog, Expect]` -> `[..., "ERROR"?]` | Executes `Prog` (full JISP program doc) in a sandbox. Compares result against `Expect` (subset match). If mismatch, pushes "ERROR". |