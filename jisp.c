#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include "yyjson.h"

// Forward declarations
struct jisp_instruction_t;
void process_functions(yyjson_mut_doc *doc, const struct jisp_instruction_t *instructions, size_t count);

// JISP op function signature
typedef void (*jisp_op)(yyjson_mut_doc *doc, yyjson_val *args);

// JISP instruction structure
typedef struct jisp_instruction_t {
    jisp_op op;
    const char *args_json; // JSON string for args, to be parsed
} jisp_instruction;

/*
    Incremental change step 1:
    - Add internal helpers to manage a root-level "ref" field on yyjson_mut_doc.
    - These helpers are not yet wired into main or opcodes; behavior remains unchanged.
    - Plan: In a subsequent step, replace direct yyjson_mut_doc_free(...) calls with jpm_doc_release(...)
      and introduce retains at creation sites or pointer factories.
*/

/* Ensure document has an object root; create one if missing or not an object. */
static yyjson_mut_val *ensure_root_object(yyjson_mut_doc *doc) {
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!root || !yyjson_mut_is_obj(root)) {
        root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
    }
    return root;
}

/* Ensure root["ref"] exists and is a numeric field; returns the value node. */
static yyjson_mut_val *ensure_ref_field(yyjson_mut_doc *doc) {
    yyjson_mut_val *root = ensure_root_object(doc);
    if (!root) return NULL;

    yyjson_mut_val *ref = yyjson_mut_obj_get(root, "ref");
    if (!ref) {
        /* Initialize "ref" to 0 if absent. */
        yyjson_mut_obj_add_int(doc, root, "ref", 0);
        ref = yyjson_mut_obj_get(root, "ref");
    } else {
        /* Coerce to integer numeric type if it exists (value preserved if numeric, becomes 0 if non-numeric). */
        int64_t cur = (int64_t)yyjson_mut_get_real(ref);
        unsafe_yyjson_set_sint(ref, cur);
    }
    return ref;
}

/* Retain: increments root["ref"]; creates it if missing. */
static void jpm_doc_retain(yyjson_mut_doc *doc) {
    if (!doc) return;
    yyjson_mut_val *ref = ensure_ref_field(doc);
    if (!ref) return;
    int64_t cur = (int64_t)yyjson_mut_get_real(ref);
    if (cur < 0) cur = 0;
    unsafe_yyjson_set_sint(ref, cur + 1);
}

/* Release: decrements root["ref"]; frees doc when it reaches zero. */
static void jpm_doc_release(yyjson_mut_doc *doc) {
    if (!doc) return;
    yyjson_mut_val *ref = ensure_ref_field(doc);
    if (!ref) {
        /* Fallback: no ref field; free immediately to avoid leak. */
        yyjson_mut_doc_free(doc);
        return;
    }
    int64_t cur = (int64_t)yyjson_mut_get_real(ref);
    if (cur > 0) cur--;
    unsafe_yyjson_set_sint(ref, cur);
    if (cur == 0) {
        yyjson_mut_doc_free(doc);
    }
}


// Core "Opcodes"

void push_value(yyjson_mut_doc *doc, yyjson_val *args) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
    yyjson_val *value_to_push = yyjson_arr_get_first(args);
    if (stack && value_to_push) {
        yyjson_mut_arr_append(stack, yyjson_val_mut_copy(doc, value_to_push));
    }
}

void pop_and_store(yyjson_mut_doc *doc, yyjson_val *args) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
    yyjson_val *key_val = yyjson_arr_get_first(args);
    if (stack && yyjson_mut_arr_size(stack) > 0 && key_val && yyjson_is_str(key_val)) {
        const char *key = yyjson_get_str(key_val);
        yyjson_mut_val *value = yyjson_mut_arr_remove_last(stack);
        if (value) {
            yyjson_mut_obj_add_val(doc, root, key, value);
        }
    }
}

void duplicate_top(yyjson_mut_doc *doc, yyjson_val *args) {
    (void)args; // unused
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
    if (stack) {
        yyjson_mut_val *top_value = yyjson_mut_arr_get_last(stack);
        if (top_value) {
            yyjson_mut_arr_append(stack, yyjson_val_mut_copy(doc, (yyjson_val *)top_value));
        }
    }
}

void add_two_top(yyjson_mut_doc *doc, yyjson_val *args) {
    (void)args; // unused
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
    if (stack && yyjson_mut_arr_size(stack) >= 2) {
        yyjson_mut_val *val1_mut = yyjson_mut_arr_remove_last(stack);
        yyjson_mut_val *val2_mut = yyjson_mut_arr_remove_last(stack);
        
        double val1 = val1_mut ? yyjson_mut_get_real(val1_mut) : 0;
        double val2 = val2_mut ? yyjson_mut_get_real(val2_mut) : 0;
        
        yyjson_mut_arr_add_real(doc, stack, val1 + val2);
    }
}

// Custom function to replicate Python's `eval_code` for the example
void calculate_final_result(yyjson_mut_doc *doc, yyjson_val *args) {
    (void)args; // unused
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    
    yyjson_mut_val *temp_sum_val = yyjson_mut_obj_get(root, "temp_sum");
    yyjson_mut_val *temp_mult_val = yyjson_mut_obj_get(root, "temp_mult");
    
    double temp_sum = temp_sum_val ? yyjson_mut_get_real(temp_sum_val) : 0;
    double temp_mult = temp_mult_val ? yyjson_mut_get_real(temp_mult_val) : 0;
    
    yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
    double stack_val = 0;
    if (stack && yyjson_mut_arr_size(stack) > 0) {
        yyjson_mut_val *val_mut = yyjson_mut_arr_remove_last(stack);
        if (val_mut) {
            stack_val = yyjson_mut_get_real(val_mut);
            // The memory for val_mut is managed by the doc's pool allocator.
        }
    }
    
    double final_result = temp_sum + temp_mult + stack_val;
    yyjson_mut_obj_add_real(doc, root, "final_result", final_result);
}

void print_json(yyjson_mut_doc *doc, yyjson_val *args) {
    (void)args; // unused
    yyjson_write_err err;
    char *json_str = yyjson_mut_write_opts(doc, YYJSON_WRITE_PRETTY, NULL, NULL, NULL, &err);
    if (json_str) {
        printf("%s\n", json_str);
        free(json_str);
    }
}


// The processor
void process_functions(yyjson_mut_doc *doc, const jisp_instruction *instructions, size_t count) {
    for (size_t i = 0; i < count; i++) {
        yyjson_doc *args_doc = NULL;
        yyjson_val *args = NULL;
        if (instructions[i].args_json) {
            args_doc = yyjson_read(instructions[i].args_json, strlen(instructions[i].args_json), 0);
            if(args_doc) {
                args = yyjson_doc_get_root(args_doc);
            }
        }
        
        instructions[i].op(doc, args);
        
        if (args_doc) {
            yyjson_doc_free(args_doc);
        }
    }
}

int main(void) {
    // Initial JSON
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    
    yyjson_mut_val *stack = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "stack", stack);
    yyjson_mut_obj_add_int(doc, root, "temp_sum", 0);
    yyjson_mut_obj_add_int(doc, root, "temp_mult", 0);
    yyjson_mut_obj_add_int(doc, root, "final_result", 0);

    // Tests for jpm_doc_retain and jpm_doc_release
    {
        yyjson_mut_val *root_local = yyjson_mut_doc_get_root(doc);
        yyjson_mut_val *ref_val = yyjson_mut_obj_get(root_local, "ref");
        // Initially, no "ref" field should be present
        assert(ref_val == NULL);

        // Retain twice should create and increment "ref" to 2
        jpm_doc_retain(doc);
        jpm_doc_retain(doc);
        ref_val = yyjson_mut_obj_get(root_local, "ref");
        assert(ref_val != NULL);
        assert((int64_t)yyjson_mut_get_real(ref_val) == 2);

        // Release once should decrement "ref" to 1 (document must not be freed)
        jpm_doc_release(doc);
        ref_val = yyjson_mut_obj_get(root_local, "ref");
        assert((int64_t)yyjson_mut_get_real(ref_val) == 1);
    }

    // add_block
    const jisp_instruction add_block[] = {
        {push_value, "[10]"},
        {push_value, "[20]"},
        {add_two_top, NULL},
        {pop_and_store, "[\"temp_sum\"]"}
    };

    // duplicate_and_store_block
    const jisp_instruction duplicate_and_store_block[] = {
        {push_value, "[50]"},
        {duplicate_top, NULL},
        {pop_and_store, "[\"temp_mult\"]"}
    };

    // main_function_list from Python script is executed here
    process_functions(doc, add_block, sizeof(add_block)/sizeof(jisp_instruction));
    process_functions(doc, duplicate_and_store_block, sizeof(duplicate_and_store_block)/sizeof(jisp_instruction));
    
    const jisp_instruction main_part2[] = {
        {push_value, "[10]"},
        {calculate_final_result, NULL},
        {print_json, NULL}
    };
    process_functions(doc, main_part2, sizeof(main_part2)/sizeof(jisp_instruction));

    yyjson_mut_doc_free(doc);
    return 0;
}
