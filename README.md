# JISP: JSON Stack Machine Interpreter

JISP (JSON Stack-based Interpreter) is a C-language interpreter that processes JSON-defined programs. It leverages the `yyjson` library for efficient JSON manipulation and features a stack-based execution model.

## Features

*   **JSON-driven Logic:** Programs are defined as JSON structures, enabling flexible and data-centric control flow.
*   **Stack Machine:** Operates on a `stack` array within the JSON document for data manipulation.
*   **Reversibility (Undo):** Records changes as JSON Patch operations (`residual` array) for best-effort undo functionality.
*   **JSON Pointer Monad:** Basic support for JSON Pointer (RFC 6901) resolution.
*   **Extensible Opcodes:** Supports custom operations (e.g., `add_two_top`, `pop_and_store`).
*   **Entrypoint Execution:** Processes a main `entrypoint` array, supporting nested calls and execution context tracking.
*   **Error Handling:** Includes robust error reporting with state snapshots and source position.

## Usage

To run a JISP program, compile `jisp.c` and provide a JSON file as an argument:

```bash
./jisp <your_program>.json
```

The interpreter will load the JSON, execute the `entrypoint` array, and print the final JSON state to standard output.

## Dependencies

*   `yyjson`: A high-performance JSON library for C.