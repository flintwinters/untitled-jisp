#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
        yyjson_mut_val *value = yyjson_mut_arr_get_last(stack);
        if (value) {
            // yyjson_mut_obj_add_val moves the value from its old parent (stack)
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
        yyjson_mut_val *val1_mut = yyjson_mut_arr_get_last(stack);
        yyjson_mut_arr_remove_last(stack);
        yyjson_mut_val *val2_mut = yyjson_mut_arr_get_last(stack);
        yyjson_mut_arr_remove_last(stack);
        
        double val1 = yyjson_mut_get_real(val1_mut);
        double val2 = yyjson_mut_get_real(val2_mut);
        
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
        yyjson_mut_val *val_mut = yyjson_mut_arr_get_last(stack);
        stack_val = yyjson_mut_get_real(val_mut);
        yyjson_mut_arr_remove_last(stack);
    }
    
    double final_result = temp_sum + temp_mult + stack_val;
    yyjson_mut_obj_add_real(doc, root, "final_result", final_result);
}

void print_json(yyjson_mut_doc *doc, yyjson_val *args) {
    (void)args; // unused
    yyjson_write_err err;
    const char *json_str = yyjson_mut_write_opts(doc, YYJSON_WRITE_PRETTY, NULL, NULL, &err);
    if (json_str) {
        printf("%s\n", json_str);
        free((void*)json_str);
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
