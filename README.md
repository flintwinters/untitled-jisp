# JISP: JSON Interpreted Stack-based Programming (C Implementation)

JISP is a proof-of-concept for a programming language where the program is a sequence of functions operating on a mutable JSON data structure. The name is a portmanteau of JSON and LISP, reflecting its data-centric nature and the "code-as-data" paradigm, with operational semantics similar to stack-based languages like Forth.

This is a C implementation using the `yyjson` library.

## Core Concepts

The execution model of JISP revolves around three main components:

1.  **The Data Object**: A mutable JSON document (`yyjson_mut_doc`) that holds the entire state of the program. By convention, it contains a `stack` for stack-based operations, but can hold any other data.

2.  **The Function Array**: A C array of `jisp_instruction` structs that represents the program. Each instruction contains a function pointer and an optional JSON string for arguments.

3.  **The Processor**: The `process_functions` function iterates through the instruction array, applying each function to the data object sequentially.

## Core "Opcodes"

The `jisp.c` implementation provides several built-in functions that act as the language's core operations:

*   `push_value(doc, args)`: Pushes a value from `args` onto the `stack`.
*   `pop_and_store(doc, args)`: Pops a value from the `stack` and stores it in the data object under a key specified in `args`.
*   `duplicate_top(doc, args)`: Duplicates the top value of the `stack`.
*   `add_two_top(doc, args)`: Pops the top two values from the `stack`, adds them, and pushes the result back onto the stack.
*   `calculate_final_result(doc, args)`: A custom function that reads values from the data object and stack, performs a calculation, and stores the result.
*   `print_json(doc, args)`: Prints the current state of the JSON data object.

## Example Walkthrough

The `jisp.c` `main` function provides an example of how these components work together.

1.  An initial `yyjson_mut_doc` is created as the starting state.
2.  `add_block` is an array of instructions that pushes 10 and 20 to the stack, adds them, and stores the result (30) in `temp_sum`.
3.  `duplicate_and_store_block` is another instruction array that pushes 50, duplicates it, and stores one of the 50s in `temp_mult`.
4.  The `main` function acts as the main program:
    *   It first executes `add_block` and `duplicate_and_store_block` by calling `process_functions`.
    *   It then executes a final block of instructions to push another `10`, calculate the `final_result` by summing `temp_sum`, `temp_mult`, and the remaining value on the stack.
    *   Finally, it prints the final JSON state.

## How to Build and Run

You will need a C compiler (like GCC or Clang) and `make`. The project assumes `yyjson.c` and `yyjson.h` are in the project root directory.

To build the executable:
```bash
make
```

To run the program:
```bash
./jisp
```

The script will print the final state of the JSON data object after execution.
