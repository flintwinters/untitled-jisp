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

/* jisp_json_to_pretty_string: Produces a pretty-printed JSON string for diagnostics or user output; use wherever the doc needs to be serialized for display. */
static char *jisp_json_to_pretty_string(yyjson_mut_doc *doc) {
    if (!doc) return NULL;
    yyjson_write_err err;
    return yyjson_mut_write_opts(doc, YYJSON_WRITE_PRETTY, NULL, NULL, &err);
}

/* jisp_dump_state: Emits a labeled snapshot of the current JSON state for debugging; call from error paths to aid investigation. */
static void jisp_dump_state(yyjson_mut_doc *doc) {
    if (!doc) return;
    char *json_str = jisp_json_to_pretty_string(doc);
    if (json_str) {
        fprintf(stderr, "\n---- JSON State Snapshot ----\n%s\n-----------------------------\n", json_str);
        free(json_str);
    }
}

/* jisp_report_pos: Reports a human-friendly location for parse errors; use when surfacing input JSON diagnostics. */
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

/* jisp_fatal: Centralized fatal error handler that also snapshots state; use to abort on unrecoverable conditions. */
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

/* jisp_fatal_parse: Reports a parse-time fatal error with location and state; use for input JSON parsing failures. */
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

/* get_root_fallible: Ensures the document has a root value; use in ops and interpreters that require a valid root object. */
static yyjson_mut_val *get_root_fallible(yyjson_mut_doc *doc, const char *ctx) {
    if (!doc) jisp_fatal(NULL, "%s: null document", ctx ? ctx : "operation");
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!root) jisp_fatal(doc, "%s: missing root", ctx ? ctx : "operation");
    return root;
}

/* get_stack_fallible: Fetches root["stack"] as an array; use when an operation requires a well-formed stack. */
static yyjson_mut_val *get_stack_fallible(yyjson_mut_doc *doc, const char *ctx) {
    yyjson_mut_val *root = get_root_fallible(doc, ctx);
    yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
    if (!stack || !yyjson_mut_is_arr(stack)) {
        jisp_fatal(doc, "%s: missing or non-array 'stack'", ctx ? ctx : "operation");
    }
    return stack;
}


/* JISP op function signature (no explicit args) */
typedef void (*jisp_op)(yyjson_mut_doc *doc);

/* Forward declare jpm_ptr for token union pointer kind. */
typedef struct jpm_ptr jpm_ptr;

/* Token stream types and interpreter */
typedef enum jisp_tok_kind {
    JISP_TOK_OP,
    JISP_TOK_INT,
    JISP_TOK_REAL,
    JISP_TOK_STR,
    JISP_TOK_JPM
} jisp_tok_kind;

typedef struct jisp_tok {
    jisp_tok_kind kind;
    union {
        jisp_op op;
        int64_t i;
        double d;
        const char *s;
        const jpm_ptr *ptr;
    } as;
} jisp_tok;

static void run_tokens(yyjson_mut_doc *doc, const jisp_tok *toks, size_t count);
static void process_ep_array(yyjson_mut_doc *doc, yyjson_mut_val *ep, const char *path_prefix);

/* JISP op forward declarations for registry */
void pop_and_store(yyjson_mut_doc *doc);
void duplicate_top(yyjson_mut_doc *doc);
void add_two_top(yyjson_mut_doc *doc);
void map_over(yyjson_mut_doc *doc);
void calculate_final_result(yyjson_mut_doc *doc);
void json_get(yyjson_mut_doc *doc);
void json_set(yyjson_mut_doc *doc);
void json_append(yyjson_mut_doc *doc);
void ptr_new(yyjson_mut_doc *doc);
void ptr_release_op(yyjson_mut_doc *doc);
void ptr_get(yyjson_mut_doc *doc);
void ptr_set(yyjson_mut_doc *doc);
void print_json(yyjson_mut_doc *doc);
void undo_last_residual(yyjson_mut_doc *doc);

/* Global JISP op registry (JSON document) */
typedef enum jisp_op_id {
    JISP_OP_POP_AND_STORE = 1,
    JISP_OP_DUPLICATE_TOP = 2,
    JISP_OP_ADD_TWO_TOP = 3,
    JISP_OP_CALCULATE_FINAL_RESULT = 4,
    JISP_OP_PRINT_JSON = 5,
    JISP_OP_UNDO_LAST_RESIDUAL = 6,
    JISP_OP_MAP_OVER = 7,
    JISP_OP_GET = 8,
    JISP_OP_SET = 9,
    JISP_OP_APPEND = 10,
    JISP_OP_PTR_NEW = 11,
    JISP_OP_PTR_RELEASE = 12,
    JISP_OP_PTR_GET = 13,
    JISP_OP_PTR_SET = 14
} jisp_op_id;

static yyjson_mut_doc *g_jisp_op_registry = NULL;

static jisp_op jisp_op_from_id(int id) {
    switch (id) {
        case JISP_OP_POP_AND_STORE: return pop_and_store;
        case JISP_OP_DUPLICATE_TOP: return duplicate_top;
        case JISP_OP_ADD_TWO_TOP: return add_two_top;
        case JISP_OP_CALCULATE_FINAL_RESULT: return calculate_final_result;
        case JISP_OP_PRINT_JSON: return print_json;
        case JISP_OP_UNDO_LAST_RESIDUAL: return undo_last_residual;
        case JISP_OP_MAP_OVER: return map_over;
        case JISP_OP_GET: return json_get;
        case JISP_OP_SET: return json_set;
        case JISP_OP_APPEND: return json_append;
        case JISP_OP_PTR_NEW: return ptr_new;
        case JISP_OP_PTR_RELEASE: return ptr_release_op;
        case JISP_OP_PTR_GET: return ptr_get;
        case JISP_OP_PTR_SET: return ptr_set;
        default: return NULL;
    }
}

static void jisp_op_registry_init(void) {
    if (g_jisp_op_registry) return;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_obj_add_int(d, root, "pop_and_store", JISP_OP_POP_AND_STORE);
    yyjson_mut_obj_add_int(d, root, "duplicate_top", JISP_OP_DUPLICATE_TOP);
    yyjson_mut_obj_add_int(d, root, "add_two_top", JISP_OP_ADD_TWO_TOP);
    yyjson_mut_obj_add_int(d, root, "calculate_final_result", JISP_OP_CALCULATE_FINAL_RESULT);
    yyjson_mut_obj_add_int(d, root, "print_json", JISP_OP_PRINT_JSON);
    yyjson_mut_obj_add_int(d, root, "undo_last_residual", JISP_OP_UNDO_LAST_RESIDUAL);
    yyjson_mut_obj_add_int(d, root, "map_over", JISP_OP_MAP_OVER);
    yyjson_mut_obj_add_int(d, root, "get", JISP_OP_GET);
    yyjson_mut_obj_add_int(d, root, "set", JISP_OP_SET);
    yyjson_mut_obj_add_int(d, root, "append", JISP_OP_APPEND);
    yyjson_mut_obj_add_int(d, root, "ptr_new", JISP_OP_PTR_NEW);
    yyjson_mut_obj_add_int(d, root, "ptr_release", JISP_OP_PTR_RELEASE);
    yyjson_mut_obj_add_int(d, root, "ptr_get", JISP_OP_PTR_GET);
    yyjson_mut_obj_add_int(d, root, "ptr_set", JISP_OP_PTR_SET);
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
   JSON Pointer Handle (non-monadic):
   - Provides jpm_ptr {doc, val, path} to track a raw yyjson pointer with optional RFC 6901 path metadata.
   - jpm_return resolves a path and returns a handle to the existing value without copying; the doc may be retained.
   - jpm_ptr_release decrements the doc refcount; no combinators (bind/map) are provided.
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

/* Global C-side Pointer Stack */
#define MAX_PTR_STACK 64
static jpm_ptr g_ptr_stack[MAX_PTR_STACK];
static size_t g_ptr_sp = 0;

static void ptr_stack_push(jpm_ptr p) {
    if (g_ptr_sp >= MAX_PTR_STACK) {
        jisp_fatal(NULL, "Pointer stack overflow (max %d)", MAX_PTR_STACK);
    }
    g_ptr_stack[g_ptr_sp++] = p;
}

static jpm_ptr ptr_stack_pop(void) {
    if (g_ptr_sp == 0) {
        jisp_fatal(NULL, "Pointer stack underflow");
    }
    return g_ptr_stack[--g_ptr_sp];
}

static jpm_ptr ptr_stack_peek(void) {
    if (g_ptr_sp == 0) {
        jisp_fatal(NULL, "Pointer stack underflow (peek)");
    }
    return g_ptr_stack[g_ptr_sp - 1];
}

static void ptr_stack_free_all(void) {
    while (g_ptr_sp > 0) {
        jpm_ptr p = g_ptr_stack[--g_ptr_sp];
        jpm_ptr_release(&p);
    }
}

/* No monadic combinators: pointers are raw handles with optional path metadata.
   Resolve explicitly with jpm_return(...) when needed. */

/* Deep copy a yyjson_mut_val within the same document. */
static yyjson_mut_val *jisp_mut_deep_copy(yyjson_mut_doc *doc, yyjson_mut_val *val) {
    if (!doc || !val) return NULL;

    yyjson_type t = unsafe_yyjson_get_type(val);

    switch (t) {
        case YYJSON_TYPE_NULL: {
            yyjson_mut_val *out = unsafe_yyjson_mut_val(doc, 1);
            if (!out) return NULL;
            unsafe_yyjson_set_tag(out, YYJSON_TYPE_NULL, YYJSON_SUBTYPE_NONE, 0);
            return out;
        }
        case YYJSON_TYPE_BOOL: {
            bool b = yyjson_get_bool((yyjson_val *)val);
            yyjson_mut_val *out = unsafe_yyjson_mut_val(doc, 1);
            if (!out) return NULL;
            unsafe_yyjson_set_bool(out, b);
            return out;
        }
        case YYJSON_TYPE_NUM: {
            int64_t n = (int64_t)yyjson_mut_get_sint(val);
            yyjson_mut_val *out = unsafe_yyjson_mut_val(doc, 1);
            if (!out) return NULL;
            unsafe_yyjson_set_sint(out, n);
            return out;
        }
        case YYJSON_TYPE_STR: {
            const char *s = unsafe_yyjson_get_str(val);
            size_t len = unsafe_yyjson_get_len(val);
            char *copy = unsafe_yyjson_mut_strncpy(doc, s ? s : "", s ? len : 0);
            yyjson_mut_val *out = unsafe_yyjson_mut_val(doc, 1);
            if (!out) return NULL;
            unsafe_yyjson_set_strn(out, copy, s ? len : 0);
            return out;
        }
        case YYJSON_TYPE_ARR: {
            yyjson_mut_val *out = yyjson_mut_arr(doc);
            if (!out) return NULL;
            yyjson_mut_arr_iter it;
            yyjson_mut_val *e;
            if (yyjson_mut_arr_iter_init(val, &it)) {
                while ((e = yyjson_mut_arr_iter_next(&it))) {
                    yyjson_mut_val *c = jisp_mut_deep_copy(doc, e);
                    if (!c) return NULL;
                    yyjson_mut_arr_append(out, c);
                }
            }
            return out;
        }
        case YYJSON_TYPE_OBJ: {
            yyjson_mut_val *out = yyjson_mut_obj(doc);
            if (!out) return NULL;
            yyjson_mut_obj_iter it;
            yyjson_mut_val *k;
            if (yyjson_mut_obj_iter_init(val, &it)) {
                while ((k = yyjson_mut_obj_iter_next(&it))) {
                    yyjson_mut_val *v = yyjson_mut_obj_iter_get_val(k);
                    const char *ks = unsafe_yyjson_get_str(k);
                    size_t klen = unsafe_yyjson_get_len(k);
                    char *kcopy = unsafe_yyjson_mut_strncpy(doc, ks ? ks : "", ks ? klen : 0);
                    yyjson_mut_val *vcopy = jisp_mut_deep_copy(doc, v);
                    if (!vcopy) return NULL;
                    yyjson_mut_obj_add_val(doc, out, kcopy, vcopy);
                }
            }
            return out;
        }
        default: {
            yyjson_mut_val *out = unsafe_yyjson_mut_val(doc, 1);
            if (!out) return NULL;
            unsafe_yyjson_set_tag(out, YYJSON_TYPE_NULL, YYJSON_SUBTYPE_NONE, 0);
            return out;
        }
    }
}

/* Residual logging helpers (JSON Patch RFC 6902 minimal recording) */

static bool is_reversible_enabled(yyjson_mut_doc *doc) {
    if (!doc) return false;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!root) return false;
    yyjson_mut_val *flag = yyjson_mut_obj_get(root, "is_reversible");
    if (!flag) return false;
    return yyjson_get_bool((yyjson_val *)flag);
}

/* Ensure root["residual"] exists and is an array; create if missing. */
static yyjson_mut_val *ensure_residual_array(yyjson_mut_doc *doc) {
    yyjson_mut_val *root = ensure_root_object(doc);
    if (!root) return NULL;
    yyjson_mut_val *res = yyjson_mut_obj_get(root, "residual");
    if (res) {
        if (yyjson_mut_is_arr(res)) return res;
        /* If present but not an array, do not mutate user data; skip logging. */
        return NULL;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    if (!arr) return NULL;
    yyjson_mut_obj_add_val(doc, root, "residual", arr);
    return arr;
}

/* Build a temporary RFC 6901 path for a single key: returns malloc'd string "/<encoded>" (caller frees). */
static char *jisp_build_path_for_key_temp(const char *key) {
    if (!key) return NULL;
    size_t in_len = strlen(key);
    size_t extra = 0;
    for (size_t i = 0; i < in_len; i++) {
        if (key[i] == '~' || key[i] == '/') extra++;
    }
    size_t out_len = 1 + in_len + extra; /* leading '/' + encoded */
    char *buf = (char *)malloc(out_len + 1);
    if (!buf) return NULL;
    char *p = buf;
    *p++ = '/';
    for (size_t i = 0; i < in_len; i++) {
        char c = key[i];
        if (c == '~') {
            *p++ = '~'; *p++ = '0';
        } else if (c == '/') {
            *p++ = '~'; *p++ = '1';
        } else {
            *p++ = c;
        }
    }
    *p = '\0';
    return buf;
}

static void record_patch_with_val(yyjson_mut_doc *doc, const char *op, const char *path, yyjson_mut_val *val) {
    if (!doc || !op || !path) return;
    if (!is_reversible_enabled(doc)) return;
    yyjson_mut_val *res = ensure_residual_array(doc);
    if (!res) return;

    yyjson_mut_val *patch = yyjson_mut_obj(doc);
    if (!patch) return;
    yyjson_mut_obj_add_strcpy(doc, patch, "op", op);
    yyjson_mut_obj_add_strcpy(doc, patch, "path", path);
    if (val) {
        yyjson_mut_val *vcopy = jisp_mut_deep_copy(doc, val);
        if (vcopy) {
            yyjson_mut_obj_add_val(doc, patch, "value", vcopy);
        }
    }
    yyjson_mut_arr_append(res, patch);
}

static void record_patch_with_real(yyjson_mut_doc *doc, const char *op, const char *path, double num) {
    if (!doc || !op || !path) return;
    if (!is_reversible_enabled(doc)) return;
    yyjson_mut_val *res = ensure_residual_array(doc);
    if (!res) return;

    yyjson_mut_val *patch = yyjson_mut_obj(doc);
    if (!patch) return;
    yyjson_mut_obj_add_strcpy(doc, patch, "op", op);
    yyjson_mut_obj_add_strcpy(doc, patch, "path", path);
    yyjson_mut_obj_add_real(doc, patch, "value", num);
    yyjson_mut_arr_append(res, patch);
}

static void record_patch_add_val(yyjson_mut_doc *doc, const char *path, yyjson_mut_val *val) {
    record_patch_with_val(doc, "add", path, val);
}

static void record_patch_replace_val(yyjson_mut_doc *doc, const char *path, yyjson_mut_val *val) {
    record_patch_with_val(doc, "replace", path, val);
}

static void record_patch_add_real(yyjson_mut_doc *doc, const char *path, double num) {
    record_patch_with_real(doc, "add", path, num);
}

static void record_patch_replace_real(yyjson_mut_doc *doc, const char *path, double num) {
    record_patch_with_real(doc, "replace", path, num);
}

/* jisp_stack_push_copy_and_log: Pushes a deep copy of elem onto stack and records a stack append; use for entrypoint literal pushes. */
static void jisp_stack_push_copy_and_log(yyjson_mut_doc *doc, yyjson_mut_val *stack, yyjson_mut_val *elem) {
    if (!doc || !stack || !elem) return;
    yyjson_mut_val *copy = jisp_mut_deep_copy(doc, elem);
    if (!copy) return;
    yyjson_mut_arr_append(stack, copy);
    record_patch_add_val(doc, "/stack/-", elem);
}

static void record_patch_remove(yyjson_mut_doc *doc, const char *path) {
    if (!doc || !path) return;
    if (!is_reversible_enabled(doc)) return;
    yyjson_mut_val *res = ensure_residual_array(doc);
    if (!res) return;

    yyjson_mut_val *patch = yyjson_mut_obj(doc);
    if (!patch) return;
    yyjson_mut_obj_add_strcpy(doc, patch, "op", "remove");
    yyjson_mut_obj_add_strcpy(doc, patch, "path", path);
    yyjson_mut_arr_append(res, patch);
}

/* Residual patch grouping helpers:
   - residual_group_begin: create a new array group if reversible is enabled (NULL otherwise).
   - residual_group_add_patch_with_{val,real}: append a patch object to the group, or fall back to single-entry logging.
   - residual_group_commit: append the whole group array to root["residual"].
   These helpers allow ops that perform multiple edits to group them for single-step undo. */
static yyjson_mut_val *residual_group_begin(yyjson_mut_doc *doc) {
    if (!doc) return NULL;
    if (!is_reversible_enabled(doc)) return NULL;
    return yyjson_mut_arr(doc);
}

static void residual_group_add_patch_with_val(yyjson_mut_doc *doc,
                                              yyjson_mut_val *group,
                                              const char *op,
                                              const char *path,
                                              yyjson_mut_val *val) {
    if (!doc || !op || !path) return;
    if (!group || !yyjson_mut_is_arr(group)) {
        /* Fallback to direct single-entry residual append */
        record_patch_with_val(doc, op, path, val);
        return;
    }
    yyjson_mut_val *patch = yyjson_mut_obj(doc);
    if (!patch) return;
    yyjson_mut_obj_add_strcpy(doc, patch, "op", op);
    yyjson_mut_obj_add_strcpy(doc, patch, "path", path);
    if (val) {
        yyjson_mut_val *vcopy = jisp_mut_deep_copy(doc, val);
        if (vcopy) {
            yyjson_mut_obj_add_val(doc, patch, "value", vcopy);
        }
    }
    yyjson_mut_arr_append(group, patch);
}

static void residual_group_add_patch_with_real(yyjson_mut_doc *doc,
                                               yyjson_mut_val *group,
                                               const char *op,
                                               const char *path,
                                               double num) {
    if (!doc || !op || !path) return;
    if (!group || !yyjson_mut_is_arr(group)) {
        /* Fallback to direct single-entry residual append */
        record_patch_with_real(doc, op, path, num);
        return;
    }
    yyjson_mut_val *patch = yyjson_mut_obj(doc);
    if (!patch) return;
    yyjson_mut_obj_add_strcpy(doc, patch, "op", op);
    yyjson_mut_obj_add_strcpy(doc, patch, "path", path);
    yyjson_mut_obj_add_real(doc, patch, "value", num);
    yyjson_mut_arr_append(group, patch);
}

static void residual_group_commit(yyjson_mut_doc *doc, yyjson_mut_val *group) {
    if (!doc || !group) return;
    if (!is_reversible_enabled(doc)) return;
    yyjson_mut_val *res = ensure_residual_array(doc);
    if (!res) return;
    yyjson_mut_arr_append(res, group);
}

/* jisp_stack_log_remove_last: Records the removal of the current top-of-stack index; call immediately before yyjson_mut_arr_remove_last so replay aligns. 
   Additionally records the value being removed in the patch's "value" field for potential undo. */
static void jisp_stack_log_remove_last(yyjson_mut_doc *doc, yyjson_mut_val *stack) {
    if (!doc || !stack) return;
    size_t sz = yyjson_mut_arr_size(stack);
    if (sz == 0) return;

    /* Build RFC 6901 path for the element to be removed. */
    char path[64];
    snprintf(path, sizeof(path), "/stack/%zu", sz - 1);

    /* Peek the current top element so we can include it in the residual patch. */
    yyjson_mut_val *last = NULL;
    yyjson_mut_arr_iter it;
    yyjson_mut_val *e;
    size_t i = 0;
    if (yyjson_mut_arr_iter_init(stack, &it)) {
        while ((e = yyjson_mut_arr_iter_next(&it))) {
            if (i == sz - 1) {
                last = e;
                break;
            }
            i++;
        }
    }

    if (last) {
        /* Record remove with the value for future undo. */
        record_patch_with_val(doc, "remove", path, last);
    } else {
        /* Fallback: record remove without value if we couldn't peek. */
        record_patch_remove(doc, path);
    }
}

/* Core "Opcodes" */


// 
/* pop_and_store: Stores a value under a key from the stack into the document; use when a [value, key] pair has been prepared on the stack. */
void pop_and_store(yyjson_mut_doc *doc) {
    yyjson_mut_val *root = get_root_fallible(doc, "pop_and_store");
    yyjson_mut_val *stack = get_stack_fallible(doc, "pop_and_store");
    if (yyjson_mut_arr_size(stack) < 2) {
        jisp_fatal(doc, "pop_and_store: need at least [value, key] on stack");
    }

    jisp_stack_log_remove_last(doc, stack);
    yyjson_mut_val *key_val_mut = yyjson_mut_arr_remove_last(stack);
    if (!yyjson_is_str((yyjson_val *)key_val_mut)) {
        jisp_fatal(doc, "pop_and_store: key must be a string");
    }
    const char *key = yyjson_get_str((yyjson_val *)key_val_mut);
    size_t key_len = strlen(key);
    char *key_in_doc = unsafe_yyjson_mut_strncpy(doc, key, key_len);

    jisp_stack_log_remove_last(doc, stack);
    yyjson_mut_val *value = yyjson_mut_arr_remove_last(stack);
    if (!value || !key_in_doc) {
        jisp_fatal(doc, "pop_and_store: failed to pop value or duplicate key");
    }
    bool existed = yyjson_mut_obj_get(root, key) != NULL;
    char *path_buf = jisp_build_path_for_key_temp(key);
    yyjson_mut_obj_add_val(doc, root, key_in_doc, value);
    if (path_buf) {
        if (existed) {
            record_patch_replace_val(doc, path_buf, value);
        } else {
            record_patch_add_val(doc, path_buf, value);
        }
        free(path_buf);
    }
}

/* duplicate_top: Duplicates the top stack value to model a pure stack operation; use to preserve and copy the current top. */
void duplicate_top(yyjson_mut_doc *doc) {
    yyjson_mut_val *stack = get_stack_fallible(doc, "duplicate_top");
    if (yyjson_mut_arr_size(stack) == 0) {
        jisp_fatal(doc, "duplicate_top: stack is empty");
    }
    /* log and pop the current top */
    jisp_stack_log_remove_last(doc, stack);
    yyjson_mut_val *last = yyjson_mut_arr_remove_last(stack);
    if (!last) {
        jisp_fatal(doc, "duplicate_top: failed to pop top of stack");
    }
    /* push original back and log */
    yyjson_mut_arr_append(stack, last);
    record_patch_add_val(doc, "/stack/-", last);
    /* push a deep copy as duplicate and log */
    yyjson_mut_val *dup = jisp_mut_deep_copy(doc, last);
    yyjson_mut_arr_append(stack, dup);
    record_patch_add_val(doc, "/stack/-", last);
}

/* add_two_top: Adds the two topmost numeric values and pushes the sum; use for simple arithmetic over the stack. */
void add_two_top(yyjson_mut_doc *doc) {
    yyjson_mut_val *stack = get_stack_fallible(doc, "add_two_top");
    if (yyjson_mut_arr_size(stack) < 2) {
        jisp_fatal(doc, "add_two_top: need at least two values on stack");
    }

    /* Begin a residual group so both pops and the push are undone together. */
    yyjson_mut_val *group = residual_group_begin(doc);

    /* Pop first operand (top of stack) and record remove */
    size_t sz = yyjson_mut_arr_size(stack);
    char path0[64];
    snprintf(path0, sizeof(path0), "/stack/%zu", sz - 1);
    yyjson_mut_val *val1_mut = yyjson_mut_arr_remove_last(stack);
    residual_group_add_patch_with_val(doc, group, "remove", path0, val1_mut);

    /* Pop second operand and record remove */
    sz = yyjson_mut_arr_size(stack);
    char path1[64];
    snprintf(path1, sizeof(path1), "/stack/%zu", sz - 1);
    yyjson_mut_val *val2_mut = yyjson_mut_arr_remove_last(stack);
    residual_group_add_patch_with_val(doc, group, "remove", path1, val2_mut);

    if (!val1_mut || !val2_mut) {
        jisp_fatal(doc, "add_two_top: failed to pop operands");
    }
    if (!yyjson_is_num((yyjson_val *)val1_mut) || !yyjson_is_num((yyjson_val *)val2_mut)) {
        jisp_fatal(doc, "add_two_top: operands must be numeric");
    }

    double val1 = (double)yyjson_mut_get_sint(val1_mut);
    double val2 = (double)yyjson_mut_get_sint(val2_mut);
    double sum = val1 + val2;
    yyjson_mut_arr_add_real(doc, stack, sum);
    residual_group_add_patch_with_real(doc, group, "add", "/stack/-", sum);

    if (group) residual_group_commit(doc, group);
}

void map_over(yyjson_mut_doc *doc) {
    yyjson_mut_val *stack = get_stack_fallible(doc, "map_over");
    if (yyjson_mut_arr_size(stack) < 2) {
        jisp_fatal(doc, "map_over: need at least [data_array, function_array] on stack");
    }

    yyjson_mut_val *group = residual_group_begin(doc);

    size_t sz = yyjson_mut_arr_size(stack);
    char path_func[64];
    snprintf(path_func, sizeof(path_func), "/stack/%zu", sz - 1);
    yyjson_mut_val *function_array = yyjson_mut_arr_remove_last(stack);
    residual_group_add_patch_with_val(doc, group, "remove", path_func, function_array);

    if (!function_array || !yyjson_mut_is_arr(function_array)) {
        jisp_fatal(doc, "map_over: top of stack must be a function array");
    }

    sz = yyjson_mut_arr_size(stack);
    char path_data[64];
    snprintf(path_data, sizeof(path_data), "/stack/%zu", sz - 1);
    yyjson_mut_val *data_array = yyjson_mut_arr_remove_last(stack);
    residual_group_add_patch_with_val(doc, group, "remove", path_data, data_array);

    if (!data_array || !yyjson_mut_is_arr(data_array)) {
        jisp_fatal(doc, "map_over: second item on stack must be a data array");
    }

    yyjson_mut_val *result_array = yyjson_mut_arr(doc);
    if (!result_array) {
        jisp_fatal(doc, "map_over: failed to create result array");
    }

    yyjson_mut_arr_iter it;
    yyjson_mut_val *data_point;
    yyjson_mut_arr_iter_init(data_array, &it);

    size_t original_stack_size = yyjson_mut_arr_size(stack);

    while ((data_point = yyjson_mut_arr_iter_next(&it))) {
        yyjson_mut_val *data_copy = jisp_mut_deep_copy(doc, data_point);
        yyjson_mut_arr_append(stack, data_copy);

        process_ep_array(doc, function_array, "/map_over/function");

        if (yyjson_mut_arr_size(stack) != original_stack_size + 1) {
            jisp_fatal(doc, "map_over: function must consume its argument and produce exactly one result on the stack. Stack size mismatch.");
        }

        yyjson_mut_val *result_element = yyjson_mut_arr_remove_last(stack);
        yyjson_mut_arr_append(result_array, result_element);
    }

    yyjson_mut_arr_append(stack, result_array);
    residual_group_add_patch_with_val(doc, group, "add", "/stack/-", result_array);

    if (group) residual_group_commit(doc, group);
}

// Custom function to replicate Python's `eval_code` for the example
/* calculate_final_result: Aggregates intermediate fields and optional stack top into final_result; use as a terminal computation step. */
void calculate_final_result(yyjson_mut_doc *doc) {
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
        jisp_stack_log_remove_last(doc, stack);
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
    bool existed_final = yyjson_mut_obj_get(root, "final_result") != NULL;
    yyjson_mut_obj_add_real(doc, root, "final_result", final_result);
    
    if (existed_final) {
        record_patch_replace_real(doc, "/final_result", final_result);
    } else {
        record_patch_add_real(doc, "/final_result", final_result);
    }
}

/* json_get: Pops a RFC 6901 path string and pushes the deep-copied value found at that path. */
void json_get(yyjson_mut_doc *doc) {
    yyjson_mut_val *root = get_root_fallible(doc, "get");
    yyjson_mut_val *stack = get_stack_fallible(doc, "get");
    if (yyjson_mut_arr_size(stack) < 1) {
        jisp_fatal(doc, "get: need at least [path] on stack");
    }

    yyjson_mut_val *group = residual_group_begin(doc);

    size_t sz = yyjson_mut_arr_size(stack);
    char path_idx[64];
    snprintf(path_idx, sizeof(path_idx), "/stack/%zu", sz - 1);
    yyjson_mut_val *path_val = yyjson_mut_arr_remove_last(stack);
    residual_group_add_patch_with_val(doc, group, "remove", path_idx, path_val);

    if (!yyjson_mut_is_str(path_val)) {
        jisp_fatal(doc, "get: path must be a string");
    }
    const char *path = yyjson_get_str((yyjson_val *)path_val);

    yyjson_mut_val *target = NULL;
    if (strcmp(path, "/") == 0) {
        target = root;
    } else {
        target = yyjson_mut_ptr_get(root, path);
    }
    if (!target) {
        jisp_fatal(doc, "get: path not found: %s", path);
    }

    yyjson_mut_val *copy = jisp_mut_deep_copy(doc, target);
    if (!copy) {
        jisp_fatal(doc, "get: failed to copy value");
    }
    yyjson_mut_arr_append(stack, copy);
    residual_group_add_patch_with_val(doc, group, "add", "/stack/-", copy);

    if (group) residual_group_commit(doc, group);
}

/* json_set: Pops [value, path] (top is path) and replaces the value at that path (scalars only). */
void json_set(yyjson_mut_doc *doc) {
    yyjson_mut_val *root = get_root_fallible(doc, "set");
    yyjson_mut_val *stack = get_stack_fallible(doc, "set");
    if (yyjson_mut_arr_size(stack) < 2) {
        jisp_fatal(doc, "set: need at least [value, path] on stack");
    }

    yyjson_mut_val *group = residual_group_begin(doc);

    /* Pop path */
    size_t sz = yyjson_mut_arr_size(stack);
    char path_idx[64];
    snprintf(path_idx, sizeof(path_idx), "/stack/%zu", sz - 1);
    yyjson_mut_val *path_val = yyjson_mut_arr_remove_last(stack);
    residual_group_add_patch_with_val(doc, group, "remove", path_idx, path_val);
    if (!yyjson_mut_is_str(path_val)) {
        jisp_fatal(doc, "set: path must be a string");
    }
    const char *path = yyjson_get_str((yyjson_val *)path_val);

    /* Pop value */
    sz = yyjson_mut_arr_size(stack);
    char val_idx[64];
    snprintf(val_idx, sizeof(val_idx), "/stack/%zu", sz - 1);
    yyjson_mut_val *value = yyjson_mut_arr_remove_last(stack);
    residual_group_add_patch_with_val(doc, group, "remove", val_idx, value);

    /* Resolve target */
    yyjson_mut_val *target = NULL;
    if (strcmp(path, "/") == 0) {
        target = root;
    } else {
        target = yyjson_mut_ptr_get(root, path);
    }
    if (!target) {
        jisp_fatal(doc, "set: path not found: %s", path);
    }

    /* Only allow scalar assignments for now. */
    yyjson_type vt = unsafe_yyjson_get_type(value);
    switch (vt) {
        case YYJSON_TYPE_NULL: {
            unsafe_yyjson_set_tag(target, YYJSON_TYPE_NULL, YYJSON_SUBTYPE_NONE, 0);
            break;
        }
        case YYJSON_TYPE_BOOL: {
            bool b = yyjson_get_bool((yyjson_val *)value);
            unsafe_yyjson_set_bool(target, b);
            break;
        }
        case YYJSON_TYPE_NUM: {
            int64_t n = (int64_t)yyjson_mut_get_sint(value);
            unsafe_yyjson_set_sint(target, n);
            break;
        }
        case YYJSON_TYPE_STR: {
            const char *s = unsafe_yyjson_get_str(value);
            size_t slen = unsafe_yyjson_get_len(value);
            char *dup = unsafe_yyjson_mut_strncpy(doc, s ? s : "", s ? slen : 0);
            if (!dup) {
                jisp_fatal(doc, "set: out of memory duplicating string");
            }
            unsafe_yyjson_set_strn(target, dup, s ? slen : 0);
            break;
        }
        default:
            jisp_fatal(doc, "set: value must be a scalar (null, bool, number, or string)");
    }

    /* Record replace at path with provided value */
    residual_group_add_patch_with_val(doc, group, "replace", path, value);

    if (group) residual_group_commit(doc, group);
}

/* json_append: Pops [value, path] (top is path) and appends value to array at path. */
void json_append(yyjson_mut_doc *doc) {
    yyjson_mut_val *root = get_root_fallible(doc, "append");
    yyjson_mut_val *stack = get_stack_fallible(doc, "append");
    if (yyjson_mut_arr_size(stack) < 2) {
        jisp_fatal(doc, "append: need at least [value, path] on stack");
    }

    yyjson_mut_val *group = residual_group_begin(doc);

    /* Pop path */
    size_t sz = yyjson_mut_arr_size(stack);
    char path_idx[64];
    snprintf(path_idx, sizeof(path_idx), "/stack/%zu", sz - 1);
    yyjson_mut_val *path_val = yyjson_mut_arr_remove_last(stack);
    residual_group_add_patch_with_val(doc, group, "remove", path_idx, path_val);
    if (!yyjson_mut_is_str(path_val)) {
        jisp_fatal(doc, "append: path must be a string");
    }
    const char *path = yyjson_get_str((yyjson_val *)path_val);

    /* Pop value */
    sz = yyjson_mut_arr_size(stack);
    char val_idx[64];
    snprintf(val_idx, sizeof(val_idx), "/stack/%zu", sz - 1);
    yyjson_mut_val *value = yyjson_mut_arr_remove_last(stack);
    residual_group_add_patch_with_val(doc, group, "remove", val_idx, value);

    /* Resolve array */
    yyjson_mut_val *arr = NULL;
    if (strcmp(path, "/") == 0) {
        arr = root;
    } else {
        arr = yyjson_mut_ptr_get(root, path);
    }
    if (!arr || !yyjson_mut_is_arr(arr)) {
        jisp_fatal(doc, "append: path must resolve to an array");
    }

    /* Append deep copy */
    yyjson_mut_val *copy = jisp_mut_deep_copy(doc, value);
    if (!copy) {
        jisp_fatal(doc, "append: failed to copy value");
    }
    yyjson_mut_arr_append(arr, copy);

    /* Build append path "<path>/-" (root "/" special-cased to "/-") */
    size_t plen = strlen(path);
    size_t out_len = (strcmp(path, "/") == 0 ? 2 : plen + 2);
    char *apath = (char *)malloc(out_len + 1);
    if (!apath) {
        jisp_fatal(doc, "append: out of memory building patch path");
    }
    if (strcmp(path, "/") == 0) {
        strcpy(apath, "/-");
    } else {
        memcpy(apath, path, plen);
        apath[plen] = '/';
        apath[plen + 1] = '-';
        apath[plen + 2] = '\0';
    }

    residual_group_add_patch_with_val(doc, group, "add", apath, value);
    free(apath);

    if (group) residual_group_commit(doc, group);
}

/* ptr_new: Pops a path string from stack, resolves to jpm_ptr, pushes to C pointer stack. */
void ptr_new(yyjson_mut_doc *doc) {
    yyjson_mut_val *stack = get_stack_fallible(doc, "ptr_new");
    if (yyjson_mut_arr_size(stack) < 1) {
        jisp_fatal(doc, "ptr_new: need [path] on stack");
    }
    
    /* Pop path string */
    jisp_stack_log_remove_last(doc, stack);
    yyjson_mut_val *path_val = yyjson_mut_arr_remove_last(stack);
    if (!yyjson_mut_is_str(path_val)) {
        jisp_fatal(doc, "ptr_new: path must be a string");
    }
    const char *path = yyjson_get_str((yyjson_val *)path_val);
    
    jpm_ptr p;
    jpm_status st = jpm_return(doc, path, &p);
    if (st != JPM_OK) {
        jisp_fatal(doc, "ptr_new: resolution failed for path '%s' (status %d)", path, st);
    }
    
    /* Push to C stack */
    ptr_stack_push(p);
}

/* ptr_release_op: Pops the top pointer from C pointer stack and releases it. */
void ptr_release_op(yyjson_mut_doc *doc) {
    (void)doc;
    jpm_ptr p = ptr_stack_pop();
    jpm_ptr_release(&p);
}

/* ptr_get: Peeks the top pointer, gets its value, deep copies it to stack. */
void ptr_get(yyjson_mut_doc *doc) {
    yyjson_mut_val *stack = get_stack_fallible(doc, "ptr_get");
    jpm_ptr p = ptr_stack_peek();
    
    if (!jpm_is_valid(p)) {
        jisp_fatal(doc, "ptr_get: invalid pointer handle");
    }
    
    yyjson_mut_val *val = jpm_value(p);
    if (!val) {
        jisp_fatal(doc, "ptr_get: pointer has null value (stale?)");
    }
    
    yyjson_mut_val *copy = jisp_mut_deep_copy(doc, val);
    if (!copy) {
        jisp_fatal(doc, "ptr_get: copy failed");
    }
    
    yyjson_mut_arr_append(stack, copy);
    record_patch_add_val(doc, "/stack/-", copy);
}

/* ptr_set: Peeks the top pointer, pops value from stack, overwrites pointer target. */
void ptr_set(yyjson_mut_doc *doc) {
    yyjson_mut_val *stack = get_stack_fallible(doc, "ptr_set");
    if (yyjson_mut_arr_size(stack) < 1) {
        jisp_fatal(doc, "ptr_set: need [value] on stack");
    }
    
    jpm_ptr p = ptr_stack_peek();
    if (!jpm_is_valid(p)) {
        jisp_fatal(doc, "ptr_set: invalid pointer handle");
    }
    
    /* Pop value from stack */
    jisp_stack_log_remove_last(doc, stack);
    yyjson_mut_val *val = yyjson_mut_arr_remove_last(stack);
    
    yyjson_mut_val *target = jpm_value(p);
    
    /* Overwrite target with val's content. Note: this changes the value in-place. */
    /* We can't easily replace a container with a scalar or vice versa using yyjson_mut_val* 
       without potentially corrupting the parent if we aren't careful, but yyjson_mut allows
       modifying the type tag. */
    
    yyjson_type vt = unsafe_yyjson_get_type(val);
    
    /* Basic scalar replacement logic */
    switch (vt) {
        case YYJSON_TYPE_NULL:
            unsafe_yyjson_set_tag(target, YYJSON_TYPE_NULL, YYJSON_SUBTYPE_NONE, 0);
            break;
        case YYJSON_TYPE_BOOL:
            unsafe_yyjson_set_bool(target, yyjson_get_bool((yyjson_val*)val));
            break;
        case YYJSON_TYPE_NUM:
            /* Assuming integer for simplicity, likely need generic number copy logic if real */
            if (yyjson_is_int((yyjson_val*)val))
                unsafe_yyjson_set_sint(target, yyjson_get_int((yyjson_val*)val));
            else
                unsafe_yyjson_set_real(target, yyjson_get_real((yyjson_val*)val));
            break;
        case YYJSON_TYPE_STR: {
            const char *s = unsafe_yyjson_get_str(val);
            size_t len = unsafe_yyjson_get_len(val);
            char *copy = unsafe_yyjson_mut_strncpy(doc, s, len); /* copy to doc's arena */
            unsafe_yyjson_set_strn(target, copy, len);
            break;
        }
        /* For containers, deep copy is complex because we need to clone children into doc's arena. 
           But 'val' is already in 'doc' (since it came from stack). 
           So we can technically just memcpy the struct? 
           No, yyjson_mut_val forms a tree. 
           Ideally, we want to 'become' the other value. */
        case YYJSON_TYPE_ARR:
        case YYJSON_TYPE_OBJ: {
             /* If target and val are both in doc, we can shallow copy the head struct?
                yyjson_mut_val is opaque but generally small. 
                However, doing this properly requires internal knowledge or full copy. 
                For now, let's allow scalar sets only to match json_set behavior safety. */
             jisp_fatal(doc, "ptr_set: currently supports scalar values only");
             break;
        }
        default:
            jisp_fatal(doc, "ptr_set: unknown value type");
    }
    
    /* TODO: Residual logging for ptr_set (requires path, which is optional in jpm_ptr) */
}

/* print_json: Displays the current document contents; use at the end of execution or for user-facing inspection. */
void print_json(yyjson_mut_doc *doc) {
    char *json_str = jisp_json_to_pretty_string(doc);
    if (json_str) {
        printf("%s\n", json_str);
        free(json_str);
    }
}

/* undo_last_residual: Best-effort undo for the last residual entry.
   Now supports grouped entries where the last residual item is an array of patch objects;
   the group will be undone in reverse order. */
static void undo_one_patch(yyjson_mut_doc *doc, yyjson_mut_val *patch) {
    if (!doc || !patch) return;

    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!root) {
        jisp_fatal(doc, "undo_last_residual: missing root");
    }

    if (!yyjson_mut_is_obj(patch)) {
        jisp_fatal(doc, "undo_last_residual: residual entry is not an object");
    }

    yyjson_mut_val *opv = yyjson_mut_obj_get(patch, "op");
    yyjson_mut_val *pathv = yyjson_mut_obj_get(patch, "path");
    if (!opv || !pathv || !yyjson_mut_is_str(opv) || !yyjson_mut_is_str(pathv)) {
        jisp_fatal(doc, "undo_last_residual: residual entry must have string 'op' and 'path'");
    }

    const char *op = yyjson_get_str((yyjson_val *)opv);
    const char *path = yyjson_get_str((yyjson_val *)pathv);

    if (strcmp(op, "add") == 0) {
        /* Best-effort undo: if this was a stack push, pop it; otherwise no-op. */
        if (strcmp(path, "/stack/-") == 0) {
            yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
            if (!stack || !yyjson_mut_is_arr(stack) || yyjson_mut_arr_size(stack) == 0) {
                /* Nothing to undo; best-effort no-op. */
                return;
            }
            (void)yyjson_mut_arr_remove_last(stack);
        } else {
            /* Unsupported path for now: best-effort no-op. */
            return;
        }
    } else if (strcmp(op, "replace") == 0) {
        /* No previous value captured in minimal mode; best-effort no-op. */
        return;
    } else if (strcmp(op, "remove") == 0) {
        /* If the patch captured the removed value, we can best-effort restore it.
           Currently support stack removals like "/stack/<idx>" by re-appending the saved value. */
        yyjson_mut_val *valv = yyjson_mut_obj_get(patch, "value");
        if (!valv) {
            /* No saved value; best-effort no-op. */
            return;
        }
        if (strncmp(path, "/stack/", 7) == 0) {
            yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
            if (!stack || !yyjson_mut_is_arr(stack)) return;
            yyjson_mut_val *copy = jisp_mut_deep_copy(doc, valv);
            if (copy) {
                yyjson_mut_arr_append(stack, copy);
            }
            return;
        }
        /* Unsupported path kinds: best-effort no-op. */
        return;
    } else {
        /* Unknown op: best-effort no-op. */
        return;
    }
}

void undo_last_residual(yyjson_mut_doc *doc) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!root) {
        jisp_fatal(doc, "undo_last_residual: missing root");
    }
    yyjson_mut_val *residual = yyjson_mut_obj_get(root, "residual");
    if (!residual || !yyjson_mut_is_arr(residual) || yyjson_mut_arr_size(residual) == 0) {
        jisp_fatal(doc, "undo_last_residual: 'residual' is missing or empty");
    }

    yyjson_mut_val *entry = yyjson_mut_arr_remove_last(residual);
    if (!entry) {
        jisp_fatal(doc, "undo_last_residual: failed to pop residual entry");
    }

    if (yyjson_mut_is_obj(entry)) {
        /* Single patch object */
        undo_one_patch(doc, entry);
        return;
    } else if (yyjson_mut_is_arr(entry)) {
        /* Grouped patches: undo in reverse order */
        while (yyjson_mut_arr_size(entry) > 0) {
            yyjson_mut_val *patch = yyjson_mut_arr_remove_last(entry);
            if (!patch || !yyjson_mut_is_obj(patch)) {
                jisp_fatal(doc, "undo_last_residual: grouped residual contains non-object entry");
            }
            undo_one_patch(doc, patch);
        }
        return;
    } else {
        jisp_fatal(doc, "undo_last_residual: top residual entry must be an object or array of objects");
    }
}


/* run_tokens: Executes a sequence of tokens against the JSON stack; use to run small programs where ops manipulate root["stack"]. */
static void run_tokens(yyjson_mut_doc *doc, const jisp_tok *toks, size_t count) {
    if (!doc || !toks) return;
    yyjson_mut_val *stack = get_stack_fallible(doc, "run_tokens");

    for (size_t i = 0; i < count; i++) {
        jisp_tok t = toks[i];
        switch (t.kind) {
            case JISP_TOK_INT:
                yyjson_mut_arr_add_sint(doc, stack, t.as.i);
                break;
            case JISP_TOK_REAL:
                yyjson_mut_arr_add_real(doc, stack, t.as.d);
                break;
            case JISP_TOK_STR:
                yyjson_mut_arr_add_strcpy(doc, stack, t.as.s ? t.as.s : "");
                break;
            case JISP_TOK_JPM:
                jisp_fatal(doc, "JPM tokens not yet supported in run_tokens");
                break;
            case JISP_TOK_OP:
                if (!t.as.op) jisp_fatal(doc, "NULL op in token stream at index %zu", i);
                t.as.op(doc);
                break;
            default:
                jisp_fatal(doc, "Unknown token kind at index %zu", i);
        }
    }
}

/* process_ep_array: Interprets an entrypoint-like array of literals and directives; use for root entrypoint and nested '.' arrays. */
static void process_ep_array(yyjson_mut_doc *doc, yyjson_mut_val *ep, const char *path_prefix) {
    if (!doc || !ep) return;
    if (!yyjson_mut_is_arr(ep)) {
        jisp_fatal(doc, "entrypoint must be an array");
    }

    yyjson_mut_val *stack = get_stack_fallible(doc, "process_entrypoint");

    yyjson_mut_arr_iter it;
    yyjson_mut_val *elem;
    if (!yyjson_mut_arr_iter_init(ep, &it)) return;

    size_t idx = 0;
    while ((elem = yyjson_mut_arr_iter_next(&it))) {
        if (yyjson_mut_is_str(elem)) {
            jisp_stack_push_copy_and_log(doc, stack, elem);
        } else if (yyjson_is_num((yyjson_val *)elem)) {
            jisp_stack_push_copy_and_log(doc, stack, elem);
        } else if (yyjson_mut_is_arr(elem)) {
            jisp_stack_push_copy_and_log(doc, stack, elem);
        } else if (yyjson_mut_is_obj(elem)) {
            /* Special-case: object with "." field: array → execute as entrypoint; string → run op if found */
            yyjson_mut_val *dot = yyjson_mut_obj_get(elem, ".");
            if (dot) {
                if (yyjson_mut_is_arr(dot)) {
                    char nested_path[1024];
                    snprintf(nested_path, sizeof(nested_path), "%s/%zu/.", path_prefix, idx);
                    process_ep_array(doc, dot, nested_path);
                } else if (yyjson_mut_is_str(dot)) {
                    const char *name = yyjson_get_str((yyjson_val *)dot);
                    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
                    yyjson_mut_val *target_array = yyjson_mut_obj_get(root, name);

                    if (target_array && yyjson_mut_is_arr(target_array)) {
                        // This is a nested entrypoint call to a root-level array
                        char target_path[512];
                        snprintf(target_path, sizeof(target_path), "/%s", name);
                        process_ep_array(doc, target_array, target_path);

                    } else {
                        // Fallback to existing opcode lookup
                        jisp_op op = jisp_op_registry_get(name);
                        if (op) {
                            op(doc);
                        } else {
                            /* Unknown op name; treat the object as a literal */
                            yyjson_mut_arr_append(stack, jisp_mut_deep_copy(doc, elem));
                        }
                    }
                } else {
                    jisp_fatal(doc, "entrypoint object '.' field must be an array or string");
                }
            } else {
                yyjson_mut_arr_append(stack, jisp_mut_deep_copy(doc, elem));
            }
        } else {
            jisp_fatal(doc, "entrypoint element is not a string, number, array, or object");
        }
        idx++;
    }
}

/* process_entrypoint: Top-level driver for entrypoint arrays; use to seed the stack from initial program data and nested directives. */
static void process_entrypoint(yyjson_mut_doc *doc) {
    if (!doc) return;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!root) return;
    yyjson_mut_val *ep = yyjson_mut_obj_get(root, "entrypoint");
    if (!ep) return;
    process_ep_array(doc, ep, "/entrypoint");
}

/* main: Orchestrates program flow from input file to execution and output; run as the CLI entry point. */
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
    jpm_doc_retain(doc);

    yyjson_doc_free(in);

    process_entrypoint(doc);
    print_json(doc);
    jpm_doc_release(doc);
    free(buf);
    ptr_stack_free_all();
    jisp_op_registry_free();
    return 0;
}
