#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
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
        int64_t cur = (int64_t)yyjson_mut_get_sint(ref);
        unsafe_yyjson_set_sint(ref, cur);
    }
    return ref;
}

/* Retain: increments root["ref"]; creates it if missing. */
static void jpm_doc_retain(yyjson_mut_doc *doc) {
    if (!doc) return;
    yyjson_mut_val *ref = ensure_ref_field(doc);
    if (!ref) return;
    int64_t cur = (int64_t)yyjson_mut_get_sint(ref);
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
    int64_t cur = (int64_t)yyjson_mut_get_sint(ref);
    if (cur > 0) cur--;
    unsafe_yyjson_set_sint(ref, cur);
    if (cur == 0) {
        yyjson_mut_doc_free(doc);
    }
}


/* 
   Simple JSON Pointer Monad (minimal step):
   - Adds jpm_status, jpm_ptr struct, and helper functions.
   - Implements a tiny jpm_return that only supports resolving the root path "/".
   - Provides jpm_ptr_release to balance successful jpm_return retains.
*/

typedef enum jpm_status {
    JPM_OK = 0,
    JPM_ERR_INVALID_ARG,
    JPM_ERR_NOT_FOUND,
    JPM_ERR_TYPE,
    JPM_ERR_RANGE,
    JPM_ERR_INTERNAL
} jpm_status;

typedef struct jpm_ptr {
    yyjson_mut_doc *doc;
    yyjson_mut_val *val;
    const char     *path; /* optional; not owned */
} jpm_ptr;

static bool jpm_is_valid(jpm_ptr p) {
    return p.doc != NULL && p.val != NULL;
}

static const char *jpm_path(jpm_ptr p) {
    return p.path;
}

static yyjson_mut_val *jpm_value(jpm_ptr p) {
    return p.val;
}

/* Resolver using yyjson's JSON Pointer utilities. Supports full RFC 6901. Retains the doc on success. */
static jpm_status jpm_return(yyjson_mut_doc *doc, const char *rfc6901_path, jpm_ptr *out) {
    if (!out) return JPM_ERR_INVALID_ARG;
    out->doc = NULL;
    out->val = NULL;
    out->path = NULL;

    if (!doc || !rfc6901_path) return JPM_ERR_INVALID_ARG;

    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!root) return JPM_ERR_NOT_FOUND;

    if (strcmp(rfc6901_path, "/") == 0) {
        jpm_doc_retain(doc);
        out->doc = doc;
        out->val = root;
        out->path = rfc6901_path;
        return JPM_OK;
    }

    /* Use yyjson's pointer resolver to avoid reimplementing RFC 6901. */
    yyjson_mut_val *val = yyjson_mut_ptr_get(root, rfc6901_path);
    if (!val) {
        return JPM_ERR_NOT_FOUND;
    }

    jpm_doc_retain(doc);
    out->doc = doc;
    out->val = val;
    out->path = rfc6901_path;
    return JPM_OK;
}

/* Release handle; decrements doc refcount and clears fields. */
static void jpm_ptr_release(jpm_ptr *p) {
    if (!p || !p->doc) return;
    jpm_doc_release(p->doc);
    p->doc = NULL;
    p->val = NULL;
    p->path = NULL;
}

/* Bind/Map API (minimal scaffolding) */
typedef jpm_status (*jpm_fn)(jpm_ptr in, jpm_ptr *out, void *ctx);
typedef jpm_status (*jpm_map_fn)(jpm_ptr in, void *ctx);

static jpm_status jpm_bind(jpm_ptr in, jpm_fn f, void *ctx, jpm_ptr *out) {
    if (!f || !out) return JPM_ERR_INVALID_ARG;
    if (!jpm_is_valid(in)) {
        out->doc = in.doc;
        out->val = NULL;
        out->path = NULL;
        return JPM_ERR_NOT_FOUND;
    }
    return f(in, out, ctx);
}

static jpm_status jpm_map(jpm_ptr in, jpm_map_fn f, void *ctx, jpm_ptr *out) {
    if (!f || !out) return JPM_ERR_INVALID_ARG;
    jpm_status st = f(in, ctx);
    if (st == JPM_OK) {
        *out = in;
    } else {
        out->doc = in.doc;
        out->val = NULL;
        out->path = in.path;
    }
    return st;
}

/* Simple helper used in tests: select a path relative to the same document. */
static jpm_status jpm_select_path(jpm_ptr in, jpm_ptr *out, void *ctx) {
    const char *path = (const char *)ctx;
    if (!path) return JPM_ERR_INVALID_ARG;
    return jpm_return(in.doc, path, out);
}

/* Simple mapper used in tests: check that the stored path equals "/". */
static jpm_status jpm_check_path_is_root(jpm_ptr in, void *ctx) {
    const char *pth = jpm_path(in);
    if (!pth || strcmp(pth, "/") != 0) return JPM_ERR_INVALID_ARG;
    return JPM_OK;
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
        size_t key_len = strlen(key);
        char *key_in_doc = unsafe_yyjson_mut_strncpy(doc, key, key_len);
        yyjson_mut_val *value = yyjson_mut_arr_remove_last(stack);
        if (value && key_in_doc) {
            yyjson_mut_obj_add_val(doc, root, key_in_doc, value);
        }
    }
}

void duplicate_top(yyjson_mut_doc *doc, yyjson_val *args) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
    if (stack && yyjson_mut_arr_size(stack) > 0) {
        yyjson_mut_val *last = yyjson_mut_arr_remove_last(stack);
        if (last) {
            /* push original back */
            yyjson_mut_arr_append(stack, last);
            /* push a deep copy as duplicate */
            yyjson_mut_arr_append(stack, yyjson_val_mut_copy(doc, (yyjson_val *)last));
        }
    }
}

void add_two_top(yyjson_mut_doc *doc, yyjson_val *args) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
    if (stack && yyjson_mut_arr_size(stack) >= 2) {
        yyjson_mut_val *val1_mut = yyjson_mut_arr_remove_last(stack);
        yyjson_mut_val *val2_mut = yyjson_mut_arr_remove_last(stack);
        
        double val1 = val1_mut ? yyjson_mut_get_sint(val1_mut) : 0;
        double val2 = val2_mut ? yyjson_mut_get_sint(val2_mut) : 0;
        
        yyjson_mut_arr_add_real(doc, stack, val1 + val2);
    }
}

// Custom function to replicate Python's `eval_code` for the example
void calculate_final_result(yyjson_mut_doc *doc, yyjson_val *args) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    
    yyjson_mut_val *temp_sum_val = yyjson_mut_obj_get(root, "temp_sum");
    yyjson_mut_val *temp_mult_val = yyjson_mut_obj_get(root, "temp_mult");
    
    double temp_sum = temp_sum_val ? yyjson_mut_get_sint(temp_sum_val) : 0;
    double temp_mult = temp_mult_val ? yyjson_mut_get_sint(temp_mult_val) : 0;
    
    yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
    double stack_val = 0;
    if (stack && yyjson_mut_arr_size(stack) > 0) {
        yyjson_mut_val *val_mut = yyjson_mut_arr_remove_last(stack);
        if (val_mut) {
            stack_val = yyjson_mut_get_sint(val_mut);
            // The memory for val_mut is managed by the doc's pool allocator.
        }
    }
    
    double final_result = temp_sum + temp_mult + stack_val;
    yyjson_mut_obj_add_real(doc, root, "final_result", final_result);
}

void print_json(yyjson_mut_doc *doc, yyjson_val *args) {
    yyjson_write_err err;
    char *json_str = yyjson_mut_write_opts(doc, YYJSON_WRITE_PRETTY, NULL, NULL, &err);
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

int main(int argc, char **argv) {
    (void)argc;
    // Load initial JSON from file provided as first command-line argument.
    const char *filename = argv[1];
    FILE *fp = fopen(filename, "rb");
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)fsz + 1);
    fread(buf, 1, (size_t)fsz, fp);
    buf[fsz] = '\0';
    fclose(fp);

    yyjson_doc *in = yyjson_read(buf, (size_t)fsz, 0);
    yyjson_val *in_root = yyjson_doc_get_root(in);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_val_mut_copy(doc, in_root);
    yyjson_mut_doc_set_root(doc, root);

    free(buf);
    yyjson_doc_free(in);
    
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
        assert((int64_t)yyjson_mut_get_int(ref_val) == 2);

        // Release once should decrement "ref" to 1 (document must not be freed)
        jpm_doc_release(doc);
        ref_val = yyjson_mut_obj_get(root_local, "ref");
        assert((int64_t)yyjson_mut_get_int(ref_val) == 1);
    }

    // Additional simple refcount test: retain then release back to previous value
    {
        yyjson_mut_val *root_local = yyjson_mut_doc_get_root(doc);
        yyjson_mut_val *ref_val = yyjson_mut_obj_get(root_local, "ref");
        assert((int64_t)yyjson_mut_get_int(ref_val) == 1);

        // Retain to bump ref to 2
        jpm_doc_retain(doc);
        ref_val = yyjson_mut_obj_get(root_local, "ref");
        assert((int64_t)yyjson_mut_get_int(ref_val) == 2);

        // Release to bring it back to 1
        jpm_doc_release(doc);
        ref_val = yyjson_mut_obj_get(root_local, "ref");
        assert((int64_t)yyjson_mut_get_int(ref_val) == 1);
    }

    // Minimal JPM tests
    {
        yyjson_mut_val *root_local = yyjson_mut_doc_get_root(doc);
        yyjson_mut_val *ref_val = yyjson_mut_obj_get(root_local, "ref");
        int64_t ref_before = ref_val ? yyjson_mut_get_int(ref_val) : 0;

        // Not found path should not change refcount
        jpm_ptr p_bad;
        jpm_status st_bad = jpm_return(doc, "/nope", &p_bad);
        assert(st_bad == JPM_ERR_NOT_FOUND);
        ref_val = yyjson_mut_obj_get(root_local, "ref");
        assert((int64_t)yyjson_mut_get_int(ref_val) == ref_before);

        // Root path "/" should work and retain
        jpm_ptr p_root;
        jpm_status st = jpm_return(doc, "/", &p_root);
        assert(st == JPM_OK);
        assert(jpm_is_valid(p_root));
        const char *pth = jpm_path(p_root);
        assert(pth && strcmp(pth, "/") == 0);
        assert(jpm_value(p_root) == root_local);

        ref_val = yyjson_mut_obj_get(root_local, "ref");
        assert((int64_t)yyjson_mut_get_int(ref_val) == ref_before + 1);

        // Release should decrement back
        jpm_ptr_release(&p_root);
        ref_val = yyjson_mut_obj_get(root_local, "ref");
        assert((int64_t)yyjson_mut_get_int(ref_val) == ref_before);
    }

    // More JPM tests: support one-segment paths and bind/map
    {
        yyjson_mut_val *root_local = yyjson_mut_doc_get_root(doc);
        yyjson_mut_val *stack_local = yyjson_mut_obj_get(root_local, "stack");
        yyjson_mut_val *ref_val = yyjson_mut_obj_get(root_local, "ref");
        int64_t ref_before = ref_val ? yyjson_mut_get_int(ref_val) : 0;

        jpm_ptr p_root;
        jpm_status st0 = jpm_return(doc, "/", &p_root);
        assert(st0 == JPM_OK && jpm_is_valid(p_root));

        jpm_ptr p_stack_via_return;
        jpm_status st1 = jpm_return(doc, "/stack", &p_stack_via_return);
        assert(st1 == JPM_OK && jpm_is_valid(p_stack_via_return));
        assert(jpm_value(p_stack_via_return) == stack_local);

        jpm_ptr p_stack_via_bind;
        jpm_status st2 = jpm_bind(p_root, jpm_select_path, (void*)"/stack", &p_stack_via_bind);
        assert(st2 == JPM_OK && jpm_is_valid(p_stack_via_bind));
        assert(jpm_value(p_stack_via_bind) == stack_local);

        jpm_ptr p_after_map;
        jpm_status st3 = jpm_map(p_root, jpm_check_path_is_root, NULL, &p_after_map);
        assert(st3 == JPM_OK && jpm_is_valid(p_after_map));
        assert(jpm_value(p_after_map) == jpm_value(p_root));

        jpm_ptr_release(&p_stack_via_bind);
        jpm_ptr_release(&p_stack_via_return);
        jpm_ptr_release(&p_root);

        ref_val = yyjson_mut_obj_get(root_local, "ref");
        assert((int64_t)yyjson_mut_get_int(ref_val) == ref_before);
    }

    {
        yyjson_mut_val *root_local = yyjson_mut_doc_get_root(doc);

        /* Prepare nested structures for multi-segment and RFC 6901 decoding tests */
        yyjson_mut_val *user = yyjson_mut_obj(doc);
        yyjson_mut_val *profile = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, user, "profile", profile);
        yyjson_mut_obj_add_val(doc, root_local, "user", user);
        yyjson_mut_obj_add_int(doc, profile, "age", 42);
        yyjson_mut_obj_add_int(doc, profile, "x/y", 1);  /* requires ~1 decoding */
        yyjson_mut_obj_add_int(doc, profile, "x~y", 2);  /* requires ~0 decoding */

        yyjson_mut_val *nums = yyjson_mut_arr(doc);
        yyjson_mut_arr_add_int(doc, nums, 7);
        yyjson_mut_arr_add_int(doc, nums, 8);
        yyjson_mut_arr_add_int(doc, nums, 9);
        yyjson_mut_obj_add_val(doc, root_local, "nums", nums);

        yyjson_mut_val *ref_val = yyjson_mut_obj_get(root_local, "ref");
        int64_t ref_before = ref_val ? yyjson_mut_get_int(ref_val) : 0;

        jpm_ptr p_age;
        assert(jpm_return(doc, "/user/profile/age", &p_age) == JPM_OK && jpm_is_valid(p_age));
        assert((int64_t)yyjson_mut_get_int(jpm_value(p_age)) == 42);

        jpm_ptr p_slash;
        assert(jpm_return(doc, "/user/profile/x~1y", &p_slash) == JPM_OK && jpm_is_valid(p_slash));
        assert((int64_t)yyjson_mut_get_int(jpm_value(p_slash)) == 1);

        jpm_ptr p_tilde;
        assert(jpm_return(doc, "/user/profile/x~0y", &p_tilde) == JPM_OK && jpm_is_valid(p_tilde));
        assert((int64_t)yyjson_mut_get_int(jpm_value(p_tilde)) == 2);

        jpm_ptr p_idx;
        assert(jpm_return(doc, "/nums/2", &p_idx) == JPM_OK && jpm_is_valid(p_idx));
        assert((int64_t)yyjson_mut_get_int(jpm_value(p_idx)) == 9);

        /* Out-of-range and invalid paths should be treated as not found with yyjson_mut_ptr_get */
        jpm_ptr p_oob;
        assert(jpm_return(doc, "/nums/99", &p_oob) == JPM_ERR_NOT_FOUND);

        jpm_ptr p_bad_type;
        assert(jpm_return(doc, "/temp_sum/0", &p_bad_type) == JPM_ERR_NOT_FOUND);

        jpm_ptr p_bad_escape;
        assert(jpm_return(doc, "/user/profile/x~2y", &p_bad_escape) == JPM_ERR_NOT_FOUND);

        jpm_ptr_release(&p_idx);
        jpm_ptr_release(&p_tilde);
        jpm_ptr_release(&p_slash);
        jpm_ptr_release(&p_age);

        ref_val = yyjson_mut_obj_get(root_local, "ref");
        assert((int64_t)yyjson_mut_get_int(ref_val) == ref_before);
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
        {calculate_final_result, NULL}
    };
    process_functions(doc, main_part2, sizeof(main_part2)/sizeof(jisp_instruction));

    print_json(doc, NULL);
    yyjson_mut_doc_free(doc);
    return 0;
}
