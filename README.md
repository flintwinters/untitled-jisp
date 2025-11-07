# JISP: JSON Interpreted Stack-based Programming

JISP is a proof-of-concept for a programming language where the program is a sequence of functions operating on a mutable JSON-like data structure. The name is a portmanteau of JSON and LISP, reflecting its data-centric nature and the "code-as-data" paradigm, with operational semantics similar to stack-based languages like Forth.

## Core Concepts

The execution model of JISP revolves around three main components:

1.  **The Data Object**: A Python dictionary (analogous to a JSON object) that holds the entire state of the program. By convention, it contains a `stack` for stack-based operations, but can hold any other data.

2.  **The Function Array**: A Python list that represents the program. Each element is either:
    *   A function that takes the current data object as its only argument and returns the modified data object.
    *   A tuple where the first element is a function and the subsequent elements are its arguments (the data object is always passed as the first argument).

3.  **The Processor**: The `process_functions` function iterates through the function array, applying each function to the data object sequentially and updating the data object with the return value of each function.

## Core "Opcodes"

The `jisp.py` implementation provides several built-in functions that act as the language's core operations:

*   `push_value(data, value)`: Pushes a `value` onto the `stack`.
*   `pop_and_store(data, key)`: Pops a value from the `stack` and stores it in the data object under the given `key`.
*   `duplicate_top(data)`: Duplicates the top value of the `stack`.
*   `add_two_top(data)`: Pops the top two values from the `stack`, adds them, and pushes the result back onto the stack.

## Control Flow & Modularity

*   `exec_sequence(data, process_fn, function_subarray)`: This function allows for executing a nested function array (a "subroutine"). This is the primary way to structure and reuse code.

## Advanced Features

*   `eval_code(data, code_string)`: Executes an arbitrary Python code string. The data object is provided as the local scope, allowing the Python code to read and modify the program's state directly. This is a powerful but potentially unsafe feature, similar to `eval` in many languages.
*   `lambda` functions can be used directly in the function array for simple, one-off operations.

## Example Walkthrough

The `jisp.py` script provides an example of how these components work together.

1.  `initial_json` is defined as the starting state.
2.  `add_block` is a function array that pushes 10 and 20 to the stack, adds them, and stores the result (30) in `temp_sum`.
3.  `duplicate_and_store_block` pushes 50, duplicates it, and stores one of the 50s in `temp_mult`.
4.  `main_function_list` is the main program:
    *   It first executes `add_block` and `duplicate_and_store_block`.
    *   It pushes another `10` to the stack.
    *   It uses `eval_code` to calculate the `final_result` by summing `temp_sum`, `temp_mult`, and the remaining value on the stack.
    *   It prints the final JSON state.
    *   Finally, it enters a simple REPL using `eval(input('>'))`.

## How to Run

Simply execute the Python script:

```bash
python jisp.py
```

The script will print the final state of the JSON data object after execution and then wait for user input.
