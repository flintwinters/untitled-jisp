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

## `yyjson` Usage Examples

The following examples demonstrate common operations with the `yyjson` library.

### Read JSON string
```c
const char *json = "{\"name\":\"Mash\",\"star\":4,\"hits\":[2,2,1,3]}";

// Read JSON and get root
yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
yyjson_val *root = yyjson_doc_get_root(doc);

// Get root["name"]
yyjson_val *name = yyjson_obj_get(root, "name");
printf("name: %s\n", yyjson_get_str(name));
printf("name length:%d\n", (int)yyjson_get_len(name));

// Get root["star"]
yyjson_val *star = yyjson_obj_get(root, "star");
printf("star: %d\n", (int)yyjson_get_int(star));

// Get root["hits"], iterate over the array
yyjson_val *hits = yyjson_obj_get(root, "hits");
size_t idx, max;
yyjson_val *hit;
yyjson_arr_foreach(hits, idx, max, hit) {
    printf("hit%d: %d\n", (int)idx, (int)yyjson_get_int(hit));
}

// Free the doc
yyjson_doc_free(doc);

// All functions accept NULL input, and return NULL on error.
```

### Write JSON string
```c
// Create a mutable doc
yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
yyjson_mut_val *root = yyjson_mut_obj(doc);
yyjson_mut_doc_set_root(doc, root);

// Set root["name"] and root["star"]
yyjson_mut_obj_add_str(doc, root, "name", "Mash");
yyjson_mut_obj_add_int(doc, root, "star", 4);

// Set root["hits"] with an array
int hits_arr[] = {2, 2, 1, 3};
yyjson_mut_val *hits = yyjson_mut_arr_with_sint32(doc, hits_arr, 4);
yyjson_mut_obj_add_val(doc, root, "hits", hits);

// To string, minified
const char *json = yyjson_mut_write(doc, 0, NULL);
if (json) {
    printf("json: %s\n", json); // {"name":"Mash","star":4,"hits":[2,2,1,3]}
    free((void *)json);
}

// Free the doc
yyjson_mut_doc_free(doc);
```

### Read JSON file with options
```c
// Read JSON file, allowing comments and trailing commas
yyjson_read_flag flg = YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS;
yyjson_read_err err;
yyjson_doc *doc = yyjson_read_file("/tmp/config.json", flg, NULL, &err);

// Iterate over the root object
if (doc) {
    yyjson_val *obj = yyjson_doc_get_root(doc);
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(obj, &iter);
    yyjson_val *key, *val;
    while ((key = yyjson_obj_iter_next(&iter))) {
        val = yyjson_obj_iter_get_val(key);
        printf("%s: %s\n", yyjson_get_str(key), yyjson_get_type_desc(val));
    }
} else {
    printf("read error (%u): %s at position: %ld\n", err.code, err.msg, err.pos);
}

// Free the doc
yyjson_doc_free(doc);
```

### Write JSON file with options
```c
// Read the JSON file as a mutable doc
yyjson_doc *idoc = yyjson_read_file("/tmp/config.json", 0, NULL, NULL);
yyjson_mut_doc *doc = yyjson_doc_mut_copy(idoc, NULL);
yyjson_mut_val *obj = yyjson_mut_doc_get_root(doc);

// Remove null values in root object
yyjson_mut_obj_iter iter;
yyjson_mut_obj_iter_init(obj, &iter);
yyjson_mut_val *key, *val;
while ((key = yyjson_mut_obj_iter_next(&iter))) {
    val = yyjson_mut_obj_iter_get_val(key);
    if (yyjson_mut_is_null(val)) {
        yyjson_mut_obj_iter_remove(&iter);
    }
}

// Write the json pretty, escape unicode
yyjson_write_flag flg = YYJSON_WRITE_PRETTY | YYJSON_WRITE_ESCAPE_UNICODE;
yyjson_write_err err;
yyjson_mut_write_file("/tmp/config.json", doc, flg, NULL, &err);
if (err.code) {
    printf("write error (%u): %s\n", err.code, err.msg);
}

// Free the doc
yyjson_doc_free(idoc);
yyjson_mut_doc_free(doc);
```
