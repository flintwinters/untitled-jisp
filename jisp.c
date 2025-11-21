#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdarg.h>
#include <execinfo.h>
#include <bfd.h>
#include <dlfcn.h>
#include <ctype.h>
#include "yyjson.h"

// --- Color Definitions ---
#define C_ANY(r, g, b)   "\033[38;2;" #r ";" #g ";" #b "m"
#define C_RESET          "\033[0m"
#define C_RED            "\033[0;31m"
#define C_GREEN          "\033[0;32m"
#define C_BRIGHT_GREEN   "\033[1;32m"
#define C_DARK_GREEN     "\033[2;32m"
#define C_ORANGE         C_ANY(255, 140, 0)
#define C_DARK_ORANGE    C_ANY(200, 100, 0)
#define C_YELLOW         "\033[0;33m"
#define C_BRIGHT_BLUE    "\033[1;34m"
#define C_BLUE           "\033[0;34m"
#define C_CYAN           "\033[0;36m"
#define C_DARK_CYAN      "\033[2;36m"
#define C_MAGENTA        "\033[0;35m"
#define C_GRAY           "\033[0;90m"

/* Error handling context and helpers */
typedef struct jisp_ctx {
    const char *filename;
    const char *src;
    size_t src_len;
} jisp_ctx;

static jisp_ctx g_jisp_ctx = {0};

static bool g_opt_raw = false;
static bool g_opt_compact = false;

// --- BFD Data Structure for Caching ---
typedef struct BfdData {
    char* path;
    bfd* abfd;
    asymbol** syms;
    struct BfdData* next;
} BfdData;

static BfdData* bfd_cache_head = NULL;

// --- BFD Cleanup ---
static void cleanup_bfd_cache(void) {
    BfdData* current = bfd_cache_head;
    while (current) {
        BfdData* next = current->next;
        if (current->syms) {
            free(current->syms);
        }
        if (current->abfd) {
            bfd_close(current->abfd);
        }
        free(current->path);
        free(current);
        current = next;
    }
    bfd_cache_head = NULL;
}

// --- Helper function to open BFD and read symbols for a given file ---
static BfdData* get_bfd_data_for_file(const char* path) {
    // Search in cache first
    for (BfdData* iter = bfd_cache_head; iter != NULL; iter = iter->next) {
        if (strcmp(iter->path, path) == 0) {
            return iter;
        }
    }

    // Not in cache, create a new entry
    BfdData* new_data = (BfdData*)calloc(1, sizeof(BfdData));
    if (!new_data) return NULL;
    new_data->path = strdup(path);
    if (!new_data->path) {
        free(new_data);
        return NULL;
    }

    // Initialize BFD
    bfd* abfd = bfd_openr(path, NULL);
    if (!abfd) {
        // Store failure and return
        new_data->next = bfd_cache_head;
        bfd_cache_head = new_data;
        return new_data;
    }

    if (!bfd_check_format(abfd, bfd_object)) {
        bfd_close(abfd);
        new_data->next = bfd_cache_head;
        bfd_cache_head = new_data;
        return new_data;
    }

    long storage_needed = bfd_get_symtab_upper_bound(abfd);
    if (storage_needed <= 0) {
        bfd_close(abfd);
        new_data->next = bfd_cache_head;
        bfd_cache_head = new_data;
        return new_data;
    }

    asymbol** syms = (asymbol**)malloc(storage_needed);
    if (!syms) {
        bfd_close(abfd);
        new_data->next = bfd_cache_head;
        bfd_cache_head = new_data;
        return new_data;
    }

    long symcount = bfd_canonicalize_symtab(abfd, syms);
    if (symcount < 0) {
        free(syms);
        bfd_close(abfd);
        new_data->next = bfd_cache_head;
        bfd_cache_head = new_data;
        return new_data;
    }

    // Success
    new_data->abfd = abfd;
    new_data->syms = syms;

    // Add to cache
    new_data->next = bfd_cache_head;
    bfd_cache_head = new_data;

    return new_data;
}

static void print_c_stacktrace(const char* msg) {
    printf("\033[1;31m -- %s --\n", msg ? msg : "STACK TRACE");
    // bfd_init() must be called once
    static int bfd_initialized = 0;
    if (!bfd_initialized) {
        bfd_init();
        atexit(cleanup_bfd_cache);
        bfd_initialized = 1;
    }

    void *array[100];
    int size;
    
    // Get the raw stack addresses
    size = backtrace(array, 100);

    // Iterate over addresses and resolve them
    for (int i = 0; i < size; i++) {
        bfd_vma addr = (bfd_vma)array[i];
        Dl_info info; 

        // Step A: Use dladdr() for basic symbol/file info (works for ALL libraries)
        if (!dladdr((void*)addr, &info) || !info.dli_fname) {
             printf(C_YELLOW "0x%lx" C_RESET " " C_RED "(dladdr failed)" C_RESET "\n", (long)addr);
             continue;
        }

        const char *filename = info.dli_fname;
        const char *functionname = info.dli_sname ? info.dli_sname : "??";
        unsigned int line = 0;
        
        // Step B: Use BFD to find line number information for any library/executable
        BfdData* bfd_data = get_bfd_data_for_file(info.dli_fname);
        if (bfd_data && bfd_data->abfd && bfd_data->syms) {
            bfd_vma adj_addr = addr - (bfd_vma)info.dli_fbase;
            asection *sec;
            bfd_boolean found = 0;
            
            for (sec = bfd_data->abfd->sections; sec != NULL; sec = sec->next) {
                if ((bfd_section_flags(sec) & SEC_ALLOC) == 0) {
                    continue;
                }

                bfd_vma sec_vma = bfd_section_vma(sec);
                bfd_size_type sec_size = bfd_section_size(sec);

                if (adj_addr >= sec_vma && adj_addr < sec_vma + sec_size) {
                    found = bfd_find_nearest_line(bfd_data->abfd, sec, bfd_data->syms, adj_addr - sec_vma,
                                                  &filename, &functionname, &line);
                    if (found) break;
                }
            }
            
            if (found && line > 0) {
                // Success: format the output like addr2line
                 printf(C_YELLOW "0x%lx\033[10G" C_GREEN "%s" C_RESET ":" C_MAGENTA "%u " C_DARK_CYAN "%s" C_RESET "\n", 
                        (long)(addr & 0xffffff),
                        functionname ? functionname : "??", 
                        line,
                        (strrchr(filename, '/') ? strrchr(filename, '/') + 1 : filename));
                continue;
            }
        }
        
        // Fallback if BFD didn't give line info but dladdr did
        printf(C_YELLOW "0x%lx\033[10G" C_GREEN "%s" C_RESET " " C_DARK_CYAN "%s" C_RESET "\n", 
            (long)(addr & 0xffffff),
            functionname ? functionname : "??",
            (strrchr(filename, '/') ? strrchr(filename, '/') + 1 : filename));

    }
    printf("\033[1;31m -- END TRACE --\n" C_RESET);
}

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
    
    // Capture formatted message for stack trace
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    print_c_stacktrace(buf);

    jisp_dump_state(doc);
    fflush(NULL);
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
    
    // Capture formatted message for stack trace
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    print_c_stacktrace(buf);

    jisp_report_pos(source_name, src, len, pos);
    jisp_dump_state(doc);
    fflush(NULL);
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

/* REQUIRE_STACK: Macro to fetch the stack and ensure it has at least 'required_size' elements; fatal otherwise. */
#define REQUIRE_STACK(doc, required_size)\
    ({\
        yyjson_mut_val *__stack = get_stack_fallible(doc, __func__);\
        if (yyjson_mut_arr_size(__stack) < (required_size)) {\
            jisp_fatal(doc, "%s: need at least %zu values on stack", __func__, (size_t)(required_size));\
        }\
        __stack;\
    })


/* JISP op function signature (no explicit args) */
typedef void (*jisp_op)(yyjson_mut_doc *doc);

/* Forward declare jpm_ptr for token union pointer kind. */
typedef struct jpm_ptr jpm_ptr;

static void process_ep_array(yyjson_mut_doc *doc, yyjson_mut_val *ep, const char *path_prefix);

/* JISP op forward declarations for registry */
void pop_and_store(yyjson_mut_doc *doc);
void duplicate_top(yyjson_mut_doc *doc);
void add_two_top(yyjson_mut_doc *doc);
void map_over(yyjson_mut_doc *doc);
void json_get(yyjson_mut_doc *doc);
void json_set(yyjson_mut_doc *doc);
void json_append(yyjson_mut_doc *doc);
void ptr_new(yyjson_mut_doc *doc);
void ptr_release_op(yyjson_mut_doc *doc);
void ptr_get(yyjson_mut_doc *doc);
void ptr_set(yyjson_mut_doc *doc);
void print_json(yyjson_mut_doc *doc);
void undo_jisp_op(yyjson_mut_doc *doc);
void step_jisp_op(yyjson_mut_doc *doc);

/* Global JISP op registry (JSON document) */
typedef enum jisp_op_id {
    JISP_OP_POP_AND_STORE = 1,
    JISP_OP_DUPLICATE_TOP = 2,
    JISP_OP_ADD_TWO_TOP = 3,
    JISP_OP_PRINT_JSON = 5,
    JISP_OP_UNDO = 6,
    JISP_OP_MAP_OVER = 7,
    JISP_OP_GET = 8,
    JISP_OP_SET = 9,
    JISP_OP_APPEND = 10,
    JISP_OP_PTR_NEW = 11,
    JISP_OP_PTR_RELEASE = 12,
    JISP_OP_PTR_GET = 13,
    JISP_OP_PTR_SET = 14,
    JISP_OP_ENTER = 15,
    JISP_OP_EXIT = 16,
    JISP_OP_TEST = 17,
    JISP_OP_PRINT_ERROR = 18,
    JISP_OP_LOAD = 19,
    JISP_OP_STORE = 20,
    JISP_OP_STEP = 21
} jisp_op_id;

void enter(yyjson_mut_doc *doc);
void op_exit(yyjson_mut_doc *doc);
void op_test(yyjson_mut_doc *doc);
void op_print_error(yyjson_mut_doc *doc);
void op_load(yyjson_mut_doc *doc);
void op_store(yyjson_mut_doc *doc);

static void process_entrypoint(yyjson_mut_doc *doc);

static yyjson_mut_doc *g_jisp_op_registry = NULL;

static jisp_op jisp_op_from_id(int id) {
    switch (id) {
        case JISP_OP_POP_AND_STORE: return pop_and_store;
        case JISP_OP_DUPLICATE_TOP: return duplicate_top;
        case JISP_OP_ADD_TWO_TOP: return add_two_top;
        case JISP_OP_PRINT_JSON: return print_json;
        case JISP_OP_UNDO: return undo_jisp_op;
        case JISP_OP_MAP_OVER: return map_over;
        case JISP_OP_GET: return json_get;
        case JISP_OP_SET: return json_set;
        case JISP_OP_APPEND: return json_append;
        case JISP_OP_PTR_NEW: return ptr_new;
        case JISP_OP_PTR_RELEASE: return ptr_release_op;
        case JISP_OP_PTR_GET: return ptr_get;
        case JISP_OP_PTR_SET: return ptr_set;
        case JISP_OP_ENTER: return enter;
        case JISP_OP_EXIT: return op_exit;
        case JISP_OP_TEST: return op_test;
        case JISP_OP_PRINT_ERROR: return op_print_error;
        case JISP_OP_LOAD: return op_load;
        case JISP_OP_STORE: return op_store;
        case JISP_OP_STEP: return step_jisp_op;
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
    yyjson_mut_obj_add_int(d, root, "print_json", JISP_OP_PRINT_JSON);
    yyjson_mut_obj_add_int(d, root, "undo", JISP_OP_UNDO);
    yyjson_mut_obj_add_int(d, root, "map_over", JISP_OP_MAP_OVER);
    yyjson_mut_obj_add_int(d, root, "get", JISP_OP_GET);
    yyjson_mut_obj_add_int(d, root, "set", JISP_OP_SET);
    yyjson_mut_obj_add_int(d, root, "append", JISP_OP_APPEND);
    yyjson_mut_obj_add_int(d, root, "ptr_new", JISP_OP_PTR_NEW);
    yyjson_mut_obj_add_int(d, root, "ptr_release", JISP_OP_PTR_RELEASE);
    yyjson_mut_obj_add_int(d, root, "ptr_get", JISP_OP_PTR_GET);
    yyjson_mut_obj_add_int(d, root, "ptr_set", JISP_OP_PTR_SET);
    yyjson_mut_obj_add_int(d, root, "enter", JISP_OP_ENTER);
    yyjson_mut_obj_add_int(d, root, "exit", JISP_OP_EXIT);
    yyjson_mut_obj_add_int(d, root, "test", JISP_OP_TEST);
    yyjson_mut_obj_add_int(d, root, "print_error", JISP_OP_PRINT_ERROR);
    yyjson_mut_obj_add_int(d, root, "load", JISP_OP_LOAD);
    yyjson_mut_obj_add_int(d, root, "store", JISP_OP_STORE);
    yyjson_mut_obj_add_int(d, root, "step", JISP_OP_STEP);
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
    Internal helpers for a root-level "ref" field on yyjson_mut_doc.
    These manage retain/release semantics without altering public behavior.
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
    return yyjson_mut_val_mut_copy(doc, val);
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
    yyjson_mut_val *stack = REQUIRE_STACK(doc, 2);

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
    yyjson_mut_val *stack = REQUIRE_STACK(doc, 1);
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
    yyjson_mut_val *stack = REQUIRE_STACK(doc, 2);

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
    yyjson_mut_arr_add_int(doc, stack, sum);
    residual_group_add_patch_with_real(doc, group, "add", "/stack/-", sum);

    if (group) residual_group_commit(doc, group);
}

static void map_over_iterate(yyjson_mut_doc *doc, yyjson_mut_val *stack, yyjson_mut_val *data_array, yyjson_mut_val *func_array, yyjson_mut_val *result_array) {
    yyjson_mut_arr_iter it;
    yyjson_mut_val *data_point;
    yyjson_mut_arr_iter_init(data_array, &it);
    size_t original_stack_size = yyjson_mut_arr_size(stack);

    while ((data_point = yyjson_mut_arr_iter_next(&it))) {
        yyjson_mut_val *data_copy = jisp_mut_deep_copy(doc, data_point);
        yyjson_mut_arr_append(stack, data_copy);

        process_ep_array(doc, func_array, "/map_over/function");

        if (yyjson_mut_arr_size(stack) != original_stack_size + 1) {
            jisp_fatal(doc, "map_over: function must consume its argument and produce exactly one result on the stack. Stack size mismatch.");
        }

        yyjson_mut_val *result_element = yyjson_mut_arr_remove_last(stack);
        yyjson_mut_arr_append(result_array, result_element);
    }
}

void map_over(yyjson_mut_doc *doc) {
    yyjson_mut_val *stack = REQUIRE_STACK(doc, 2);

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

    map_over_iterate(doc, stack, data_array, function_array, result_array);

    yyjson_mut_arr_append(stack, result_array);
    residual_group_add_patch_with_val(doc, group, "add", "/stack/-", result_array);

    if (group) residual_group_commit(doc, group);
}

static void assign_scalar_to_target(yyjson_mut_doc *doc, yyjson_mut_val *target, yyjson_mut_val *val, const char *ctx) {
    yyjson_type vt = unsafe_yyjson_get_type(val);
    switch (vt) {
        case YYJSON_TYPE_NULL:
            unsafe_yyjson_set_tag(target, YYJSON_TYPE_NULL, YYJSON_SUBTYPE_NONE, 0);
            break;
        case YYJSON_TYPE_BOOL:
            unsafe_yyjson_set_bool(target, yyjson_get_bool((yyjson_val*)val));
            break;
        case YYJSON_TYPE_NUM:
            if (yyjson_is_int((yyjson_val*)val))
                unsafe_yyjson_set_sint(target, yyjson_get_int((yyjson_val*)val));
            else
                unsafe_yyjson_set_real(target, yyjson_get_real((yyjson_val*)val));
            break;
        case YYJSON_TYPE_STR: {
            const char *s = unsafe_yyjson_get_str(val);
            size_t len = unsafe_yyjson_get_len(val);
            char *copy = unsafe_yyjson_mut_strncpy(doc, s, len);
            if (!copy) jisp_fatal(doc, "%s: out of memory copying string", ctx);
            unsafe_yyjson_set_strn(target, copy, len);
            break;
        }
        default:
            jisp_fatal(doc, "%s: value must be a scalar (null, bool, number, or string)", ctx);
    }
}

/* json_get: Pops a RFC 6901 path string and pushes the deep-copied value found at that path. */
void json_get(yyjson_mut_doc *doc) {
    yyjson_mut_val *root = get_root_fallible(doc, "get");
    yyjson_mut_val *stack = REQUIRE_STACK(doc, 1);

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
    yyjson_mut_val *stack = REQUIRE_STACK(doc, 2);

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
    assign_scalar_to_target(doc, target, value, "set");

    /* Record replace at path with provided value */
    residual_group_add_patch_with_val(doc, group, "replace", path, value);

    if (group) residual_group_commit(doc, group);
}

/* json_append: Pops [value, path] (top is path) and appends value to array at path. */
void json_append(yyjson_mut_doc *doc) {
    yyjson_mut_val *root = get_root_fallible(doc, "append");
    yyjson_mut_val *stack = REQUIRE_STACK(doc, 2);

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
    yyjson_mut_val *stack = REQUIRE_STACK(doc, 1);
    
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
    yyjson_mut_val *stack = REQUIRE_STACK(doc, 1);
    
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
    
    assign_scalar_to_target(doc, target, val, "ptr_set");
    
    /* TODO: Residual logging for ptr_set (requires path, which is optional in jpm_ptr) */
}

/* print_json: Displays the current document contents; use at the end of execution or for user-facing inspection. */
void print_json(yyjson_mut_doc *doc) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!root) return;

    if (g_opt_raw && yyjson_mut_is_str(root)) {
        printf("%s\n", yyjson_get_str((yyjson_val *)root));
        return;
    }

    yyjson_write_flag flg = YYJSON_WRITE_NOFLAG;
    if (!g_opt_compact) {
        flg |= YYJSON_WRITE_PRETTY;
    }

    yyjson_write_err err;
    char *json_str = yyjson_mut_write_opts(doc, flg, NULL, NULL, &err);
    if (json_str) {
        printf("%s\n", json_str);
        free(json_str);
    }
}

static void undo_op_add(yyjson_mut_doc *doc, yyjson_mut_val *root, const char *path) {
    if (strcmp(path, "/stack/-") == 0) {
        yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
        if (stack && yyjson_mut_is_arr(stack) && yyjson_mut_arr_size(stack) > 0) {
            (void)yyjson_mut_arr_remove_last(stack);
        }
    }
}

static void undo_op_remove(yyjson_mut_doc *doc, yyjson_mut_val *root, const char *path, yyjson_mut_val *valv) {
    if (!valv) return;
    if (strncmp(path, "/stack/", 7) == 0) {
        yyjson_mut_val *stack = yyjson_mut_obj_get(root, "stack");
        if (stack && yyjson_mut_is_arr(stack)) {
            yyjson_mut_val *copy = jisp_mut_deep_copy(doc, valv);
            if (copy) yyjson_mut_arr_append(stack, copy);
        }
    }
}

/* undo_last_residual: Best-effort undo for the last residual entry.
   Now supports grouped entries where the last residual item is an array of patch objects;
   the group will be undone in reverse order. */
static void undo_one_patch(yyjson_mut_doc *doc, yyjson_mut_val *patch) {
    if (!doc || !patch) return;

    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!root) {
        jisp_fatal(doc, "undo: missing root");
    }

    if (!yyjson_mut_is_obj(patch)) {
        jisp_fatal(doc, "undo: residual entry is not an object");
    }

    yyjson_mut_val *opv = yyjson_mut_obj_get(patch, "op");
    yyjson_mut_val *pathv = yyjson_mut_obj_get(patch, "path");
    if (!opv || !pathv || !yyjson_mut_is_str(opv) || !yyjson_mut_is_str(pathv)) {
        jisp_fatal(doc, "undo: residual entry must have string 'op' and 'path'");
    }

    const char *op = yyjson_get_str((yyjson_val *)opv);
    const char *path = yyjson_get_str((yyjson_val *)pathv);

    if (strcmp(op, "add") == 0) {
        undo_op_add(doc, root, path);
    } else if (strcmp(op, "remove") == 0) {
        yyjson_mut_val *valv = yyjson_mut_obj_get(patch, "value");
        undo_op_remove(doc, root, path, valv);
    }
    /* replace is currently no-op in minimal mode */
}

static void perform_undo(yyjson_mut_doc *doc) {
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    if (!root) {
        jisp_fatal(doc, "undo: missing root");
    }
    yyjson_mut_val *residual = yyjson_mut_obj_get(root, "residual");
    if (!residual || !yyjson_mut_is_arr(residual) || yyjson_mut_arr_size(residual) == 0) {
        jisp_fatal(doc, "undo: 'residual' is missing or empty");
    }

    yyjson_mut_val *entry = yyjson_mut_arr_remove_last(residual);
    if (!entry) {
        jisp_fatal(doc, "undo: failed to pop residual entry");
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
                jisp_fatal(doc, "undo: grouped residual contains non-object entry");
            }
            undo_one_patch(doc, patch);
        }
        return;
    } else {
        jisp_fatal(doc, "undo: top residual entry must be an object or array of objects");
    }
}

void undo_jisp_op(yyjson_mut_doc *doc) {
    yyjson_mut_val *stack = REQUIRE_STACK(doc, 1);

    jisp_stack_log_remove_last(doc, stack);
    yyjson_mut_val *program = yyjson_mut_arr_remove_last(stack);

    if (!program || !yyjson_mut_is_obj(program)) {
        jisp_fatal(doc, "undo: top of stack must be a program object");
    }

    yyjson_mut_doc *sub_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *sub_root = jisp_mut_deep_copy(sub_doc, program);
    yyjson_mut_doc_set_root(sub_doc, sub_root);

    perform_undo(sub_doc);

    yyjson_mut_val *result = yyjson_mut_doc_get_root(sub_doc);
    yyjson_mut_val *result_copy = jisp_mut_deep_copy(doc, result);
    yyjson_mut_arr_append(stack, result_copy);
    record_patch_add_val(doc, "/stack/-", result_copy);

    yyjson_mut_doc_free(sub_doc);
}


/* Call Stack Helpers */

/* Ensure root["call_stack"] exists and is an array. */
static yyjson_mut_val *ensure_call_stack(yyjson_mut_doc *doc) {
    yyjson_mut_val *root = ensure_root_object(doc);
    yyjson_mut_val *cs = yyjson_mut_obj_get(root, "call_stack");
    if (cs && yyjson_mut_is_arr(cs)) return cs;
    
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "call_stack", arr);
    return arr;
}

static void push_call_stack(yyjson_mut_doc *doc, const char *path) {
    yyjson_mut_val *cs = ensure_call_stack(doc);
    yyjson_mut_arr_add_strcpy(doc, cs, path ? path : "<unknown>");
}

static void pop_call_stack(yyjson_mut_doc *doc) {
    yyjson_mut_val *cs = ensure_call_stack(doc);
    /* Best effort: remove the last frame if present. */
    if (yyjson_mut_arr_size(cs) > 0) {
        (void)yyjson_mut_arr_remove_last(cs);
    }
}

/* Interrupt handling for exit */
static void set_exit_interrupt(yyjson_mut_doc *doc) {
    yyjson_mut_val *root = ensure_root_object(doc);
    yyjson_mut_obj_add_bool(doc, root, "_interrupt_exit", true);
}

static bool check_and_clear_exit_interrupt(yyjson_mut_doc *doc) {
    yyjson_mut_val *root = ensure_root_object(doc);
    yyjson_mut_val *flag = yyjson_mut_obj_get(root, "_interrupt_exit");
    if (flag && yyjson_get_bool((yyjson_val*)flag)) {
        // Clear it so we only exit one level
        yyjson_mut_obj_remove_key(root, "_interrupt_exit"); 
        return true;
    }
    return false;
}

/* enter: Pops target from stack. If string path, executes array at path. If array, executes it in-place. */
void enter(yyjson_mut_doc *doc) {
    yyjson_mut_val *stack = REQUIRE_STACK(doc, 1);
    
    /* Pop the top argument and dispatch by type. */
    yyjson_mut_val *top = yyjson_mut_arr_remove_last(stack); // Pop it!
    
    if (yyjson_mut_is_str(top)) {
        const char *path = yyjson_get_str((yyjson_val*)top);
        // Resolve path
        yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
        yyjson_mut_val *target = NULL;
        if (strcmp(path, "/") == 0) target = root;
        else target = yyjson_mut_ptr_get(root, path);
        
        if (!target || !yyjson_mut_is_arr(target)) {
            jisp_fatal(doc, "enter: path '%s' does not resolve to an array", path);
        }
        
        // Execute
        process_ep_array(doc, target, path);
        
    } else if (yyjson_mut_is_arr(top)) {
        // Anonymous array execution
        // We popped it.
        process_ep_array(doc, top, "<anonymous>");
        
    } else {
        jisp_fatal(doc, "enter: top of stack must be a path string or an array");
    }
}

/* exit: signals the current loop to break */
void op_exit(yyjson_mut_doc *doc) {
    set_exit_interrupt(doc);
}

static bool json_subset_equals(yyjson_mut_val *subset, yyjson_mut_val *superset) {
    if (!subset || !superset) return subset == superset;
    
    if (unsafe_yyjson_get_type(subset) != unsafe_yyjson_get_type(superset)) {
        // If types mismatch, check if one is generic number and other is int/real?
        // yyjson handles num type internally.
        // unsafe_yyjson_equals handles type check.
        return false; 
    }
    
    if (yyjson_mut_is_obj(subset)) {
        if (!yyjson_mut_is_obj(superset)) return false;
        
        yyjson_mut_obj_iter it;
        yyjson_mut_val *key, *val;
        yyjson_mut_obj_iter_init(subset, &it);
        while ((key = yyjson_mut_obj_iter_next(&it))) {
            val = yyjson_mut_obj_iter_get_val(key);
            const char *kstr = unsafe_yyjson_get_str(key);
            
            yyjson_mut_val *super_val = yyjson_mut_obj_get(superset, kstr);
            if (!super_val) return false;
            
            if (!json_subset_equals(val, super_val)) return false;
        }
        return true;
    } else {
        // Strict equality for other types (Array, Scalar)
        return yyjson_mut_equals(subset, superset);
    }
}

static yyjson_mut_val *jisp_create_error(yyjson_mut_doc *doc, const char *kind, const char *msg) {
    yyjson_mut_val *err = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, err, "error", true);
    yyjson_mut_obj_add_strcpy(doc, err, "kind", kind);
    yyjson_mut_obj_add_strcpy(doc, err, "message", msg);
    return err;
}

static void print_jisp_error_pretty(yyjson_mut_val *val) {
    if (!val || !yyjson_mut_is_obj(val)) {
        printf("Invalid Error Object\n");
        return;
    }
    
    yyjson_mut_val *kind = yyjson_mut_obj_get(val, "kind");
    yyjson_mut_val *msg = yyjson_mut_obj_get(val, "message");
    yyjson_mut_val *details = yyjson_mut_obj_get(val, "details");
    
    const char *kind_str = (kind && yyjson_mut_is_str(kind)) ? yyjson_get_str((yyjson_val*)kind) : "Unknown Error";
    const char *msg_str = (msg && yyjson_mut_is_str(msg)) ? yyjson_get_str((yyjson_val*)msg) : "";
    
    printf("\n-- %s --\n", kind_str);
    if (msg_str[0]) {
        printf("%s\n", msg_str);
    }
    
    if (details && yyjson_mut_is_obj(details)) {
        yyjson_mut_val *expected = yyjson_mut_obj_get(details, "expected");
        yyjson_mut_val *actual = yyjson_mut_obj_get(details, "actual");
        
        if (expected || actual) {
            if (expected) {
                printf("Expected:\n");
                char *json = yyjson_mut_val_write_opts(expected, YYJSON_WRITE_PRETTY | YYJSON_WRITE_INF_AND_NAN_AS_NULL, NULL, NULL, NULL);
                if (json) { printf("%s\n", json); free(json); }
            }
            if (actual) {
                printf("Actual:\n");
                char *json = yyjson_mut_val_write_opts(actual, YYJSON_WRITE_PRETTY | YYJSON_WRITE_INF_AND_NAN_AS_NULL, NULL, NULL, NULL);
                if (json) { printf("%s\n", json); free(json); }
            }
        } else {
             // Fallback for other details
             printf("Details\n");
             char *json = yyjson_mut_val_write_opts(details, YYJSON_WRITE_PRETTY | YYJSON_WRITE_INF_AND_NAN_AS_NULL, NULL, NULL, NULL);
             if (json) { printf("%s\n", json); free(json); }
        }
    }
}

void op_print_error(yyjson_mut_doc *doc) {
    yyjson_mut_val *stack = REQUIRE_STACK(doc, 1);
    
    jisp_stack_log_remove_last(doc, stack);
    yyjson_mut_val *val = yyjson_mut_arr_remove_last(stack);
    
    print_jisp_error_pretty(val);
}

void op_load(yyjson_mut_doc *doc) {
    yyjson_mut_val *stack = REQUIRE_STACK(doc, 1);
    
    jisp_stack_log_remove_last(doc, stack);
    yyjson_mut_val *path_val = yyjson_mut_arr_remove_last(stack);
    
    if (!yyjson_mut_is_str(path_val)) {
        jisp_fatal(doc, "load: path must be a string");
    }
    const char *path = yyjson_get_str((yyjson_val *)path_val);
    
    yyjson_read_err err;
    yyjson_doc *loaded = yyjson_read_file(path, YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS, NULL, &err);
    if (!loaded) {
        /* Fail fast on read errors for clear diagnostics. */
        jisp_fatal(doc, "load: failed to read file '%s': %s at pos %zu", path, err.msg, err.pos);
    }
    
    yyjson_val *root = yyjson_doc_get_root(loaded);
    yyjson_mut_val *copy = yyjson_val_mut_copy(doc, root);
    
    yyjson_doc_free(loaded);
    
    if (!copy) {
         jisp_fatal(doc, "load: failed to copy loaded JSON");
    }
    
    yyjson_mut_arr_append(stack, copy);
    record_patch_add_val(doc, "/stack/-", copy);
}

void op_store(yyjson_mut_doc *doc) {
    yyjson_mut_val *stack = REQUIRE_STACK(doc, 2);
    
    jisp_stack_log_remove_last(doc, stack);
    yyjson_mut_val *path_val = yyjson_mut_arr_remove_last(stack);
    if (!yyjson_mut_is_str(path_val)) {
        jisp_fatal(doc, "store: path must be a string");
    }
    const char *path = yyjson_get_str((yyjson_val *)path_val);
    
    jisp_stack_log_remove_last(doc, stack);
    yyjson_mut_val *val = yyjson_mut_arr_remove_last(stack);
    
    yyjson_write_err err;
    bool ok = yyjson_mut_write_file(path, doc, YYJSON_WRITE_PRETTY, NULL, &err);
    /* Legacy: write entire document, then write the selected value for compatibility. */
    ok = yyjson_mut_val_write_file(path, val, YYJSON_WRITE_PRETTY, NULL, &err);
    
    if (!ok) {
        jisp_fatal(doc, "store: failed to write file '%s': %s", path, err.msg);
    }
}

void op_test(yyjson_mut_doc *doc) {
    yyjson_mut_val *stack = REQUIRE_STACK(doc, 2);
    
    jisp_stack_log_remove_last(doc, stack);
    yyjson_mut_val *expected = yyjson_mut_arr_remove_last(stack);
    
    jisp_stack_log_remove_last(doc, stack);
    yyjson_mut_val *program = yyjson_mut_arr_remove_last(stack);
    
    if (!expected || !program) {
        jisp_fatal(doc, "test: null arguments");
    }
    
    // Create isolated sub-document
    yyjson_mut_doc *sub_doc = yyjson_mut_doc_new(NULL);
    
    // Deep copy program to sub_doc root
    // Note: program is in 'doc'. We need to copy to 'sub_doc'.
    // jisp_mut_deep_copy takes a doc and a val. It copies val (from whatever doc) INTO 'doc'.
    // So:
    yyjson_mut_val *sub_root = jisp_mut_deep_copy(sub_doc, program);
    yyjson_mut_doc_set_root(sub_doc, sub_root);
    
    jpm_doc_retain(sub_doc);
    
    // Run it
    process_entrypoint(sub_doc);
    
    // Check result
    // sub_doc root is the result.
    yyjson_mut_val *result = yyjson_mut_doc_get_root(sub_doc);
    
    bool ok = json_subset_equals(expected, result);
    
    if (!ok) {
        // Create structured error object
        yyjson_mut_val *error_obj = jisp_create_error(doc, "test_failure", "Test failed: result mismatch");
        yyjson_mut_val *details = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, error_obj, "details", details);
        
        // Add expected and actual to details
        yyjson_mut_obj_add_val(doc, details, "expected", jisp_mut_deep_copy(doc, expected));
        yyjson_mut_obj_add_val(doc, details, "actual", jisp_mut_deep_copy(doc, result));

        yyjson_mut_arr_append(stack, error_obj);
        record_patch_add_val(doc, "/stack/-", yyjson_mut_arr_get_last(stack));
    }
    
    jpm_doc_release(sub_doc);
}

static void process_ep_object(yyjson_mut_doc *doc, yyjson_mut_val *stack, yyjson_mut_val *elem, const char *path_prefix, size_t idx) {
    yyjson_mut_val *dot = yyjson_mut_obj_get(elem, ".");
    if (!dot) {
        yyjson_mut_arr_append(stack, jisp_mut_deep_copy(doc, elem));
        return;
    }
		
    if (yyjson_mut_is_arr(dot)) {
        char nested_path[1024];
        snprintf(nested_path, sizeof(nested_path), "%s/%zu/.", path_prefix, idx);
        process_ep_array(doc, dot, nested_path);
        return;
    } 
    
    if (yyjson_mut_is_str(dot)) {
        const char *name = yyjson_get_str((yyjson_val *)dot);
        yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
        yyjson_mut_val *target_array = yyjson_mut_obj_get(root, name);

        if (target_array && yyjson_mut_is_arr(target_array)) {
            char target_path[512];
            snprintf(target_path, sizeof(target_path), "/%s", name);
            process_ep_array(doc, target_array, target_path);
        } else {
            jisp_op op = jisp_op_registry_get(name);
            if (op) {
                op(doc);
            } else {
                /* Unknown op name; treat the object as a literal */
                yyjson_mut_arr_append(stack, jisp_mut_deep_copy(doc, elem));
            }
        }
        return;
    }
    
    jisp_fatal(doc, "entrypoint object '.' field must be an array or string");
}

static void process_one_instruction(yyjson_mut_doc *doc, yyjson_mut_val *stack, yyjson_mut_val *elem, const char *path_prefix, size_t idx) {
    if (yyjson_mut_is_obj(elem)) {
        process_ep_object(doc, stack, elem, path_prefix, idx);
    } else if (yyjson_mut_is_str(elem) || yyjson_is_num((yyjson_val *)elem) || yyjson_mut_is_arr(elem)) {
        jisp_stack_push_copy_and_log(doc, stack, elem);
    } else {
        jisp_fatal(doc, "entrypoint element is not a string, number, array, or object");
    }
}

/* process_ep_array: Interprets an entrypoint-like array of literals and directives; use for root entrypoint and nested '.' arrays. */
static void process_ep_array(yyjson_mut_doc *doc, yyjson_mut_val *ep, const char *path_prefix) {
    if (!doc || !ep) return;
    if (!yyjson_mut_is_arr(ep)) {
        jisp_fatal(doc, "entrypoint must be an array");
    }

    push_call_stack(doc, path_prefix);
    
    yyjson_mut_val *stack = get_stack_fallible(doc, "process_entrypoint");

    yyjson_mut_arr_iter it;
    yyjson_mut_val *elem;
    if (!yyjson_mut_arr_iter_init(ep, &it)) {
        pop_call_stack(doc);        
        return;
    }

    size_t idx = 0;
    while ((elem = yyjson_mut_arr_iter_next(&it))) {
        
        if (check_and_clear_exit_interrupt(doc)) {
            break;
        }
        
        process_one_instruction(doc, stack, elem, path_prefix, idx);
        idx++;
    }
    
    pop_call_stack(doc);
}

void step_jisp_op(yyjson_mut_doc *doc) {
    yyjson_mut_val *stack = REQUIRE_STACK(doc, 1);

    jisp_stack_log_remove_last(doc, stack);
    yyjson_mut_val *program = yyjson_mut_arr_remove_last(stack);

    if (!program || !yyjson_mut_is_obj(program)) {
        jisp_fatal(doc, "step: top of stack must be a program object");
    }

    // Create isolated sub-document
    yyjson_mut_doc *sub_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *sub_root = jisp_mut_deep_copy(sub_doc, program);
    yyjson_mut_doc_set_root(sub_doc, sub_root);
    jpm_doc_retain(sub_doc);

    // Get PC
    yyjson_mut_val *pc_val = yyjson_mut_obj_get(sub_root, "pc");
    int64_t pc = 0;
    if (pc_val && yyjson_mut_is_int(pc_val)) {
        pc = yyjson_mut_get_sint(pc_val);
    } else {
        yyjson_mut_obj_add_int(sub_doc, sub_root, "pc", 0);
    }

    // Get entrypoint
    yyjson_mut_val *entrypoint = yyjson_mut_obj_get(sub_root, "entrypoint");
    if (!entrypoint || !yyjson_mut_is_arr(entrypoint)) {
        yyjson_mut_val *result = yyjson_mut_doc_get_root(sub_doc);
        yyjson_mut_val *result_copy = jisp_mut_deep_copy(doc, result);
        yyjson_mut_arr_append(stack, result_copy);
        record_patch_add_val(doc, "/stack/-", result_copy);
        jpm_doc_release(sub_doc);
        return;
    }

    size_t ep_size = yyjson_mut_arr_size(entrypoint);
    if (pc >= 0 && (size_t)pc < ep_size) {
        yyjson_mut_val *instruction = yyjson_mut_arr_get(entrypoint, (size_t)pc);
        
        yyjson_mut_val *sub_stack = get_stack_fallible(sub_doc, "step");
        process_one_instruction(sub_doc, sub_stack, instruction, "/entrypoint", (size_t)pc);

        yyjson_mut_obj_add_val(sub_doc, sub_root, "pc", yyjson_mut_int(sub_doc, pc + 1));
    }
    
    yyjson_mut_val *result = yyjson_mut_doc_get_root(sub_doc);
    yyjson_mut_val *result_copy = jisp_mut_deep_copy(doc, result);
    yyjson_mut_arr_append(stack, result_copy);
    record_patch_add_val(doc, "/stack/-", result_copy);

    jpm_doc_release(sub_doc);
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

static void jisp_process_stream(FILE *fp, const char *filename) {
    size_t buf_cap = 65536; // Start with 64KB
    char *buf = (char *)malloc(buf_cap);
    if (!buf) jisp_fatal(NULL, "Out of memory allocating stream buffer");
    
    size_t buf_len = 0; // Bytes currently in buffer
    
    while (true) {
        // Refill buffer
        size_t to_read = buf_cap - buf_len;
        size_t n = fread(buf + buf_len, 1, to_read, fp);
        buf_len += n;
        
        if (n == 0 && buf_len == 0) {
             break; // EOF and empty
        }
        
        // Process buffer
        size_t offset = 0;
        while (offset < buf_len) {
            // Skip leading whitespace manually to track consumption
            while (offset < buf_len && isspace((unsigned char)buf[offset])) {
                offset++;
            }
            
            if (offset == buf_len) {
                 // Only trailing whitespace in this chunk
                 if (n == 0 && feof(fp)) {
                     goto cleanup; // EOF and only whitespace left -> Done
                 }
                 break; // Need more data
            }
            
            yyjson_read_err err;
            // Attempt to parse one JSON value
            yyjson_doc *in = yyjson_read_opts(buf + offset, 
                                              buf_len - offset, 
                                              YYJSON_READ_STOP_WHEN_DONE | YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS, 
                                              NULL, 
                                              &err);
                                              
            if (in) {
                 // Success
                 size_t read_size = yyjson_doc_get_read_size(in);
                 
                 // Convert to mutable doc and execute
                 yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
                 yyjson_mut_val *root = yyjson_val_mut_copy(doc, yyjson_doc_get_root(in));
                 yyjson_mut_doc_set_root(doc, root);
                 jpm_doc_retain(doc);
                 
                 yyjson_doc_free(in);
                 
                 // Update context for error reporting during execution
                 g_jisp_ctx.filename = filename;
                 g_jisp_ctx.src = buf + offset; 
                 g_jisp_ctx.src_len = read_size;
                 
                 process_entrypoint(doc);
                 jpm_doc_release(doc);
                 
                 offset += read_size;
            } else {
                 // Parse error
                 if (err.code == YYJSON_READ_ERROR_UNEXPECTED_END) {
                     // Incomplete JSON, break to read more
                     break;
                 } else {
                     // Fatal parse error
                     jisp_fatal_parse(NULL, filename, buf + offset, buf_len - offset, err.pos, "Parse error: %s", err.msg);
                 }
            }
        }
        
        // Move remaining data to front
        if (offset > 0) {
            size_t remaining = buf_len - offset;
            if (remaining > 0) {
                memmove(buf, buf + offset, remaining);
            }
            buf_len = remaining;
        }
        
        // Handle buffer full or EOF conditions
        if (buf_len == buf_cap) {
            if (n == 0) {
                 // EOF, buffer full, parse failed (UNEXPECTED_END)
                 yyjson_read_err err;
                 yyjson_read_opts(buf, buf_len, YYJSON_READ_STOP_WHEN_DONE | YYJSON_READ_ALLOW_COMMENTS, NULL, &err);
                 jisp_fatal_parse(NULL, filename, buf, buf_len, err.pos, "Unexpected end of stream: %s", err.msg);
            }
            
            // Expand buffer
            size_t new_cap = buf_cap * 2;
            char *new_buf = (char *)realloc(buf, new_cap);
            if (!new_buf) {
                free(buf);
                jisp_fatal(NULL, "Out of memory expanding stream buffer");
            }
            buf = new_buf;
            buf_cap = new_cap;
        } else if (n == 0 && buf_len > 0) {
             // EOF, data remains, incomplete JSON
             yyjson_read_err err;
             yyjson_read_opts(buf, buf_len, YYJSON_READ_STOP_WHEN_DONE | YYJSON_READ_ALLOW_COMMENTS, NULL, &err);
             jisp_fatal_parse(NULL, filename, buf, buf_len, err.pos, "Invalid data at end of stream: %s", err.msg);
        }
    }

cleanup:
    free(buf);
}

static void jisp_process_whole_file(const char *filename) {
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
    jpm_doc_release(doc);
    free(buf);
}

/* main: Orchestrates program flow from input file to execution and output; run as the CLI entry point. */
int main(int argc, char **argv) {
    /* Initialize global JISP op registry (JSON doc) */
    jisp_op_registry_init();

    int arg_idx = 1;
    const char *filename = NULL;

    while (arg_idx < argc) {
        const char *arg = argv[arg_idx];
        if (arg[0] == '-' && arg[1] != '\0') {
             // It's a flag or stdin "-"
             if (strcmp(arg, "-") == 0) {
                 filename = "-"; // explicit stdin
             } else {
                 // Parse flags like -rc, -c, -r
                 for (int j = 1; arg[j]; j++) {
                     if (arg[j] == 'r') g_opt_raw = true;
                     else if (arg[j] == 'c') g_opt_compact = true;
                     else {
                         fprintf(stderr, "Unknown option: -%c\n", arg[j]);
                         return 1;
                     }
                 }
             }
        } else {
            filename = arg;
        }
        arg_idx++;
    }

    if (!filename) {
        jisp_process_stream(stdin, "stdin");
    } else if (strcmp(filename, "-") == 0) {
        jisp_process_stream(stdin, "stdin");
    } else {
        jisp_process_whole_file(filename);
    }
    
    ptr_stack_free_all();
    jisp_op_registry_free();
    return 0;
}
