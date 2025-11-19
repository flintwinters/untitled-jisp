# JISP: JSON Instruction Stream Processor

JISP is a compact, embeddable stack machine runtime written in C. It treats a JSON document as both its memory and its instruction stream.

## Quick Start

### 1. Build
```bash
gcc -o jisp jisp.c yyjson.c -O3
```

### 2. Run
Create a file `test.json`:
```json
{
  "stack": [],
  "entrypoint": [
    "Hello World",
    "message",
    { ".": "pop_and_store" },
    { ".": "print_json" }
  ]
}
```

Run it:
```bash
./jisp test.json
```

## Documentation

Full technical documentation is available in [docs/JISP_MANUAL.md](docs/JISP_MANUAL.md).

- **Architecture:** How JISP uses JSON as memory.
- **Opcodes:** Complete instruction set reference.
- **Reversibility:** How to use the undo system.

## Key Features

*   **Zero-Parse Overhead:** Instructions are just JSON arrays; no separate bytecode parsing step (beyond standard JSON parsing).
*   **Introspection:** The entire machine state (stack, registers, heap) is inspectable at any time by printing the document.
*   **Residual Tracking:** Built-in support for recording mutations via JSON Patch for undo/redo functionality.
