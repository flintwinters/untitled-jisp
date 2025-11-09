#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdarg.h>
#include "yyjson.h"

/* Error handling context and helpers */
typedef struct jisp_ctx {
    const char *filename;
    const char *src;
    size_t src_len;
} jisp_ctx;

static jisp_ctx g_jisp_ctx = {0};

static void jisp_dump_state(yyjson_mut_doc *doc) {
    if (!doc) return;
    yyjson_write_err err;
    char *json_str = yyjson_mut_write_opts(doc, YYJSON_WRITE_PRETTY, NULL, NULL, &err);
    if (json_str) {
        fprintf(stderr, "\n---- JSON State Snapshot ----\n%s\n-----------------------------\n", json_str);
        free(json_str);
    }
}

static void jisp_report_pos(const char *source_name, const char *src, size_t len, size_t pos) {
    if (!src || len == 0) {
        fprintf(stderr, "%s: at byte %zu (source unknown)\n", source_name ? source_name : "source", pos);
        return;
    }
    size_t line = 0, col = 0, line_pos = 0;
    if (yyjson_locate_pos(src, len, pos, &line, &col, &line_pos)) {
        fprintf(stderr, "%s: at byte %zu (line %zu, col %zu)\n", source_name ? source_name : "source", pos, line, col);
    } else {
        fprintf(stderr, "%s: at byte %zu\n", source_name ? source_name : "source", pos);
    }
}

static void jisp_fatal(yyjson_mut_doc *doc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "JISP fatal error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    jisp_dump_state(doc);
    exit(1);
}

static void jisp_fatal_parse(yyjson_mut_doc *doc, const char *source_name, const char *src, size_t len, size_t pos, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "JISP parse error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    jisp_report_pos(source_name, src, len, pos);
    jisp_dump_state(doc);
    exit(1);
}

// Forward declarations
struct jisp_instruction_t;
void process_functions(yyjson_mut_doc *doc, struct jisp_instruction_t *instructions, size_t count);

// JISP op function signature
typedef void (*jisp_op)(yyjson_mut_doc *doc, yyjson_val *args);

// JISP instruction structure
typedef struct jisp_instruction_t {
    jisp_op op;
    char *args_json; // JSON string for args, to be parsed
} jisp_instruction;

/* JISP op forward declarations for registry */
void push_value(yyjson_mut_doc *doc, yyjson_val *args);
void pop_and_store(yyjson_mut_doc *doc, yyjson_val *args);
void duplicate_top(yyjson_mut_doc *doc, yyjson_val *args);
void add_two_top(yyjson_mut_doc *doc, yyjson_val *args);
void calculate_final_result(yyjson_mut_doc *doc, yyjson_val *args);
void print_json(yyjson_mut_doc *doc, yyjson_val *args);

/* Global JISP op registry (JSON document) */
typedef enum jisp_op_id {
    JISP_OP_PUSH_VALUE = 1,
    JISP_OP_POP_AND_STORE = 2,
    JISP_OP_DUPLICATE_TOP = 3,
    JISP_OP_ADD_TWO_TOP = 4,
    JISP_OP_CALCULATE_FINAL_RESULT = 5,
    JISP_OP_PRINT_JSON = 6
} jisp_op_id;

static yyjson_mut_doc *g_jisp_op_registry = NULL;

static jisp_op jisp_op_from_id(int id) {
    switch (id) {
        case JISP_OP_PUSH_VALUE: return push_value;
        case JISP_OP_POP_AND_STORE: return pop_and_store;
        case JISP_OP_DUPLICATE_TOP: return duplicate_top;
        case JISP_OP_ADD_TWO_TOP: return add_two_top;
        case JISP_OP_CALCULATE_FINAL_RESULT: return calculate_final_result;
        case JISP_OP_PRINT_JSON: return print_json;
        default: return NULL;
    }
}

static void jisp_op_registry_init(void) {
    if (g_jisp_op_registry) return;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_int(d, root, "push_value", JISP_OP_PUSH_VALUE);
    yyjson_mut_obj_add_int(d, root, "pop_and_store", JISP_OP_POP_AND_STORE);
    yyjson_mut_obj_add_int(d, root, "duplicate_top", JISP_OP_DUPLICATE_TOP);
    yyjson_mut_obj_add_int(d, root, "add_two_top", JISP_OP_ADD_TWO_TOP);
    yyjson_mut_obj_add_int(d, root, "calculate_final_result", JISP_OP_CALCULATE_FINAL_RESULT);
    yyjson_mut_obj_add_int(d, root, "print_json", JISP_OP_PRINT_JSON);
    g_jisp_op_registry = d;
}

static void jisp_op_registry_free(void) {
    if (g_jisp_op_registry) {
        yyjson_mut_doc_free(g_jisp_op_registry);
        g_jisp_op_registry = NULL;
    }
}

/* Optional helper: fetch op by name from registry */
static jisp_op jisp_op_registry_get(const char *name) {
    if (!g_jisp_op_registry || !name) return NULL;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(g_jisp_op_registry);
    yyjson_mut_val *idv = yyjson_mut_obj_get(root, name);
    if (!idv) return NULL;
    int id = yyjson_mut_get_int(idv);
    return jisp_op_from_id(id);
}

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
    if (!stack || !yyjson_mut_is_arr(stack)) {
        jisp_fatal(doc, "push_value: missing or non-array 'stack'");
    }
    if (!args || !yyjson_is_arr(args)) {
        jisp_fatal(doc, "push_value: args must be a JSON array");
    }
    yyjson_val *value_to_push = yyjson_arr_get_first(args);
    if (!value_to_push) {
        jisp_fatal(doc, "push_value: args array is empty");
    }
    yyjson_mut_arr_append(stack, yyjson_val_mut_copy(doc, value_to_push));
}

void pop_and_store(yyjson_mut_doc *doc, yyjson_val *args) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
    if (!stack || !yyjson_mut_is_arr(stack)) {
        jisp_fatal(doc, "pop_and_store: missing or non-array 'stack'");
    }
    if (!args || !yyjson_is_arr(args)) {
        jisp_fatal(doc, "pop_and_store: args must be a JSON array");
    }
    yyjson_val *key_val = yyjson_arr_get_first(args);
    if (!key_val || !yyjson_is_str(key_val)) {
        jisp_fatal(doc, "pop_and_store: first arg must be a string key");
    }
    if (yyjson_mut_arr_size(stack) == 0) {
        jisp_fatal(doc, "pop_and_store: stack is empty");
    }

    const char *key = yyjson_get_str(key_val);
    size_t key_len = strlen(key);
    char *key_in_doc = unsafe_yyjson_mut_strncpy(doc, key, key_len);
    yyjson_mut_val *value = yyjson_mut_arr_remove_last(stack);
    if (!value || !key_in_doc) {
        jisp_fatal(doc, "pop_and_store: failed to pop value or duplicate key");
    }
    yyjson_mut_obj_add_val(doc, root, key_in_doc, value);
}

void duplicate_top(yyjson_mut_doc *doc, yyjson_val *args) {
    (void)args;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
    if (!stack || !yyjson_mut_is_arr(stack)) {
        jisp_fatal(doc, "duplicate_top: missing or non-array 'stack'");
    }
    if (yyjson_mut_arr_size(stack) == 0) {
        jisp_fatal(doc, "duplicate_top: stack is empty");
    }
    yyjson_mut_val *last = yyjson_mut_arr_remove_last(stack);
    if (!last) {
        jisp_fatal(doc, "duplicate_top: failed to pop top of stack");
    }
    /* push original back */
    yyjson_mut_arr_append(stack, last);
    /* push a deep copy as duplicate */
    yyjson_mut_arr_append(stack, yyjson_val_mut_copy(doc, (yyjson_val *)last));
}

void add_two_top(yyjson_mut_doc *doc, yyjson_val *args) {
    (void)args;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
    if (!stack || !yyjson_mut_is_arr(stack)) {
        jisp_fatal(doc, "add_two_top: missing or non-array 'stack'");
    }
    if (yyjson_mut_arr_size(stack) < 2) {
        jisp_fatal(doc, "add_two_top: need at least two values on stack");
    }

    yyjson_mut_val *val1_mut = yyjson_mut_arr_remove_last(stack);
    yyjson_mut_val *val2_mut = yyjson_mut_arr_remove_last(stack);
    if (!val1_mut || !val2_mut) {
        jisp_fatal(doc, "add_two_top: failed to pop operands");
    }
    if (!yyjson_is_num((yyjson_val *)val1_mut) || !yyjson_is_num((yyjson_val *)val2_mut)) {
        jisp_fatal(doc, "add_two_top: operands must be numeric");
    }

    double val1 = (double)yyjson_mut_get_sint(val1_mut);
    double val2 = (double)yyjson_mut_get_sint(val2_mut);
    yyjson_mut_arr_add_real(doc, stack, val1 + val2);
}

// Custom function to replicate Python's `eval_code` for the example
void calculate_final_result(yyjson_mut_doc *doc, yyjson_val *args) {
    (void)args;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);

    yyjson_mut_val *temp_sum_val = yyjson_mut_obj_get(root, "temp_sum");
    yyjson_mut_val *temp_mult_val = yyjson_mut_obj_get(root, "temp_mult");

    double temp_sum = 0;
    double temp_mult = 0;

    if (temp_sum_val) {
        if (!yyjson_is_num((yyjson_val *)temp_sum_val)) {
            jisp_fatal(doc, "calculate_final_result: 'temp_sum' must be numeric");
        }
        temp_sum = (double)yyjson_mut_get_sint(temp_sum_val);
    }
    if (temp_mult_val) {
        if (!yyjson_is_num((yyjson_val *)temp_mult_val)) {
            jisp_fatal(doc, "calculate_final_result: 'temp_mult' must be numeric");
        }
        temp_mult = (double)yyjson_mut_get_sint(temp_mult_val);
    }

    yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
    double stack_val = 0;
    if (stack && yyjson_mut_arr_size(stack) > 0) {
        yyjson_mut_val *val_mut = yyjson_mut_arr_remove_last(stack);
        if (!val_mut) {
            jisp_fatal(doc, "calculate_final_result: failed to pop value from stack");
        }
        if (!yyjson_is_num((yyjson_val *)val_mut)) {
            jisp_fatal(doc, "calculate_final_result: top of stack must be numeric");
        }
        stack_val = (double)yyjson_mut_get_sint(val_mut);
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
void process_functions(yyjson_mut_doc *doc, jisp_instruction *instructions, size_t count) {
    for (size_t i = 0; i < count; i++) {
        yyjson_doc *args_doc = NULL;
        yyjson_val *args = NULL;
        if (instructions[i].args_json) {
            yyjson_read_err rerr;
            args_doc = yyjson_read_opts(instructions[i].args_json, strlen(instructions[i].args_json), 0, NULL, &rerr);
            if (!args_doc) {
                jisp_fatal_parse(doc, "instruction args", instructions[i].args_json, strlen(instructions[i].args_json), rerr.pos, "Failed to parse instruction args at index %zu: %s", i, rerr.msg ? rerr.msg : "unknown");
            }
            args = yyjson_doc_get_root(args_doc);
        }
        
        instructions[i].op(doc, args);
        
        if (args_doc) {
            yyjson_doc_free(args_doc);
        }
    }
}

/* Build and execute instructions from root["entrypoint"] string array */
static void process_entrypoint(yyjson_mut_doc *doc) {
    if (!doc) return;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!root) return;
    yyjson_mut_val *ep = yyjson_mut_obj_get(root, "entrypoint");
    if (!ep) return;
    if (!yyjson_mut_is_arr(ep)) {
        jisp_fatal(doc, "entrypoint must be an array of strings, numbers, or arrays");
    }

    size_t cap = yyjson_mut_arr_size(ep);
    if (cap == 0) return;

    jisp_instruction *ins = (jisp_instruction *)malloc(sizeof(jisp_instruction) * cap);
    if (!ins) jisp_fatal(doc, "Out of memory allocating instruction buffer for %zu items", cap);

    size_t cnt = 0;
    yyjson_mut_arr_iter it;
    yyjson_mut_val *elem;

    if (!yyjson_mut_arr_iter_init(ep, &it)) {
        free(ins);
        return;
    }
    while ((elem = yyjson_mut_arr_iter_next(&it))) {
        if (yyjson_mut_is_str(elem)) {
            const char *name = yyjson_get_str((yyjson_val *)elem);
            jisp_op op = jisp_op_registry_get(name);
            if (op) {
                ins[cnt].op = op;
                ins[cnt].args_json = NULL;
                cnt++;
            } else {
                jisp_fatal(doc, "Unknown entrypoint op: %s", name ? name : "(null)");
            }
        } else if (yyjson_is_num((yyjson_val *)elem)) {
            /* Numeric literal: translate to push_value with inline JSON args like "[50]" */
            double v = yyjson_get_int((yyjson_val *)elem);
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "[%.17g]", v);
            if (n > 0) {
                char *dup = (char *)malloc((size_t)n + 1);
                if (dup) {
                    memcpy(dup, buf, (size_t)n + 1);
                    ins[cnt].op = push_value;
                    ins[cnt].args_json = dup;
                    cnt++;
                }
            }
        } else if (yyjson_mut_is_arr(elem)) {
            /* Array form: ["op_name", ...args]
               - First element must be string op name
               - Remaining elements become the args JSON array */
            yyjson_mut_arr_iter it2;
            if (!yyjson_mut_arr_iter_init(elem, &it2)) {
                jisp_fatal(doc, "entrypoint array init failed at index %zu", cnt);
            }
            yyjson_mut_val *first = yyjson_mut_arr_iter_next(&it2);
            if (!first || !yyjson_mut_is_str(first)) {
                jisp_fatal(doc, "entrypoint array missing string op name at index %zu", cnt);
            }
            const char *op_name = yyjson_get_str((yyjson_val *)first);
            jisp_op op = jisp_op_registry_get(op_name);
            if (!op) {
                jisp_fatal(doc, "Unknown entrypoint op: %s", op_name ? op_name : "(null)");
            }

            /* Build args JSON by copying remaining elements into a temp doc array and writing it */
            yyjson_mut_doc *adoc = yyjson_mut_doc_new(NULL);
            if (!adoc) {
                jisp_fatal(doc, "allocation failed building args for %s", op_name ? op_name : "(null)");
            }
            yyjson_mut_val *arr = yyjson_mut_arr(adoc);
            yyjson_mut_doc_set_root(adoc, arr);

            yyjson_mut_val *sub;
            while ((sub = yyjson_mut_arr_iter_next(&it2))) {
                yyjson_mut_arr_append(arr, yyjson_val_mut_copy(adoc, (yyjson_val *)sub));
            }

            yyjson_write_err werr;
            char *args_str = yyjson_mut_write_opts(adoc, 0, NULL, NULL, &werr);
            yyjson_mut_doc_free(adoc);
            if (!args_str) {
                jisp_fatal(doc, "failed to serialize args for %s: %s", op_name ? op_name : "(null)", werr.msg ? werr.msg : "unknown");
            }

            ins[cnt].op = op;
            ins[cnt].args_json = args_str;
            cnt++;
        } else {
            jisp_fatal(doc, "entrypoint element at index %zu is not a string or number", cnt);
        }
    }

    if (cnt > 0) {
        process_functions(doc, ins, cnt);
    }

    for (size_t i = 0; i < cnt; i++) {
        if (ins[i].args_json) free((void *)ins[i].args_json);
    }
    free(ins);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s test.json\n", argv[0]);
        return 1;
    }
    /* Initialize global JISP op registry (JSON doc) */
    jisp_op_registry_init();
    // Load initial JSON from file provided as first command-line argument.
    const char *filename = argv[1];
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        jisp_fatal(NULL, "Failed to open file: %s", filename);
    }
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    if (fsz < 0) {
        fclose(fp);
        jisp_fatal(NULL, "Failed to stat file: %s", filename);
    }
    fseek(fp, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)fsz + 1);
    if (!buf) {
        fclose(fp);
        jisp_fatal(NULL, "Out of memory reading file: %s", filename);
    }
    size_t rd = fread(buf, 1, (size_t)fsz, fp);
    if (rd != (size_t)fsz) {
        fclose(fp);
        free(buf);
        jisp_fatal(NULL, "Short read from file: %s", filename);
    }
    buf[fsz] = '\0';
    fclose(fp);
    g_jisp_ctx.filename = filename;
    g_jisp_ctx.src = buf;
    g_jisp_ctx.src_len = (size_t)fsz;

    yyjson_read_err rerr;
    yyjson_doc *in = yyjson_read_opts(buf, (size_t)fsz, YYJSON_READ_ALLOW_COMMENTS, NULL, &rerr);
    if (!in) {
        jisp_fatal_parse(NULL, g_jisp_ctx.filename ? g_jisp_ctx.filename : "input", buf, (size_t)fsz, rerr.pos, "Failed to parse input JSON: %s", rerr.msg ? rerr.msg : "unknown");
    }
    yyjson_val *in_root = yyjson_doc_get_root(in);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_val_mut_copy(doc, in_root);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_doc_free(in);
    
    /* Initial state (stack, temp_sum, temp_mult, final_result, user/profile, nums)
       is provided by the input JSON file. */

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

        /* Nested structures (user/profile and nums) are provided by the input JSON file. */

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
    jisp_instruction add_block[] = {
        {push_value, "[10]"},
        {push_value, "[20]"},
        {add_two_top, NULL},
        {pop_and_store, "[\"temp_sum\"]"}
    };

    // duplicate_and_store_block
    jisp_instruction duplicate_and_store_block[] = {
        {push_value, "[50]"},
        {duplicate_top, NULL},
        {pop_and_store, "[\"temp_mult\"]"}
    };

    // main_function_list from Python script is executed here
    process_functions(doc, add_block, sizeof(add_block)/sizeof(jisp_instruction));
    process_functions(doc, duplicate_and_store_block, sizeof(duplicate_and_store_block)/sizeof(jisp_instruction));
    
    jisp_instruction main_part2[] = {
        {push_value, "[10]"},
        {calculate_final_result, NULL}
    };
    process_functions(doc, main_part2, sizeof(main_part2)/sizeof(jisp_instruction));

    process_entrypoint(doc);
    yyjson_mut_doc_free(doc);
    free(buf);
    jisp_op_registry_free();
    return 0;
}
