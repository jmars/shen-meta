/*
 * zincvm.c — ZINC bytecode parser and virtual machine
 *
 * Parses the canonical s-expression bytecode format produced by
 * compile.shen (nat->csexp) and executes it with a register/stack
 * machine matching the Shen ZINC interpreter semantics.
 *
 * Grammar:
 *   csexp-list   ::= "(" elem* ")"
 *   elem         ::= opcode | csexp-atom | csexp-list
 *   csexp-atom   ::= "[" len ":" type "]" value
 *   len          ::= [0-9]+    (decimal, number of bytes in value)
 *   type         ::= "s" | "n" | "S" | "b"
 *
 * Opcodes (single characters):
 *   m  pushmark    p  apply         u  push
 *   r  grab        v  return        e  let
 *   d  endlet      t  appterm
 *   a  access      g  global        f  jmpf
 *   j  jmp         c  cur           n  number
 *   S  string      s  symbol        b  boolean
 *   P  prim
 *
 * Compile: gcc -Wall -Wextra -o zincvm zincvm.c
 * Test:    ./zincvm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>

#include <stdint.h>
#include "gc.h"

/* Cheney GC: mostly-copying semi-space collector.  gc_alloc returns
 * zeroed memory.  Call gc_init() once before any allocation, and
 * gc_set_extra_roots() for global data that the collector must trace. */
/* GC_VALUE, GC_STR, GC_VALUE_ARRAY, and value types are in zincvm.h */

/* ------------------------------------------------------------------ */
/*  Value types (shared with gc.c via zinctypes.h)                     */
/* ------------------------------------------------------------------ */

#include "zincvm.h"

/* ---- GC scanning functions (called by gc.c collect() scavenger) ---- */

/* gc_move is implemented in gc.c */
void *gc_move(void *p);

/* gc_evacuate: update a single pointer slot to point to the evacuated copy */
void gc_evacuate(void **slot) {
    *slot = gc_move(*slot);
}

/* gc_scan_value: evacuate all GC-managed pointers within a Value */
void gc_scan_value(Value *v) {
    switch (v->tag) {
    case VAL_CONS:
        gc_evacuate((void **)&v->cons.car);
        gc_evacuate((void **)&v->cons.cdr);
        break;
    case VAL_LAMBDA:
        gc_evacuate((void **)&v->lambda.code);
        gc_evacuate((void **)&v->lambda.env);
        break;
    case VAL_VECTOR:
        gc_evacuate((void **)&v->vector.data);
        break;
    case VAL_STRING:
        gc_evacuate((void **)&v->str.data);
        break;
    case VAL_ERROR:
        gc_evacuate((void **)&v->error.message);
        break;
    /* These types contain no GC-managed pointers:
     *   VAL_NUMBER, VAL_SYMBOL (sym.name is strdup'd C-heap),
     *   VAL_BOOLEAN, VAL_NIL, VAL_MARK,
     *   VAL_PRIM (prim.name is a literal string),
     *   VAL_STREAM (stream.file is FILE* / intptr_t)
     */
    default:
        break;
    }
}

/* True iff v references any GC object in the nursery.  Must mirror
 * exactly the pointer fields gc_scan_value evacuates. */
static int value_references_nursery(Value *v) {
    switch (v->tag) {
    case VAL_CONS:    return gc_in_nursery(v->cons.car) || gc_in_nursery(v->cons.cdr);
    case VAL_LAMBDA:  return gc_in_nursery(v->lambda.code) || gc_in_nursery(v->lambda.env);
    case VAL_VECTOR:  return v->vector.data && gc_in_nursery(v->vector.data);
    case VAL_STRING:  return v->str.data && gc_in_nursery(v->str.data);
    case VAL_ERROR:   return v->error.message && gc_in_nursery(v->error.message);
    default:          return 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Parser state                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    const char *start;
} ParseState;

static jmp_buf parse_err_jmp;
static char parse_err_msg[256];

#define PARSE_ERROR(msg) do { \
    snprintf(parse_err_msg, sizeof(parse_err_msg), \
             "parse error at offset %ld: %s", \
             (long)(ps->p - ps->start), (msg)); \
    longjmp(parse_err_jmp, 1); \
} while (0)

/* ------------------------------------------------------------------ */
/*  Value helpers                                                      */
/* ------------------------------------------------------------------ */

Value val_number(long n) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_NUMBER; v.number = n; return v;
}
Value val_string(const char *data, int len) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_STRING;
    v.str.data = GC_STR(len);
    memcpy(v.str.data, data, len);
    v.str.data[len] = '\0';
    v.str.len = len; return v;
}
Value val_string_from(Value *src_slot, int off, int len) {
    /* Pins src_slot so its str.data survives the GC_STR alloc.
       For string/error primitives that read a popped Value's interior
       pointer across a gc_alloc_atomic call. */
    gc_root_push_value(src_slot);
    char *dst = (char*)gc_alloc_atomic(len + 1);
    memcpy(dst, src_slot->str.data + off, len);
    dst[len] = '\0';
    gc_root_pop();
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_STRING; v.str.data = dst; v.str.len = len;
    return v;
}
Value val_symbol(const char *name) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_SYMBOL; v.sym.name = strdup(name); return v;
}
Value val_boolean(int b) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_BOOLEAN; v.boolean = b; return v;
}
Value val_cons(Value car, Value cdr) {
    /* Pin car and cdr so their interior pointers survive the two gc_alloc
     * calls below.  The existing volatile car_root mechanism is still needed
     * for the conservative scan to find car_cell across the second alloc;
     * the precise-root pins are additive and cover the by-value args. */
    gc_root_push_value(&car);
    gc_root_push_value(&cdr);
    Value *car_cell = (Value*)gc_alloc(sizeof(Value), GC_TYPE_VALUE);
    Value *volatile car_root = car_cell;
    Value *cdr_cell = (Value*)gc_alloc(sizeof(Value), GC_TYPE_VALUE);
    *car_root = car;
    *cdr_cell = cdr;
    gc_root_pop();
    gc_root_pop();
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_CONS; v.cons.car = car_root; v.cons.cdr = cdr_cell;
    return v;
}
Value val_nil(void) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_NIL; return v;
}
Value val_lambda(Instr *code, int code_len, Value *env, int env_len) {
    /* env arrays are GC-allocated via gcalloc so GC traces captured
       Values when the closure is reachable (e.g. via global_table). */
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_LAMBDA;
    v.lambda.code = code; v.lambda.code_len = code_len;
    if (env_len > 0) {
        v.lambda.env = GC_VALUE_ARRAY(env_len);
        memcpy(v.lambda.env, env, env_len * sizeof(Value));
        v.lambda.env_len = env_len;
    } else { v.lambda.env = NULL; v.lambda.env_len = 0; }
    return v;
}
#define check_closure(cl, where) ((void)0)
/* verify_heap() is now in zincvm.h */
static Value val_mark(void) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_MARK; return v;
}
static Value val_prim(const char *name) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_PRIM; v.prim.name = name; return v;
}
static Value val_error(const char *msg) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_ERROR;
    /* GC-allocate so the message is reclaimed with the collector instead of
       leaking via strdup on every raised error. */
    int len = (int)strlen(msg);
    char *buf = (char*)gc_alloc_atomic(len + 1);
    memcpy(buf, msg, len); buf[len] = '\0';
    v.error.message = buf;
    return v;
}
Value val_vector(int size) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_VECTOR; v.vector.len = size;
    if (size > 0) v.vector.data = (Value*)gc_alloc(size * sizeof(Value), GC_TYPE_VALUE_ARRAY);
    return v;
}
static Value val_stream_in(FILE *f) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_STREAM; v.stream.file = f; v.stream.is_input = 1; return v;
}
static Value val_stream_out(FILE *f) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_STREAM; v.stream.file = f; v.stream.is_input = 0; return v;
}

/* String stream storage — avoids bloating sizeof(Value).
   Index stored in stream.file cast to (FILE*)(intptr_t)idx. */
#define MAX_STRING_STREAMS 8
static struct { char *data; int len; int pos; } string_streams[MAX_STRING_STREAMS];
static int n_string_streams = 0;

static Value val_string_stream_in(const char *src, int srclen) {
    if (n_string_streams >= MAX_STRING_STREAMS) {
        fprintf(stderr, "runtime: too many string streams\n");
        return val_error("too many string streams");
    }
    int idx = n_string_streams++;
    string_streams[idx].data = malloc(srclen + 1);
    memcpy(string_streams[idx].data, src, srclen);
    string_streams[idx].data[srclen] = '\0';
    string_streams[idx].len = srclen;
    string_streams[idx].pos = 0;
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_STREAM;
    v.stream.file = (FILE*)(intptr_t)(idx + 1);  /* +1 so 0 = no string stream */
    v.stream.is_input = 1;
    v.stream.is_string = 1;
    return v;
}

void print_value(Value v) {
    switch (v.tag) {
    case VAL_NUMBER: printf("%ld", v.number); break;
    case VAL_STRING: printf("\"%.*s\"", v.str.len, v.str.data); break;
    case VAL_SYMBOL: printf("%s", v.sym.name); break;
    case VAL_BOOLEAN: printf(v.boolean ? "true" : "false"); break;
    case VAL_CONS: printf("[cons "); print_value(*v.cons.car);
                   printf(" . "); print_value(*v.cons.cdr);
                   printf("]"); break;
    case VAL_NIL: printf("[]"); break;
    case VAL_LAMBDA:
        printf("[lambda %p %d env=%p %d]",
               (void *)v.lambda.code, v.lambda.code_len,
               (void *)v.lambda.env, v.lambda.env_len); break;
    case VAL_MARK: printf("mark"); break;
    case VAL_PRIM: printf("[prim %s]", v.prim.name); break;
    case VAL_ERROR: printf("[error \"%s\"]", v.error.message); break;
    case VAL_VECTOR: printf("[vector %d]", v.vector.len); break;
    case VAL_STREAM: printf("[stream %s]", v.stream.is_input ? "in" : "out"); break;
    default: printf("?%d?", v.tag);
    }
}

/* ------------------------------------------------------------------ */
/*  Value stack                                                        */
/* ------------------------------------------------------------------ */

#define STACK_INIT_CAP 64

static void va_init(ValueArray *a) {
    a->data = GC_VALUE_ARRAY(STACK_INIT_CAP);
    a->len = 0; a->cap = STACK_INIT_CAP;
}
static void va_push(ValueArray *a, Value v) {
    if (a->len >= a->cap) {
        int new_cap = a->cap * 2;
        Value *new_data = GC_VALUE_ARRAY(new_cap);
        memcpy(new_data, a->data, a->len * sizeof(Value));
        a->data = new_data; a->cap = new_cap;
    }
    a->data[a->len++] = v;
}
static Value va_pop(ValueArray *a) {
    if (a->len <= 0) { fprintf(stderr, "fatal: pop from empty stack\n"); exit(1); }
    return a->data[--a->len];
}
static Value va_peek(ValueArray *a) { return a->data[a->len - 1]; }
static void va_free(ValueArray *a) { a->data = NULL; a->len = a->cap = 0; }

/* ------------------------------------------------------------------ */
/*  Closure tracing (--trace <name>)                                   */
/* ------------------------------------------------------------------ */

/* MAX_TRACED is in zincvm.h */
Instr  *traced_code[MAX_TRACED];
static const char *traced_name[MAX_TRACED];
int   num_traced = 0;

/* Add a function name to the trace list.  The code pointer is resolved
   after parse_bundle (when closures are in the global table). */
void trace_add(const char *name) {
    if (num_traced < MAX_TRACED) {
        traced_name[num_traced++] = name;
    }
}

/* ------------------------------------------------------------------ */
/*  Global table                                                       */
/* ------------------------------------------------------------------ */

/* GLOBAL_TABLE_MAX and GlobalEntry are in zincvm.h */

GlobalEntry global_table[GLOBAL_TABLE_MAX];
int global_table_len = 0;

/* GC-visible pointer to the active value stack.  A conservative C-stack
   scan alone is unreliable (register allocation can hide pointers).  By
   keeping the stack data pointer here (registered as an extra root), the
   GC always traces Values on the VM stack. */

void global_set(const char *name, Value v) {
    for (int i = 0; i < global_table_len; i++) {
        if (strcmp(global_table[i].name, name) == 0) {
            global_table[i].closure = v; return;
        }
    }
    if (global_table_len < GLOBAL_TABLE_MAX) {
        global_table[global_table_len].name = strdup(name);
        global_table[global_table_len].closure = v;
        global_table_len++;
    }
}
static int exec_primitive_valid(const char *name);
Value global_get(const char *name) {
    for (int i = 0; i < global_table_len; i++)
        if (strcmp(global_table[i].name, name) == 0)
            return global_table[i].closure;
    /* Only return VAL_PRIM for known C primitives. Unknown names
       (e.g., *macros*, *stinput*) must be VAL_SYMBOL so that
       cons?, element?, and other list-traversal code can match them.
       The = primitive already handles SYMBOL-vs-PRIM comparison in
       both directions, so this doesn't break fail comparisons. */
    if (exec_primitive_valid(name))
        return val_prim(name);
    return val_symbol(name);
}

/* ------------------------------------------------------------------ */
/*  Error handling for trap-error / simple-error                       */
/* ------------------------------------------------------------------ */

/* CatchFrame is in zincvm.h */

CatchFrame *vm_catch_chain = NULL;

static void vm_throw(const char *msg) {
    if (!vm_catch_chain) {
        fprintf(stderr, "uncaught Shen error: %s\n", msg);
        abort();
    }
    vm_catch_chain->error_val = val_error(msg);
    longjmp(vm_catch_chain->buf, 1);
}

/* Type-error in a primitive.  Primary ownership is the Shen safe-wrapper layer
   (shen/primitives.shen): those wrappers validate args and raise a catchable
   simple-error before the raw primitive is ever called.  This C-level routing is
   therefore only DEFENSE-IN-DEPTH, enabled solely in debug builds (ZINCVM_DEBUG)
   so that raw/%%-style direct calls into a primitive are still catchable while
   developing.  In release builds the wrapper is the contract: a primitive that
   reaches here prints and returns -1 (a hard, non-catchable VM error), which is
   fine because the wrapper never forwards bad input. */
#ifdef ZINCVM_DEBUG
#define PRIM_TYPE_ERROR(msg) \
    do { \
        if (vm_catch_chain && vm_catch_chain->in_trap_error) \
            vm_throw(msg); \
        fprintf(stderr, "runtime: %s\n", msg); \
        return -1; \
    } while (0)
#else
/* Release: the Shen safe-wrapper layer is the sole owner of argument
   validation, and static call sites from the proven type-safe interpreter
   never pass bad types.  Expanding to a bare ((void)0) makes GCC -O2
   eliminate the enclosing `if (cond) PRIM_TYPE_ERROR(...)` entirely — no
   comparison, no type check, no runtime cost.  Only the always-on throw
   sites (simple-error, fail, apply/appterm non-callable, env_pop, eval-kl)
   remain, since they are not primitive type guards. */
#define PRIM_TYPE_ERROR(msg) ((void)0)
#endif

/* alarm_jmp and test_timed_out moved to zinctest.c (test binary) */

static int repl_mode = 0;
static jmp_buf repl_exit_jmp;

/* (vm_exec / vm_exec_env now declared in zincvm.h) */

/* ------------------------------------------------------------------ */
/*  Marshal layer: convert C Value ↔ Shen tagged representation        */
/* ------------------------------------------------------------------ */

/* marshal_to_tagged: C Value → Shen tagged form.
   Tagged forms (from interp.shen extract-kl):
     [number X]  = cons(symbol("number"), cons(X, nil))
     [symbol X]  = cons(symbol("symbol"), cons(X, nil))
     [string X]  = cons(symbol("string"), cons(X, nil))
     [boolean X] = cons(symbol("boolean"), cons(X, nil))
     [cons X Y]  = cons(symbol("cons"), cons(X', cons(Y', nil)))
     [cons]      = cons(symbol("cons"), nil)   — empty list
     mark        = symbol("mark")
   Unmarshallable types (lambdas, prims, errors, vectors, streams)
   pass through unchanged. */
static Value marshal_to_tagged(Value v) {
    switch (v.tag) {
    case VAL_NUMBER:
        return val_cons(val_symbol("number"), val_cons(v, val_nil()));
    case VAL_SYMBOL:
        return val_cons(val_symbol("symbol"), val_cons(v, val_nil()));
    case VAL_STRING:
        return val_cons(val_symbol("string"), val_cons(v, val_nil()));
    case VAL_BOOLEAN:
        return val_cons(val_symbol("boolean"), val_cons(v, val_nil()));
    case VAL_CONS: {
        /* Don't recursively marshal car/cdr — extract-kl handles its own
           recursion on [cons X Y] by calling extract-kl on X and Y directly.
           Recursive marshalling creates deeply nested structures that the
           compiled interp patterns can't match. */
        return val_cons(val_symbol("cons"),
                        val_cons(*v.cons.car, val_cons(*v.cons.cdr, val_nil())));
    }
    case VAL_NIL:
        return val_cons(val_symbol("cons"), val_nil());
    case VAL_MARK:
        return val_symbol("mark");
    default:
        return v;  /* lambdas, prims, errors, vectors, streams */
    }
}

/* demarshal_from_tagged: Shen tagged form → C Value.
   Inverse of marshal_to_tagged.  Non-tagged atoms pass through. */
static Value demarshal_from_tagged(Value tagged) {
    if (tagged.tag == VAL_NUMBER || tagged.tag == VAL_STRING ||
        tagged.tag == VAL_BOOLEAN) return tagged;
    if (tagged.tag == VAL_SYMBOL) {
        if (strcmp(tagged.sym.name, "mark") == 0) return val_nil();
        return tagged;
    }
    if (tagged.tag != VAL_CONS) return tagged;
    /* Check for tagged form: car is a symbol tag */
    Value car = *tagged.cons.car;
    if (car.tag != VAL_SYMBOL) return tagged;
    const char *tag = car.sym.name;

    if (strcmp(tag, "number") == 0 || strcmp(tag, "symbol") == 0 ||
        strcmp(tag, "string") == 0 || strcmp(tag, "boolean") == 0) {
        /* [tag X] — extract the value: cadr of the tagged form */
        Value cdr = *tagged.cons.cdr;
        return *cdr.cons.car;
    }
    if (strcmp(tag, "cons") == 0) {
        Value cdr = *tagged.cons.cdr;
        if (cdr.tag == VAL_NIL) return val_nil();  /* [cons] — empty list */
        /* [cons X Y] — recursively demarshal car and cdr */
        Value tagged_car = *cdr.cons.car;
        Value tagged_cdr = *cdr.cons.cdr;
        Value actual_cdr = *tagged_cdr.cons.car;
        return val_cons(demarshal_from_tagged(tagged_car),
                        demarshal_from_tagged(actual_cdr));
    }
    return tagged;  /* unknown tag */
}

/* ------------------------------------------------------------------ */
/*  Primitive dispatch                                                 */
/* ------------------------------------------------------------------ */

/* Deep structural equality for cons cells and vectors.  Used by the =
   primitive to compare lists, trees, and vectors.  Handles circular
   structures via conservative cycle detection (depth limit). */
static int deep_equal(Value a, Value b) {
    /* Depth limit to avoid infinite recursion on cyclic structures */
    #define DEEP_EQUAL_MAX_DEPTH 1000
    static int depth = 0;
    if (depth > DEEP_EQUAL_MAX_DEPTH) return 0;
    
    if (a.tag != b.tag) return 0;
    switch (a.tag) {
        case VAL_NUMBER:  return a.number == b.number;
        case VAL_STRING:  return a.str.len == b.str.len &&
                                 memcmp(a.str.data, b.str.data, a.str.len) == 0;
        case VAL_SYMBOL:   return strcmp(a.sym.name, b.sym.name) == 0;
        case VAL_BOOLEAN: return a.boolean == b.boolean;
        case VAL_NIL:     return 1;
        case VAL_CONS:
            depth++;
            { int r = deep_equal(*a.cons.car, *b.cons.car) &&
                      deep_equal(*a.cons.cdr, *b.cons.cdr);
              depth--;
              return r; }
        case VAL_VECTOR:
            if (a.vector.len != b.vector.len) return 0;
            depth++;
            for (int i = 0; i < a.vector.len; i++) {
                if (!deep_equal(a.vector.data[i], b.vector.data[i])) {
                    depth--;
                    return 0;
                }
            }
            depth--;
            return 1;
        default:          return 0;
    }
    #undef DEEP_EQUAL_MAX_DEPTH
}

/* Build string representation of any Value into buf (matching shen-scheme's
   put-datum behaviour: full printed form for all types).  Used by str primitive. */
static void str_value(Value v, char *buf, int *pos, int bufsize, int depth) {
    if (depth > 100) { *pos += snprintf(buf + *pos, bufsize - *pos, "..."); return; }
    switch (v.tag) {
        case VAL_SYMBOL:
            *pos += snprintf(buf + *pos, bufsize - *pos, "%s", v.sym.name);
            break;
        case VAL_STRING:
            *pos += snprintf(buf + *pos, bufsize - *pos, "\"%.*s\"", v.str.len, v.str.data);
            break;
        case VAL_NUMBER:
            *pos += snprintf(buf + *pos, bufsize - *pos, "%ld", v.number);
            break;
        case VAL_BOOLEAN:
            *pos += snprintf(buf + *pos, bufsize - *pos, "%s", v.boolean ? "true" : "false");
            break;
        case VAL_NIL:
            *pos += snprintf(buf + *pos, bufsize - *pos, "[]");
            break;
        case VAL_CONS: {
            Value *cur = &v;
            int first = 1;
            *pos += snprintf(buf + *pos, bufsize - *pos, "[");
            while (cur->tag == VAL_CONS && *pos < bufsize - 1) {
                if (!first) *pos += snprintf(buf + *pos, bufsize - *pos, " ");
                first = 0;
                str_value(*cur->cons.car, buf, pos, bufsize, depth + 1);
                cur = cur->cons.cdr;
            }
            if (cur->tag != VAL_NIL && *pos < bufsize - 1) {
                *pos += snprintf(buf + *pos, bufsize - *pos, " . ");
                str_value(*cur, buf, pos, bufsize, depth + 1);
            }
            *pos += snprintf(buf + *pos, bufsize - *pos, "]");
            break;
        }
        case VAL_ERROR:
            *pos += snprintf(buf + *pos, bufsize - *pos, "<error %s>", v.error.message);
            break;
        case VAL_LAMBDA:
            *pos += snprintf(buf + *pos, bufsize - *pos, "<lambda>");
            break;
        case VAL_PRIM:
            *pos += snprintf(buf + *pos, bufsize - *pos, "<prim %s>", v.prim.name);
            break;
        case VAL_VECTOR:
            *pos += snprintf(buf + *pos, bufsize - *pos, "<vector %d>", v.vector.len);
            break;
        case VAL_STREAM:
            *pos += snprintf(buf + *pos, bufsize - *pos, "<stream>");
            break;
        default:
            *pos += snprintf(buf + *pos, bufsize - *pos, "<unknown>");
            break;
    }
}

/* Returns true if `name` is a known C primitive. */
static int exec_primitive_valid(const char *name) {
    static const char *prims[] = {
        "symbol?","boolean?","number?","string?","cons?",
        "error?","function?","stream?",
        "+","-","*","/","=","<",">","<=",">=",
        "cons","hd","tl","cn","emptylist",
        "simple-error","trap-error","error-to-string",
        "eval-kl","absvector","<-address","address->",
        "n->string","string->n","str","tlstr","hdstr","pos",
        "intern","value","open","close","read-byte","write-byte",
        "set","get-time","read-file-as-string",
        "@p","fst","snd","gensym","variable?","newvar",
        "shen.fail!","fail",
        "stinput","stoutput",
        /* YACC terminals (yacc.kl) — not C prims but must resolve as symbols */
        NULL
    };
    for (int i = 0; prims[i]; i++)
        if (strcmp(name, prims[i]) == 0) return 1;
    return 0;
}

static int exec_primitive(const char *name, Value *acc, ValueArray *stack) {
    /* --- Type predicates --- */
    if (strcmp(name, "symbol?") == 0) {
        Value a = va_pop(stack); *acc = val_boolean(a.tag == VAL_SYMBOL); return 0;
    }
    if (strcmp(name, "boolean?") == 0) {
        Value a = va_pop(stack); *acc = val_boolean(a.tag == VAL_BOOLEAN); return 0;
    }
    if (strcmp(name, "string?") == 0) {
        Value a = va_pop(stack); *acc = val_boolean(a.tag == VAL_STRING); return 0;
    }
    if (strcmp(name, "number?") == 0) {
        Value a = va_pop(stack); *acc = val_boolean(a.tag == VAL_NUMBER); return 0;
    }
    if (strcmp(name, "cons?") == 0) {
        Value a = va_pop(stack); *acc = val_boolean(a.tag == VAL_CONS); return 0;
    }
    if (strcmp(name, "error?") == 0) {
        Value a = va_pop(stack); *acc = val_boolean(a.tag == VAL_ERROR); return 0;
    }
    if (strcmp(name, "absvector?") == 0) {
        Value a = va_pop(stack); *acc = val_boolean(a.tag == VAL_VECTOR); return 0;
    }
    if (strcmp(name, "function?") == 0) {
        Value a = va_pop(stack); *acc = val_boolean(a.tag == VAL_LAMBDA || a.tag == VAL_PRIM); return 0;
    }
    if (strcmp(name, "stream?") == 0) {
        Value a = va_pop(stack); *acc = val_boolean(a.tag == VAL_STREAM); return 0;
    }
    if (strcmp(name, "variable?") == 0) {
        /* Shen variable: first char uppercase A-Z, rest alphanumeric + misc chars.
           Misc: ` = * / + _ ? $ ! @ ~ . > < & % ' #  (matching shen.misc?) */
        Value a = va_pop(stack);
        if (a.tag != VAL_SYMBOL) { *acc = val_boolean(0); return 0; }
        const char *s = a.sym.name;
        if (!s[0] || s[0] < 'A' || s[0] > 'Z') { *acc = val_boolean(0); return 0; }
        for (int i = 1; s[i]; i++) {
            int c = (unsigned char)s[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9')) continue;
            /* shen.misc? chars: ` = * / + _ ? $ ! @ ~ . > < & % ' # */
            if (c == '`' || c == '=' || c == '*' || c == '/' || c == '+' ||
                c == '_' || c == '?' || c == '$' || c == '!' || c == '@' ||
                c == '~' || c == '.' || c == '>' || c == '<' || c == '&' ||
                c == '%' || c == '\'' || c == '#') continue;
            *acc = val_boolean(0); return 0;
        }
        *acc = val_boolean(1); return 0;
    }

    /* --- KLambda tuple ops --- */
    if (strcmp(name, "@p") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        *acc = val_cons(a1, a2); return 0;
    }
    if (strcmp(name, "fst") == 0) {
        Value a = va_pop(stack);
        if (a.tag != VAL_CONS) PRIM_TYPE_ERROR("fst on non-cons");
        *acc = *a.cons.car; return 0;
    }
    if (strcmp(name, "snd") == 0) {
        Value a = va_pop(stack);
        if (a.tag != VAL_CONS) PRIM_TYPE_ERROR("snd on non-cons");
        *acc = *a.cons.cdr; return 0;
    }

    /* --- Arithmetic --- */
    if (strcmp(name, "+") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        if (a1.tag != VAL_NUMBER || a2.tag != VAL_NUMBER) PRIM_TYPE_ERROR("+ on non-numbers");
        *acc = val_number(a1.number + a2.number); return 0;
    }
    if (strcmp(name, "-") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        if (a1.tag != VAL_NUMBER || a2.tag != VAL_NUMBER) PRIM_TYPE_ERROR("- on non-numbers");
        *acc = val_number(a1.number - a2.number); return 0;
    }
    if (strcmp(name, "*") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        if (a1.tag != VAL_NUMBER || a2.tag != VAL_NUMBER) PRIM_TYPE_ERROR("* on non-numbers");
        *acc = val_number(a1.number * a2.number); return 0;
    }
    if (strcmp(name, "/") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        if (a1.tag != VAL_NUMBER || a2.tag != VAL_NUMBER) PRIM_TYPE_ERROR("/ on non-numbers");
        if (a2.number == 0) PRIM_TYPE_ERROR("division by zero");
        *acc = val_number(a1.number / a2.number); return 0;
    }

    /* --- Comparison --- */
    if (strcmp(name, "=") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        if (a1.tag == VAL_NUMBER && a2.tag == VAL_NUMBER)
            *acc = val_boolean(a1.number == a2.number);
        else if (a1.tag == VAL_STRING && a2.tag == VAL_STRING)
            *acc = val_boolean(a1.str.len == a2.str.len && memcmp(a1.str.data, a2.str.data, a1.str.len) == 0);
        else if (a1.tag == VAL_SYMBOL && a2.tag == VAL_SYMBOL)
            *acc = val_boolean(strcmp(a1.sym.name, a2.sym.name) == 0);
        else if (a1.tag == VAL_BOOLEAN && a2.tag == VAL_BOOLEAN)
            *acc = val_boolean(a1.boolean == a2.boolean);
        /* cons-vs-symbol and symbol-vs-cons: always false.
           zinc-c currently generates correct hd-wrapped comparisons, so
           flat comparisons like = [number 42] "number" no longer occur. */
        else if ((a1.tag == VAL_CONS && a2.tag == VAL_SYMBOL) ||
                 (a1.tag == VAL_SYMBOL && a2.tag == VAL_CONS))
            *acc = val_boolean(false);
        /* fail is registered as VAL_PRIM so it can be both applied (error)
           and compared in where clauses.  Compare symbol name with prim name. */
        else if (a1.tag == VAL_SYMBOL && a2.tag == VAL_PRIM)
            *acc = val_boolean(strcmp(a1.sym.name, a2.prim.name) == 0);
        else if (a1.tag == VAL_PRIM && a2.tag == VAL_SYMBOL)
            *acc = val_boolean(strcmp(a1.prim.name, a2.sym.name) == 0);
        /* Deep structural equality for cons cells.  Critical for
           macroexpand-h's fixed-point check: (= original walked).
           Without this, cons==cons always returns false (falls through
           to the NIL==NIL catch-all), causing infinite recursion. */
        else if (a1.tag == VAL_CONS && a2.tag == VAL_CONS)
            *acc = val_boolean(deep_equal(a1, a2));
        else if (a1.tag == VAL_VECTOR && a2.tag == VAL_VECTOR)
            *acc = val_boolean(deep_equal(a1, a2));
        else *acc = val_boolean(a1.tag == VAL_NIL && a2.tag == VAL_NIL);
        return 0;
    }
    if (strcmp(name, "<") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        *acc = val_boolean(a1.tag == VAL_NUMBER && a2.tag == VAL_NUMBER && a1.number < a2.number); return 0;
    }
    if (strcmp(name, ">") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        *acc = val_boolean(a1.tag == VAL_NUMBER && a2.tag == VAL_NUMBER && a1.number > a2.number); return 0;
    }
    if (strcmp(name, "<=") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        *acc = val_boolean(a1.tag == VAL_NUMBER && a2.tag == VAL_NUMBER && a1.number <= a2.number); return 0;
    }
    if (strcmp(name, ">=") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        *acc = val_boolean(a1.tag == VAL_NUMBER && a2.tag == VAL_NUMBER && a1.number >= a2.number); return 0;
    }

    /* --- List ops --- */
    if (strcmp(name, "cons") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        *acc = val_cons(a1, a2); return 0;
    }
    if (strcmp(name, "hd") == 0) {
        Value a = va_pop(stack);
        if (a.tag == VAL_NIL) { *acc = val_nil(); return 0; }
        if (a.tag != VAL_CONS) PRIM_TYPE_ERROR("hd on non-cons");
        *acc = *a.cons.car; return 0;
    }
    if (strcmp(name, "tl") == 0) {
        Value a = va_pop(stack);
        if (a.tag == VAL_NIL) { *acc = val_nil(); return 0; }
        if (a.tag != VAL_CONS) PRIM_TYPE_ERROR("tl on non-cons");
        *acc = *a.cons.cdr; return 0;
    }
    if (strcmp(name, "emptylist") == 0) {
        Value a = va_pop(stack);
        if (a.tag == VAL_NUMBER && a.number == 0) { *acc = val_nil(); return 0; }
        if (a.tag != VAL_NUMBER || a.number != 0) PRIM_TYPE_ERROR("emptylist on non-zero");
    }

    /* --- String ops --- */
    if (strcmp(name, "cn") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        char b1[256], b2[256];
        if (a1.tag == VAL_STRING) snprintf(b1, sizeof(b1), "%.*s", a1.str.len, a1.str.data);
        else if (a1.tag == VAL_NUMBER) snprintf(b1, sizeof(b1), "%ld", a1.number);
        else if (a1.tag == VAL_SYMBOL) snprintf(b1, sizeof(b1), "%s", a1.sym.name);
        else if (a1.tag == VAL_BOOLEAN) snprintf(b1, sizeof(b1), "%s", a1.boolean ? "true" : "false");
        else if (a1.tag == VAL_NIL) snprintf(b1, sizeof(b1), "[]");
        else snprintf(b1, sizeof(b1), "[?]");
        if (a2.tag == VAL_STRING) snprintf(b2, sizeof(b2), "%.*s", a2.str.len, a2.str.data);
        else if (a2.tag == VAL_NUMBER) snprintf(b2, sizeof(b2), "%ld", a2.number);
        else if (a2.tag == VAL_SYMBOL) snprintf(b2, sizeof(b2), "%s", a2.sym.name);
        else if (a2.tag == VAL_BOOLEAN) snprintf(b2, sizeof(b2), "%s", a2.boolean ? "true" : "false");
        else if (a2.tag == VAL_NIL) snprintf(b2, sizeof(b2), "[]");
        else snprintf(b2, sizeof(b2), "[?]");
        int len = strlen(b2) + strlen(b1);
        char *r = malloc(len + 1);
        strcpy(r, b1); strcat(r, b2);
        *acc = val_string(r, len); free(r); return 0;
    }
    if (strcmp(name, "n->string") == 0) {
        Value a = va_pop(stack);
        if (a.tag != VAL_NUMBER) PRIM_TYPE_ERROR("n->string on non-number");
        /* Shen: number→character code (ASCII). (n->string 40) → "(" */
        char buf[2] = { (char)a.number, '\0' };
        *acc = val_string(buf, 1); return 0;
    }
    if (strcmp(name, "string->n") == 0) {
        Value a = va_pop(stack);
        if (a.tag != VAL_STRING) PRIM_TYPE_ERROR("string->n on non-string");
        /* Shen: character code of first character. (string->n "(") → 40 */
        *acc = val_number(a.str.len > 0 ? (unsigned char)a.str.data[0] : 0); return 0;
    }
    if (strcmp(name, "str") == 0) {
        Value a = va_pop(stack);
        if (a.tag == VAL_SYMBOL) *acc = val_string(a.sym.name, strlen(a.sym.name));
        else if (a.tag == VAL_STRING) *acc = a;
        else if (a.tag == VAL_NUMBER) { char buf[64]; int len = snprintf(buf, sizeof(buf), "%ld", a.number); *acc = val_string(buf, len); }
        else if (a.tag == VAL_BOOLEAN) *acc = val_string(a.boolean ? "true" : "false",
                                                         a.boolean ? 4 : 5);
        else { static char buf[4096]; int pos = 0; str_value(a, buf, &pos, sizeof(buf), 0); *acc = val_string(buf, pos); }
        return 0;
    }
    if (strcmp(name, "tlstr") == 0) {
        Value a = va_pop(stack);
        if (a.tag != VAL_STRING || a.str.len < 1) PRIM_TYPE_ERROR("tlstr on empty/non-string");
        *acc = val_string_from(&a, 1, a.str.len - 1); return 0;
    }
    if (strcmp(name, "hdstr") == 0) {
        Value a = va_pop(stack);
        if (a.tag != VAL_STRING || a.str.len < 1) PRIM_TYPE_ERROR("hdstr on empty/non-string");
        *acc = val_string_from(&a, 0, 1); return 0;
    }
    if (strcmp(name, "pos") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
#ifdef ZINCVM_DEBUG
        if (a1.tag != VAL_STRING || a2.tag != VAL_NUMBER) {
            if (vm_catch_chain && vm_catch_chain->in_trap_error)
                vm_throw("pos on bad types");
            fprintf(stderr, "runtime: pos on bad types\n"); return -1;
        }
#endif
        /* Shen: (pos Str N) returns the single character at index N.
           Out of bounds → empty string. But when inside trap-error,
           OOB must trigger an error so that callers (e.g. shen.string->byte)
           can catch it and return shen.eos. Without this, shen.write-chars
           loops forever writing NUL bytes. */
        int pl = (int)a2.number;
        if (pl < 0 || pl >= a1.str.len) {
            /* OOB is a semantic error (not a type error). Shen code relies on
               trap-error catching this to detect end-of-string (e.g. strlen-acc).
               Must throw unconditionally when inside trap-error, not just in debug. */
            if (vm_catch_chain && vm_catch_chain->in_trap_error)
                vm_throw("pos out of bounds");
            *acc = val_string("", 0);
        } else *acc = val_string_from(&a1, pl, 1);
        return 0;
    }

    /* --- Symbol ops --- */
    if (strcmp(name, "intern") == 0) {
        Value a = va_pop(stack);
        if (a.tag != VAL_STRING) PRIM_TYPE_ERROR("intern on non-string");
        char buf[256]; int n = a.str.len < 255 ? a.str.len : 255;
        memcpy(buf, a.str.data, n); buf[n] = '\0';
        *acc = val_symbol(buf); return 0;
    }
    if (strcmp(name, "value") == 0) {
        Value a = va_pop(stack);
#ifdef ZINCVM_DEBUG
        if (a.tag != VAL_SYMBOL) {
            if (vm_catch_chain && vm_catch_chain->in_trap_error)
                vm_throw("value on non-symbol");
            fprintf(stderr, "runtime: value on non-symbol\n");
            return -1;
        }
#endif
        *acc = global_get(a.sym.name); return 0;
    }

    /* --- Vector ops --- */
    if (strcmp(name, "absvector") == 0) {
        Value a = va_pop(stack);
        if (a.tag != VAL_NUMBER || a.number < 0) PRIM_TYPE_ERROR("absvector bad size");
        *acc = val_vector((int)a.number); return 0;
    }
    if (strcmp(name, "<-address") == 0) {
        Value vec = va_pop(stack), idx = va_pop(stack);
#ifdef ZINCVM_DEBUG
        if (vec.tag != VAL_VECTOR || idx.tag != VAL_NUMBER) {
            if (vm_catch_chain && vm_catch_chain->in_trap_error)
                vm_throw("<-address bad types");
            fprintf(stderr, "runtime: <-address bad types\n"); return -1;
        }
#endif
        int i = (int)idx.number;
#ifdef ZINCVM_DEBUG
        if (i < 0 || i >= vec.vector.len) {
            if (vm_catch_chain && vm_catch_chain->in_trap_error)
                vm_throw("<-address OOB");
            fprintf(stderr, "runtime: <-address OOB\n"); return -1;
        }
#endif
        *acc = vec.vector.data[i]; return 0;
    }
    if (strcmp(name, "address->") == 0) {
        Value vec = va_pop(stack), idx = va_pop(stack), val = va_pop(stack);
        if (vec.tag != VAL_VECTOR || idx.tag != VAL_NUMBER) PRIM_TYPE_ERROR("address-> bad types");
        int i = (int)idx.number;
        if (i < 0 || i >= vec.vector.len) PRIM_TYPE_ERROR("address-> OOB");
        vec.vector.data[i] = val;
        /* Phase 2 Step 5 — write barrier: if the vector's element array
         * is in old-gen and the stored value references a nursery object,
         * record the element array so the next nursery scavenge scans it
         * (old-gen is not otherwise scanned).  vec is a by-value pop,
         * but vec.vector.data is the real heap array pointer. */
        if (vec.vector.data &&
            gc_in_oldgen(vec.vector.data) &&
            value_references_nursery(&val)) {
            gc_dirty_vectors_add(vec.vector.data);
        }
        *acc = vec; return 0;
    }

    /* --- Error handling --- */
    if (strcmp(name, "simple-error") == 0) {
        Value a = va_pop(stack);
        /* REPL EOF exit: when stdin hits EOF, the reader raises
           "error: empty stream" and shen.loop would normally catch it
           and re-loop infinitely.  In repl_mode, longjmp to repl_exit_jmp
           for a clean exit instead. */
        if (repl_mode && a.tag == VAL_STRING
            && a.str.len == 19 && strncmp(a.str.data, "error: empty stream", 19) == 0) {
            longjmp(repl_exit_jmp, 1);
        }
        char msg[256];
        if (a.tag == VAL_STRING) snprintf(msg, sizeof(msg), "%.*s", a.str.len, a.str.data);
        else snprintf(msg, sizeof(msg), "simple-error called");
        vm_throw(msg);
    }
    if (strcmp(name, "shen.fail!") == 0 || strcmp(name, "fail") == 0) {
        /* (defun fail () shen.fail!) — called as (fail) triggers error.
           When called WITH arguments (e.g., YACC's (fail 0)), return a
           sentinel cons [fail arg] instead.  This matches standard Shen
           behavior where (fail N) creates a parse-failure sentinel for
           parse-failure? to match via =. */
        if (stack->len > 0) {
            Value arg = va_pop(stack);
            *acc = val_cons(val_symbol("fail"), val_cons(arg, val_nil()));
            return 0;
        }
        vm_throw("fail");
    }
    if (strcmp(name, "error-to-string") == 0) {
        Value a = va_pop(stack);
        /* Pin a so a.error.message survives the GC_STR alloc inside
           val_string (4a precise-root site). */
        gc_root_push_value(&a);
        if (a.tag == VAL_ERROR) *acc = val_string(a.error.message, strlen(a.error.message));
        else if (a.tag == VAL_STRING) *acc = a;
        else *acc = val_string("unknown error", 13);
        gc_root_pop();
        return 0;
    }
    if (strcmp(name, "trap-error") == 0) {
        /* RTL: (trap-error Body Handler) — Handler pushed first, then Body.
           Stack: [mark, Handler, Body] → pop Body first, then Handler.
           handler is volatile: after longjmp from vm_throw, the compiler
           must re-read handler from the stack, not from a cached register. */
        volatile Value body = va_pop(stack);
        volatile Value handler = va_pop(stack);
        /* Pin body and handler so their interior pointers (lambda.env,
           lambda.code) survive the GC_VALUE_ARRAY calls below (4a precise-root).
           Cast away volatile — the shadow stack only reads, never writes. */
        gc_root_push_value((Value*)&body);
        gc_root_push_value((Value*)&handler);
        if (handler.tag != VAL_LAMBDA) PRIM_TYPE_ERROR("trap-error handler not fn");
        CatchFrame cf;
        cf.parent = vm_catch_chain;
        cf.in_trap_error = 0;   /* set to 1 inside body branch */
        vm_catch_chain = &cf;
        if (setjmp(cf.buf) == 0) {
            /* Body path: in_trap_error=1 so primitive type errors throw */
            cf.in_trap_error = 1;
            if (body.tag == VAL_LAMBDA) {
                /* kmacros wraps the body in (lambda (newvar) B), so the
                   bytecode expects a dummy parameter at env index 0.
                   Append a nil to the captured env to match. */
                int new_len = body.lambda.env_len + 1;
                Value *new_env = GC_VALUE_ARRAY(new_len);
                if (body.lambda.env_len > 0)
                    memcpy(new_env, body.lambda.env, body.lambda.env_len * sizeof(Value));
                new_env[body.lambda.env_len] = val_nil();
                *acc = vm_exec_env(body.lambda.code, body.lambda.code_len, new_env, new_len);
            }
            else *acc = body;
            vm_catch_chain = cf.parent;
            gc_root_pop();  /* handler */
            gc_root_pop();  /* body */
            return 0;
        } else {
            /* Error path: unlink FIRST so handler's simple-error propagates
               to the enclosing catch frame, not back to this one. */
            vm_catch_chain = cf.parent;
            Value err = cf.error_val;
            Instr *hc = handler.lambda.code; int hl = handler.lambda.code_len;
            Value *henv = GC_VALUE_ARRAY(handler.lambda.env_len + 1);
            if (handler.lambda.env_len > 0)
                memcpy(henv, handler.lambda.env, handler.lambda.env_len * sizeof(Value));
            henv[handler.lambda.env_len] = err;
            handler.lambda.env = henv; handler.lambda.env_len++;
            *acc = vm_exec_env(hc, hl, handler.lambda.env, handler.lambda.env_len);
            gc_root_pop();  /* handler */
            gc_root_pop();  /* body */
            return 0;
        }
    }

    /* --- I/O --- */
    if (strcmp(name, "open") == 0) {
        /* ZINC evaluates args right-to-left, so the stack has:
           [rightmost, leftmost] with leftmost on top.
           For (open path dir): stack=[dir, path], path on top. */
        Value path = va_pop(stack), dir = va_pop(stack);
#ifdef ZINCVM_DEBUG
        if (path.tag != VAL_STRING || dir.tag != VAL_SYMBOL) {
            if (vm_catch_chain && vm_catch_chain->in_trap_error)
                vm_throw("open bad types");
            fprintf(stderr, "runtime: open bad types — path.tag=%d dir.tag=%d", path.tag, dir.tag);
            if (path.tag == VAL_SYMBOL) fprintf(stderr, " path='%s'", path.sym.name);
            if (path.tag == VAL_MARK) fprintf(stderr, " path=MARK");
            if (dir.tag == VAL_STRING) fprintf(stderr, " dir='%.*s'", dir.str.len, dir.str.data);
            fprintf(stderr, " stack_remaining=%d\n", stack->len);
            for (int si = stack->len - 1; si >= 0 && si >= stack->len - 5; si--) {
                fprintf(stderr, "  stack[%d]: tag=%d", si, stack->data[si].tag);
                if (stack->data[si].tag == VAL_SYMBOL) fprintf(stderr, " '%s'", stack->data[si].sym.name);
                if (stack->data[si].tag == VAL_MARK) fprintf(stderr, " MARK");
                fprintf(stderr, "\n");
            }
            return -1;
        }
#endif
        char pb[256]; int n = path.str.len < 255 ? path.str.len : 255;
        memcpy(pb, path.str.data, n); pb[n] = '\0';
        if (strcmp(dir.sym.name, "in") == 0) {
            FILE *f = fopen(pb, "r");
            if (f) { *acc = val_stream_in(f); return 0; }
            /* File not found — treat as string stream */
            if (errno == ENOENT) {
                *acc = val_string_stream_in(path.str.data, path.str.len);
                return 0;
            }
            /* Genuine open failure (EACCES/ENOTDIR/...).  Return false so the
               Shen safe wrapper (safe.open) can detect it and raise a catchable
               simple-error — reference Shen's kl:open raises "File does not
               exist".  The wrapper owns this error, not the C primitive. */
            *acc = val_boolean(false); return 0;
        } else if (strcmp(dir.sym.name, "out") == 0) {
            FILE *f = fopen(pb, "w");
            if (!f) { *acc = val_boolean(false); return 0; }
            *acc = val_stream_out(f); return 0;
        }
#ifdef ZINCVM_DEBUG
        if (vm_catch_chain && vm_catch_chain->in_trap_error)
            vm_throw("open invalid direction");
        fprintf(stderr, "runtime: open direction must be in or out\n"); return -1;
#endif
    }
    if (strcmp(name, "close") == 0) {
        Value s = va_pop(stack);
        if (s.tag != VAL_STREAM) PRIM_TYPE_ERROR("close on non-stream");
        if (s.stream.is_string) {
            int idx = (int)(intptr_t)s.stream.file - 1;
            if (idx < 0 || idx >= n_string_streams) { fprintf(stderr, "runtime: bad string stream idx\n"); return -1; }
            free(string_streams[idx].data);
            string_streams[idx].data = NULL;
            *acc = val_nil(); return 0;
        }
        if (s.stream.file) fclose(s.stream.file);
        *acc = val_nil(); return 0;
    }
    if (strcmp(name, "read-byte") == 0) {
        Value s = va_pop(stack);
#ifdef ZINCVM_DEBUG
        if (s.tag != VAL_STREAM || !s.stream.is_input) {
            if (vm_catch_chain && vm_catch_chain->in_trap_error)
                vm_throw("read-byte on non-input");
            fprintf(stderr, "runtime: read-byte on non-input\n"); return -1;
        }
#endif
        if (s.stream.is_string) {
            int idx = (int)(intptr_t)s.stream.file - 1;
            if (idx < 0 || idx >= n_string_streams) { return -1; }
            if (string_streams[idx].pos >= string_streams[idx].len) {
                *acc = val_number(-1);  /* EOF */
            } else {
                *acc = val_number((unsigned char)string_streams[idx].data[string_streams[idx].pos++]);
            }
            return 0;
        }
        int c = fgetc(s.stream.file); *acc = val_number(c == EOF ? -1 : c); return 0;
    }
    if (strcmp(name, "read-file-as-string") == 0) {
        Value path = va_pop(stack);
        if (path.tag != VAL_STRING) PRIM_TYPE_ERROR("read-file-as-string on non-string");
        char *p = strndup(path.str.data, path.str.len);
        FILE *f = fopen(p, "r");
        free(p);
        if (!f) { fprintf(stderr, "runtime: cannot open file for read-file-as-string\n"); *acc = val_string("", 0); return 0; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc(sz + 1);
        size_t n = fread(buf, 1, sz, f);
        fclose(f);
        buf[n] = '\0';
        *acc = val_string(buf, n);
        free(buf);
        return 0;
    }
    if (strcmp(name, "write-byte") == 0) {
        /* RTL: (write-byte Byte Stream) — Stream pushed first (bottom), Byte last (top). */
        Value byte = va_pop(stack);
        Value s    = va_pop(stack);
#ifdef ZINCVM_DEBUG
        if (s.tag != VAL_STREAM || s.stream.is_input) {
            if (vm_catch_chain && vm_catch_chain->in_trap_error)
                vm_throw("write-byte on non-output");
            fprintf(stderr, "runtime: write-byte on non-output\n"); return -1;
        }
        if (byte.tag != VAL_NUMBER) {
            if (vm_catch_chain && vm_catch_chain->in_trap_error)
                vm_throw("write-byte requires number");
            fprintf(stderr, "runtime: write-byte requires number\n"); return -1;
        }
#endif
        fputc((int)byte.number, s.stream.file);
        if (s.stream.file == stdout) fflush(stdout);
        *acc = val_number(byte.number); return 0;
    }
    if (strcmp(name, "get-time") == 0) {
        Value mode = va_pop(stack);
        if (mode.tag != VAL_SYMBOL) PRIM_TYPE_ERROR("get-time requires symbol");
        if (strcmp(mode.sym.name, "unix") == 0 || strcmp(mode.sym.name, "real") == 0)
            { *acc = val_number((long)time(NULL)); return 0; }
        if (strcmp(mode.sym.name, "run") == 0) { *acc = val_number((long)clock()); return 0; }
#ifdef ZINCVM_DEBUG
        if (vm_catch_chain && vm_catch_chain->in_trap_error)
            vm_throw("get-time unknown mode");
        fprintf(stderr, "runtime: unknown get-time mode '%s'\n", mode.sym.name); return -1;
#endif
    }

    /* --- Meta --- */
    if (strcmp(name, "gensym") == 0) {
        static long gensym_counter = 0;
        char buf[64];
        if (stack->len > 0) va_pop(stack);  /* consume prefix arg (Shen compiler passes it) */
        snprintf(buf, sizeof(buf), "shen.gensym_%ld", gensym_counter++);
        *acc = val_symbol(buf); return 0;
    }
    if (strcmp(name, "newvar") == 0) {
        static int newvar_counter = 0;
        char buf[64];
        if (stack->len > 0) va_pop(stack);  /* consume prefix arg */
        snprintf(buf, sizeof(buf), "shen.V%d", newvar_counter++);
        *acc = val_symbol(buf); return 0;
    }
    if (strcmp(name, "set") == 0) {
        Value sym = va_pop(stack), v = va_pop(stack);
        if (sym.tag != VAL_SYMBOL) PRIM_TYPE_ERROR("set requires symbol");
        global_set(sym.sym.name, v); *acc = v; return 0;
    }
    if (strcmp(name, "eval-kl") == 0) {
        Value a = va_pop(stack);
        /* eval-kl chain (interp.shen:80):
           marshal native → tagged form
           → extract-kl    (tagged → raw KLambda)
           → kl->zinc      (KLambda → ZINC bytecode)
           → toplevel-interp (ZINC bytecode → tagged result)
           → demarshal tagged result → native Value

           The three closures are in the bundle via set-toplevel
           in interp.shen:193-195.  The recursion guard prevents
           infinite re-entry when %% eval-kl is called from within
           Shen code executed by the chain. */
        /* Recursion guard removed — was breaking defun compilation
           by returning identity instead of evaluating nested eval-kl.
           With Boehm GC, deep recursion through eval-kl→kl→zinc→eval-kl
           is safe (no GC corruption).  A CatchFrame swallows pipeline
           errors (simple-error → longjmp) and returns identity. */
        CatchFrame cf;
        cf.parent = vm_catch_chain;
        cf.in_trap_error = 0;
        vm_catch_chain = &cf;
        volatile Value result = a;  /* default: identity; volatile so the longjmp path re-reads from the stack */
        /* Watermark for longjmp unwind: any roots pushed inside the setjmp==0
           block must be truncated on the error path (4a precise-root). */
        volatile size_t eval_kl_wm = gc_root_watermark();
        if (setjmp(cf.buf) == 0) {

        /* Marshal native Value → Shen tagged form */
        Value tagged = marshal_to_tagged(a);
        gc_root_push_value(&tagged);

        /* Step 1: extract-kl — tagged form → raw KLambda */
        Value extkl = global_get("extract-kl");
        gc_root_push_value(&extkl);
        if (extkl.tag != VAL_LAMBDA) {
            fprintf(stderr, "runtime: eval-kl: extract-kl not found in bundle\n");
            goto done;
        }
        Value *env1 = GC_VALUE_ARRAY(extkl.lambda.env_len + 1);
        if (extkl.lambda.env_len > 0)
            memcpy(env1, extkl.lambda.env, extkl.lambda.env_len * sizeof(Value));
        env1[extkl.lambda.env_len] = tagged;
        Value klambda = vm_exec_env(extkl.lambda.code, extkl.lambda.code_len,
                                     env1, extkl.lambda.env_len + 1);
        gc_root_push_value(&klambda);

        /* Step 2: kl->zinc — raw KLambda → ZINC bytecode */
        Value klzinc = global_get("kl->zinc");
        gc_root_push_value(&klzinc);
        if (klzinc.tag != VAL_LAMBDA) {
            fprintf(stderr, "runtime: eval-kl: kl->zinc not found in bundle\n");
            goto done;
        }
        Value *env2 = GC_VALUE_ARRAY(klzinc.lambda.env_len + 1);
        if (klzinc.lambda.env_len > 0)
            memcpy(env2, klzinc.lambda.env, klzinc.lambda.env_len * sizeof(Value));
        env2[klzinc.lambda.env_len] = klambda;
        Value zinc_code = vm_exec_env(klzinc.lambda.code, klzinc.lambda.code_len,
                                       env2, klzinc.lambda.env_len + 1);
        gc_root_push_value(&zinc_code);

        /* Step 3: toplevel-interp — ZINC bytecode → tagged result */
        Value tli = global_get("toplevel-interp");
        gc_root_push_value(&tli);
        if (tli.tag != VAL_LAMBDA) {
            fprintf(stderr, "runtime: eval-kl: toplevel-interp not found in bundle\n");
            goto done;
        }
        Value *env3 = GC_VALUE_ARRAY(tli.lambda.env_len + 1);
        if (tli.lambda.env_len > 0)
            memcpy(env3, tli.lambda.env, tli.lambda.env_len * sizeof(Value));
        env3[tli.lambda.env_len] = zinc_code;
        Value tagged_result = vm_exec_env(tli.lambda.code, tli.lambda.code_len,
                                           env3, tli.lambda.env_len + 1);
        gc_root_push_value(&tagged_result);

        /* Step 4: demarshal tagged result → native Value */
        result = demarshal_from_tagged(tagged_result);

        done:
        gc_root_pop_to(eval_kl_wm);
        vm_catch_chain = cf.parent;
        *acc = result;
        return 0;
        } /* end setjmp == 0 block */
        /* Error path: unlink, truncate any roots pushed before longjmp. */
        gc_root_pop_to(eval_kl_wm);
        vm_catch_chain = cf.parent;
        *acc = result;
        return 0;
    }

    /* --- Stream accessors for REPL --- */
    if (strcmp(name, "stinput") == 0) {
        Value v; memset(&v, 0, sizeof(v));
        v.tag = VAL_STREAM;
        v.stream.file = stdin;
        v.stream.is_input = 1;
        *acc = v; return 0;
    }
    if (strcmp(name, "stoutput") == 0) {
        Value v; memset(&v, 0, sizeof(v));
        v.tag = VAL_STREAM;
        v.stream.file = stdout;
        v.stream.is_input = 0;
        *acc = v; return 0;
    }

    fprintf(stderr, "runtime: unknown primitive '%s'\n", name);
    return -1;
}

/* ------------------------------------------------------------------ */
/*  csexp parser                                                       */
/* ------------------------------------------------------------------ */

static void skip_ws(ParseState *ps) {
    while (isspace((unsigned char)*ps->p)) ps->p++;
}
static int parse_int(ParseState *ps) {
    int n = 0;
    if (!isdigit((unsigned char)*ps->p)) PARSE_ERROR("expected digit");
    while (isdigit((unsigned char)*ps->p)) { n = n * 10 + (*ps->p - '0'); ps->p++; }
    return n;
}
static Value parse_csexp_atom(ParseState *ps) {
    skip_ws(ps);
    if (*ps->p != '[') PARSE_ERROR("expected '[' for csexp atom");
    ps->p++;
    int len = parse_int(ps);
    if (*ps->p != ':') PARSE_ERROR("expected ':' after length");
    ps->p++;
    char type = *ps->p; ps->p++;
    if (*ps->p != ']') PARSE_ERROR("expected ']' after type");
    ps->p++;
    if (len < 0) PARSE_ERROR("negative length");
    char *buf = malloc(len + 1);
    memcpy(buf, ps->p, len); buf[len] = '\0'; ps->p += len;
    Value v; memset(&v, 0, sizeof(v));
    switch (type) {
    case 's': v = val_symbol(buf); break;
    case 'n': v = val_number(atol(buf)); break;
    case 'S': v = val_string(buf, len); break;
    case 'b': v = val_boolean(strcmp(buf, "true") == 0); break;
    default: free(buf); { char msg[64]; snprintf(msg, sizeof(msg), "unknown csexp type '%c'", type); PARSE_ERROR(msg); }
    }
    free(buf); return v;
}
static int parse_csexp_list(ParseState *ps, Instr **out);

static int parse_body(ParseState *ps, Instr **out) {
    int cap = 16, len = 0;
    Instr *code = (Instr*)gc_alloc(cap * sizeof(Instr), GC_TYPE_INSTR_ARRAY);
    while (1) {
        skip_ws(ps); char c = *ps->p;
        if (c == ')' || c == '\0') break;
        if (c == '(') PARSE_ERROR("unexpected nested list in body");
        Instr instr; memset(&instr, 0, sizeof(instr));
        switch (c) {
        case 'm': instr.op = OP_PUSHMARK; ps->p++; break;
        case 'p': instr.op = OP_APPLY;    ps->p++; break;
        case 'r': instr.op = OP_GRAB;     ps->p++; break;
        case 'v': instr.op = OP_RETURN;   ps->p++; break;
        case 'e': instr.op = OP_LET;      ps->p++; break;
        case 'd': instr.op = OP_ENDLET;   ps->p++; break;
        case 't': instr.op = OP_APPTERM;  ps->p++; break;
        case 'a': instr.op = OP_ACCESS;   ps->p++; instr.operand = parse_csexp_atom(ps); break;
        case 'f': instr.op = OP_JMPF;     ps->p++; instr.operand = parse_csexp_atom(ps); break;
        case 'j': instr.op = OP_JMP;      ps->p++; instr.operand = parse_csexp_atom(ps); break;
        case 'n': instr.op = OP_NUMBER;   ps->p++; instr.operand = parse_csexp_atom(ps); break;
        case 'g': instr.op = OP_GLOBAL;   ps->p++; instr.operand = parse_csexp_atom(ps); break;
        case 's': instr.op = OP_SYMBOL;   ps->p++; instr.operand = parse_csexp_atom(ps); break;
        case 'P': instr.op = OP_PRIM;     ps->p++; instr.operand = parse_csexp_atom(ps); break;
        case 'S': instr.op = OP_STRING;   ps->p++; instr.operand = parse_csexp_atom(ps); break;
        case 'b': instr.op = OP_BOOLEAN;  ps->p++; instr.operand = parse_csexp_atom(ps); break;
        case 'c':
            instr.op = OP_CUR; ps->p++; skip_ws(ps);
            if (*ps->p != '(') PARSE_ERROR("expected '(' after 'c'");
            ps->p++; instr.closure_len = parse_body(ps, &instr.closure_code);
            if (*ps->p != ')') PARSE_ERROR("expected ')' after cur body");
            ps->p++; break;
        default: { char msg[64]; snprintf(msg, sizeof(msg), "unknown opcode '%c' (0x%02x)", c, (unsigned char)c); PARSE_ERROR(msg); }
        }
        if (len >= cap) { int old_cap = cap; cap *= 2; code = (Instr*)gc_realloc(code, old_cap * sizeof(Instr), cap * sizeof(Instr), GC_TYPE_INSTR_ARRAY); }
        code[len++] = instr;
    }
    *out = code; return len;
}
static int parse_csexp_list(ParseState *ps, Instr **out) {
    skip_ws(ps);
    if (*ps->p != '(') PARSE_ERROR("expected '(' for list");
    ps->p++;
    int len = parse_body(ps, out);
    if (*ps->p != ')') PARSE_ERROR("expected ')' after list body");
    ps->p++;
    return len;
}
int parse_bytecode(const char *str, Instr **out) {
    ParseState ps; ps.p = str; ps.start = str;
    if (setjmp(parse_err_jmp)) { fprintf(stderr, "%s\n", parse_err_msg); *out = NULL; return 0; }
    return parse_csexp_list(&ps, out);
}

/* ------------------------------------------------------------------ */
/*  Debug printing                                                     */
/* ------------------------------------------------------------------ */

void print_instr(Instr *code, int len, int indent) {
    for (int i = 0; i < len; i++) {
        for (int j = 0; j < indent; j++) printf("  ");
        Instr *in = &code[i];
        switch (in->op) {
        case OP_PUSHMARK: printf("pushmark\n"); break;
        case OP_APPLY:    printf("apply\n"); break;
        case OP_GRAB:     printf("grab\n"); break;
        case OP_RETURN:   printf("return\n"); break;
        case OP_LET:      printf("let\n"); break;
        case OP_ENDLET:   printf("endlet\n"); break;
        case OP_APPTERM:  printf("appterm\n"); break;
        case OP_ACCESS:   printf("access "); print_value(in->operand); printf("\n"); break;
        case OP_GLOBAL:   printf("global "); print_value(in->operand); printf("\n"); break;
        case OP_JMPF:     printf("jmpf "); print_value(in->operand); printf(" (tgt=%d)\n", in->jmp_target); break;
        case OP_JMP:      printf("jmp ");  print_value(in->operand); printf(" (tgt=%d)\n", in->jmp_target); break;
        case OP_NUMBER:   printf("number "); print_value(in->operand); printf("\n"); break;
        case OP_STRING:   printf("string "); print_value(in->operand); printf("\n"); break;
        case OP_SYMBOL:   printf("symbol "); print_value(in->operand); printf("\n"); break;
        case OP_BOOLEAN:  printf("boolean "); print_value(in->operand); printf("\n"); break;
        case OP_PRIM:     printf("prim "); print_value(in->operand); printf("\n"); break;
        case OP_CUR:
            printf("cur (code=%d):\n", in->closure_len);
            print_instr(in->closure_code, in->closure_len, indent + 1);
            for (int j = 0; j < indent; j++) printf("  ");
            printf("endcur\n");
            break;
        default: printf("??? (op=%c)\n", in->op);
        }
    }
}

/* Resolve trace names to code pointers.  Call after parse_bundle. */
void trace_resolve(void) {
    for (int i = 0; i < num_traced; i++) {
        Value g = global_get(traced_name[i]);
        if (g.tag == VAL_LAMBDA) {
            traced_code[i] = g.lambda.code;
            fprintf(stderr, "[trace] watching '%s' (%d instrs)\n",
                    traced_name[i], g.lambda.code_len);
        } else {
            fprintf(stderr, "[trace] '%s' not a lambda (tag=%d), skipping\n",
                    traced_name[i], g.tag);
            traced_code[i] = NULL;
        }
    }
}

/* Print one instruction in raw format (same style as zincdec --raw) */
static void print_instr_one(Instr *in, int pc) {
    printf("  %04d  ", pc);
    switch (in->op) {
    case OP_PUSHMARK: printf("pushmark\n"); break;
    case OP_APPLY:    printf("apply\n"); break;
    case OP_GRAB:     printf("grab\n"); break;
    case OP_RETURN:   printf("return\n"); break;
    case OP_LET:      printf("let\n"); break;
    case OP_ENDLET:   printf("endlet\n"); break;
    case OP_APPTERM:  printf("appterm\n"); break;
    case OP_ACCESS:   printf("access "); print_value(in->operand); printf("\n"); break;
    case OP_GLOBAL:   printf("global "); print_value(in->operand); printf("\n"); break;
    case OP_JMPF:     printf("jmpf "); print_value(in->operand);
                      printf(" (tgt=%d)\n", in->jmp_target); break;
    case OP_JMP:      printf("jmp ");  print_value(in->operand);
                      printf(" (tgt=%d)\n", in->jmp_target); break;
    case OP_NUMBER:   printf("number "); print_value(in->operand); printf("\n"); break;
    case OP_STRING:   printf("string "); print_value(in->operand); printf("\n"); break;
    case OP_SYMBOL:   printf("symbol "); print_value(in->operand); printf("\n"); break;
    case OP_BOOLEAN:  printf("boolean "); print_value(in->operand); printf("\n"); break;
    case OP_PRIM:     printf("prim "); print_value(in->operand); printf("\n"); break;
    case OP_CUR:      printf("cur (code=%d)\n", in->closure_len); break;
    default:          printf("??? (%c)\n", in->op);
    }
}

/* ------------------------------------------------------------------ */
/*  Resolve jumps                                                      */
/* ------------------------------------------------------------------ */

void resolve_jumps(Instr *code, int len) {
    for (int i = 0; i < len; i++) {
        Instr *in = &code[i];
        switch (in->op) {
        case OP_JMP: case OP_JMPF: case OP_ACCESS:
            if (in->operand.tag == VAL_NUMBER) in->jmp_target = (int)in->operand.number;
            else in->jmp_target = 0;
            break;
        case OP_CUR: resolve_jumps(in->closure_code, in->closure_len); break;
        default: break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  VM execution                                                       */
/* ------------------------------------------------------------------ */

static Value lookup_env(int n, Value *env, int env_len) {
    if (n < 0 || n >= env_len) {
        /* Out-of-bounds access: return 0 silently.
           This occurs in nested closures with empty captured environments
           during interp execution. The sentinel value allows graceful
           degradation; downstream guards (cons?, =, etc.) reject it. */
        Value v; memset(&v, 0, sizeof(v)); v.tag = VAL_NUMBER; v.number = 0; return v;
    }
    return env[env_len - 1 - n];
}
static void env_push(Value **env, int *env_len, int *env_cap, Value v) {
    if (*env_len >= *env_cap) {
        int new_cap = *env_cap ? (*env_cap) * 2 : 4;
        Value *new_env = GC_VALUE_ARRAY(new_cap);
        if (*env_len > 0) memcpy(new_env, *env, *env_len * sizeof(Value));
        *env = new_env; *env_cap = new_cap;
    }
    (*env)[(*env_len)++] = v;
}
static Value env_pop(Value **env, int *env_len) {
    if (*env_len <= 0) {
        if (vm_catch_chain && vm_catch_chain->in_trap_error)
            vm_throw("runtime: pop empty environment");
        fprintf(stderr, "runtime: pop empty environment\n"); exit(1);
    }
    return (*env)[--(*env_len)];
}

int trace_counter = -1;
int trace_limit = 0;

Value vm_exec_env(Instr *code, int code_len, Value *init_env, int init_env_len) {
    ValueArray stack; va_init(&stack);
    Value *env = NULL; int env_len = 0, env_cap = 0;
    if (init_env_len > 0 && init_env) {
        env_cap = init_env_len;
        env = GC_VALUE_ARRAY(env_cap);
        memcpy(env, init_env, init_env_len * sizeof(Value));
        env_len = init_env_len;
    }
    Value acc; memset(&acc, 0, sizeof(acc)); acc.tag = VAL_NIL;
    CallFrame *frame_stack = (CallFrame*)gc_alloc_oldgen(CALL_STACK_DEPTH * sizeof(CallFrame), GC_TYPE_CALLFRAME_ARRAY);
    if (!frame_stack) { va_free(&stack); return acc; }
    memset(frame_stack, 0, CALL_STACK_DEPTH * sizeof(CallFrame));
    int frames_sp = 0;
    int pc = 0; Instr *cur_code = code; int cur_len = code_len;
    int instr_count = 0;
    gc_root_push_value(&acc);              /* ROOT_VALUE */
    gc_root_push_ptr((void**)&env);         /* ROOT_PTR — Value** */
    gc_root_push_ptr((void**)&stack.data);  /* ROOT_PTR — Value** */
    gc_root_push_ptr((void**)&cur_code);    /* ROOT_PTR — Instr** */
    gc_root_push_ptr((void**)&frame_stack); /* ROOT_PTR — CallFrame** */
    #define INSTR_HARD_LIMIT 500000000

    while (1) {
        if (++instr_count >= INSTR_HARD_LIMIT) {
            fprintf(stderr, "[HARD LIMIT] %d instructions, aborting at pc=%d frames=%d\n",
                    instr_count, pc, frames_sp);
            goto done;
        }
        if (trace_counter >= 0) {
            if (++trace_counter >= trace_limit) trace_counter = -1;
        }
        if (pc < 0 || pc >= cur_len) {
            if (frames_sp > 0) {
                CallFrame *cf = &frame_stack[--frames_sp];
                
                
                
                cur_code = cf->code; cur_len = cf->code_len; pc = cf->pc;
                env = cf->env; env_len = cf->env_len; env_cap = cf->env_cap;
                va_free(&stack);
                stack = cf->stack;
                continue;
            }
            break;
        }
        Instr *in = &cur_code[pc];
        /* Trace: print instruction if current code is being watched */
        if (num_traced > 0) {
            for (int t = 0; t < num_traced; t++) {
                if (cur_code == traced_code[t]) {
                    printf("[%s] ", traced_name[t]);
                    print_instr_one(in, pc);
                    break;
                }
            }
        }
        switch (in->op) {
        case OP_NUMBER: case OP_STRING: case OP_SYMBOL: case OP_BOOLEAN:
            acc = in->operand;
            va_push(&stack, acc);
            pc++; break;
        case OP_PRIM: {
            /* ZINC [prim X]: args already on stack (auto-pushed by loads).
               Execute primitive, push result. */
            const char *pn = (in->operand.tag == VAL_SYMBOL) ? in->operand.sym.name : "";
            if (exec_primitive(pn, &acc, &stack) < 0) goto done;
            va_push(&stack, acc);
            if (trace_counter >= 0 && trace_counter < trace_limit + 5) {
                fprintf(stderr, "    -> acc after prim %s: ", pn);
                print_value(acc); fprintf(stderr, " (tag=%d)\n", acc.tag);
            }
            pc++;
            break;
        }
        case OP_PUSHMARK: va_push(&stack, val_mark()); pc++; break;
        case OP_GRAB: {
            if (stack.len > 0 && va_peek(&stack).tag == VAL_MARK) {
                va_pop(&stack);
                if (frames_sp > 0) {
                    CallFrame *cf = &frame_stack[--frames_sp];
                    
                
                
                    cur_code = cf->code; cur_len = cf->code_len; pc = cf->pc;
                    env = cf->env; env_len = cf->env_len; env_cap = cf->env_cap;
                    stack = cf->stack;
                    va_push(&stack, acc);  /* push return value to caller stack */
                } else goto done;
            } else if (stack.len > 0) { env_push(&env, &env_len, &env_cap, va_pop(&stack)); pc++; }
            else pc++;
            break;
        }
        case OP_APPLY: {
            /* Standard ZINC: function is auto-pushed on stack top.
               Pop it, then collect args up to the mark.
               zinc-c always emits pushmark — the mark is required. */
            if (stack.len > 0) acc = va_pop(&stack);  /* pop function */
            if (acc.tag == VAL_LAMBDA) {
                check_closure(acc, "APPLY");
                /* Collect all non-mark args (stop at the mark).
                   Stale marks and function already popped above. */
                int nargs = 0;
                Value argbuf[64];
                while (stack.len > 0 && va_peek(&stack).tag != VAL_MARK) {
                    if (nargs < 64) argbuf[nargs++] = va_pop(&stack);
                    else { vm_throw("runtime: too many args (>64)"); }
                }
                /* Pop the required mark (zinc-c always emits pushmark) */
                if (stack.len == 0 || va_peek(&stack).tag != VAL_MARK) {
                    fprintf(stderr, "runtime: apply missing pushmark\n"); goto done;
                }
                va_pop(&stack);

                if (frames_sp >= CALL_STACK_DEPTH) { goto done; }
                CallFrame *cf = &frame_stack[frames_sp++];
                cf->code = cur_code; cf->code_len = cur_len; cf->pc = pc + 1;
                cf->env = env; cf->env_len = env_len; cf->env_cap = env_cap;
                cf->stack = stack; va_init(&stack);
                
                
                
                va_init(&stack);
                env = NULL; env_len = 0; env_cap = 0;

                int lambda_env_len = acc.lambda.env_len;
                int new_env_len = lambda_env_len + nargs;
                gc_root_push_value_array(argbuf, &nargs);
                Value *ne = GC_VALUE_ARRAY(new_env_len);
                /* acc.lambda.env stays reachable via the conservative stack
                 * scan — safe to read after gcalloc. */
                cur_code = acc.lambda.code; cur_len = acc.lambda.code_len;
                Value *lambda_env = acc.lambda.env;
                if (lambda_env_len > 0 && lambda_env) {
                    memcpy(ne, lambda_env, lambda_env_len * sizeof(Value));
                }
                for (int i = 0; i < nargs; i++)
                    ne[lambda_env_len + i] = argbuf[i];
                env = ne; env_len = new_env_len; env_cap = new_env_len;
                gc_root_pop();
                pc = 0;
            } else if (acc.tag == VAL_PRIM) {
                /* Function already popped; pop mark before args if present */
                if (stack.len > 0 && va_peek(&stack).tag == VAL_MARK) va_pop(&stack);
                const char *pn = acc.prim.name;
                if (exec_primitive(pn, &acc, &stack) < 0) goto done;
                va_push(&stack, acc);
                pc++;
            } else {
                if (vm_catch_chain && vm_catch_chain->in_trap_error)
                    vm_throw("apply non-callable");
                fprintf(stderr, "runtime: apply non-callable tag=%d", acc.tag);
                if (acc.tag == VAL_SYMBOL) {
                    fprintf(stderr, " sym='%s'", acc.sym.name);
                    /* Show recent globals that resolved to this symbol */
                    fprintf(stderr, " at pc=%d depth=%d", pc, frames_sp);
                }
                fprintf(stderr, "\n");
                goto done;
            }
            break;
        }
        case OP_RETURN: {
            if (frames_sp > 0) {
                CallFrame *cf = &frame_stack[--frames_sp];
                
                
                
                cur_code = cf->code; cur_len = cf->code_len; pc = cf->pc;
                env = cf->env; env_len = cf->env_len; env_cap = cf->env_cap;
                va_free(&stack);
                stack = cf->stack;
                va_push(&stack, acc);  /* push return value to caller stack */
            } else goto done;
            break;
        }
        case OP_ACCESS:
            acc = lookup_env((in->operand.tag == VAL_NUMBER) ? (int)in->operand.number : in->jmp_target, env, env_len);
            va_push(&stack, acc);
            pc++; break;
        case OP_GLOBAL: {
            const char *nm = (in->operand.tag == VAL_SYMBOL) ? in->operand.sym.name : "";
            acc = global_get(nm);
            va_push(&stack, acc);
            pc++; break;
        }
        case OP_LET: {
            Value v = (stack.len > 0) ? va_pop(&stack) : acc;
            env_push(&env, &env_len, &env_cap, v);
            pc++; break;
        }
        case OP_ENDLET: if (env_len > 0) env_pop(&env, &env_len); pc++; break;
        case OP_JMP: pc = in->jmp_target; break;
        case OP_JMPF: {
            Value cond = (stack.len > 0) ? va_pop(&stack) : acc;
            if (!(cond.tag == VAL_BOOLEAN && !cond.boolean)) pc++;
            else pc = in->jmp_target;
            break;
        }
        case OP_CUR: {
            /* val_lambda now GC-allocates its own env copy */
            acc = val_lambda(in->closure_code, in->closure_len, env, env_len);
            va_push(&stack, acc);
            pc++; break;
        }
        case OP_APPTERM: {
            /* Standard ZINC: pop function from stack top, collect args
               up to mark.  zinc-t always emits pushmark — required. */
            if (stack.len > 0) acc = va_pop(&stack);  /* pop function */
            if (acc.tag == VAL_LAMBDA) {
                check_closure(acc, "APPTERM");
                if (stack.len <= 0) { fprintf(stderr, "runtime: appterm empty stack\n"); goto done; }
                int nargs = 0;
                Value argbuf[64];
                while (stack.len > 0 && va_peek(&stack).tag != VAL_MARK) {
                    if (nargs < 64) argbuf[nargs++] = va_pop(&stack);
                    else { vm_throw("runtime: appterm too many args (>64)"); }
                }
                /* zinc-t always emits pushmark — required */
                if (stack.len == 0 || va_peek(&stack).tag != VAL_MARK) {
                    fprintf(stderr, "runtime: appterm missing pushmark\n"); goto done;
                }
                va_pop(&stack);  /* pop mark */
                if (nargs == 0) { fprintf(stderr, "runtime: appterm zero args\n"); goto done; }

                int lambda_env_len = acc.lambda.env_len;
                int new_env_len = lambda_env_len + nargs;
                gc_root_push_value_array(argbuf, &nargs);
                Value *ne = GC_VALUE_ARRAY(new_env_len);
                cur_code = acc.lambda.code; cur_len = acc.lambda.code_len;
                Value *lambda_env = acc.lambda.env;
                if (lambda_env_len > 0 && lambda_env) {
                    memcpy(ne, lambda_env, lambda_env_len * sizeof(Value));
                }
                for (int i = 0; i < nargs; i++)
                    ne[lambda_env_len + i] = argbuf[i];
                env = ne; env_len = new_env_len; env_cap = new_env_len;
                gc_root_pop();
                pc = 0; break;
            } else if (acc.tag == VAL_PRIM) {
                /* Function already popped; pop mark before args if present */
                if (stack.len > 0 && va_peek(&stack).tag == VAL_MARK) va_pop(&stack);
                const char *pn = acc.prim.name;
                if (exec_primitive(pn, &acc, &stack) < 0) goto done;
                va_push(&stack, acc);
                pc++; break;
            } else {
                if (vm_catch_chain && vm_catch_chain->in_trap_error)
                    vm_throw("appterm non-lambda");
                fprintf(stderr, "runtime: appterm non-lambda\n"); goto done;
            }
        }
        default: fprintf(stderr, "runtime: unknown op '%c' at pc=%d\n", in->op, pc); goto done;
        }
    }
done:
    gc_root_pop(); gc_root_pop(); gc_root_pop(); gc_root_pop(); gc_root_pop();
    va_free(&stack);
    /* frame_stack is GC-allocated — no free needed */
    return acc;
}

Value vm_exec(Instr *code, int code_len) {
    return vm_exec_env(code, code_len, NULL, 0);
}

/* ------------------------------------------------------------------ */
/*  Meta REPL                                                          */
/* ------------------------------------------------------------------ */
#ifndef ZINCTEST
/* Call a bundled lambda closure by name with a single argument.
   Mirrors the convention used in eval-kl: the arg is placed after the
   closure's captured env, and the closure reads its param via `access N`. */
static Value call_closure1(const char *name, Value arg) {
    Value g = global_get(name);
    if (g.tag != VAL_LAMBDA) {
        fprintf(stderr, "meta-repl: %s not found in bundle (tag=%d)\n", name, g.tag);
        return val_nil();
    }
    int env_len = g.lambda.env_len;
    Value *env = GC_VALUE_ARRAY(env_len + 1);
    if (env_len > 0) memcpy(env, g.lambda.env, env_len * sizeof(Value));
    env[env_len] = arg;
    return vm_exec_env(g.lambda.code, g.lambda.code_len, env, env_len + 1);
}

/* Call a bundled lambda closure by name with three arguments
   (used for parse-exprs Str Pos Len). */
static Value call_closure3(const char *name, Value a, Value b, Value c) {
    Value g = global_get(name);
    if (g.tag != VAL_LAMBDA) {
        fprintf(stderr, "meta-repl: %s not found in bundle (tag=%d)\n", name, g.tag);
        return val_nil();
    }
    int env_len = g.lambda.env_len;
    Value *env = GC_VALUE_ARRAY(env_len + 3);
    if (env_len > 0) memcpy(env, g.lambda.env, env_len * sizeof(Value));
    env[env_len] = a; env[env_len+1] = b; env[env_len+2] = c;
    return vm_exec_env(g.lambda.code, g.lambda.code_len, env, env_len + 3);
}

static int is_defun_form(Value f) {
    if (f.tag != VAL_CONS) return 0;
    Value h = *f.cons.car;
    return h.tag == VAL_SYMBOL && strcmp(h.sym.name, "defun") == 0;
}

/* Read one line from stdin (until newline or EOF), growing the buffer.
   Returns malloc'd string (caller frees) or NULL on EOF. */
static char *read_stdin_line(void) {
    int cap = 256, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    int ch;
    while ((ch = fgetc(stdin)) != EOF && ch != '\n') {
        if (len >= cap - 1) {
            cap *= 2;
            char *newbuf = realloc(buf, cap);
            if (!newbuf) { free(buf); return NULL; }
            buf = newbuf;
        }
        buf[len++] = (char)ch;
    }
    if (ch == EOF && len == 0) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

/* Print a Shen-style representation of a value (uses str_value). */
static void print_shen(Value v) {
    char *pbuf = malloc(4096); int pos = 0;
    str_value(v, pbuf, &pos, 4096, 0);
    pbuf[pos] = '\0';
    printf("%s\n", pbuf);
    fflush(stdout);
    free(pbuf);
}

/* The meta REPL: reads KLambda text, parses it with the bundled
   parse-exprs reader, evaluates each form via eval-kl (expressions)
   or interp-eval (defuns).  Bypasses the Shen OS REPL (shen.repl)
   which is not present in the reduced bundle. */
static void meta_repl(void) {
    printf("=== Meta REPL (metacircular KLambda interpreter, no Shen OS) ===\n");
    printf("Type KLambda expressions.  Primitive calls evaluate, e.g.\n");
    printf("  (+ 1 2)  (cons 1 2)  (hd ...)  (tl ...)  (cn \"a\" \"b\")\n");
    printf("  (= x y)  (< x y)     (str X)   (number? X)\n");
    printf("Structural forms (if/and/or/cond/let/lambda/defun) and calls to\n");
    printf("non-primitive bundled closures need the metacircular compile\n");
    printf("pipeline, which is not yet functional in the reduced C-VM bundle\n");
    printf("(the repo's open 'close the loop' item).\n");
    printf("Ctrl-D (EOF) to exit.\n\n");
    fflush(stdout);

    while (1) {
        printf("meta> "); fflush(stdout);
        char *line = read_stdin_line();
        if (!line) break;

        /* skip blank / whitespace-only lines */
        int only_ws = 1;
        for (char *p = line; *p; p++) if (!isspace((unsigned char)*p)) { only_ws = 0; break; }
        if (only_ws) { free(line); continue; }

        int n = (int)strlen(line);
        Value Str = val_string(line, n);
        Value Zero = val_number(0);
        Value Len = val_number((long)n);
        Value parsed = call_closure3("parse-exprs", Str, Zero, Len);
        if (parsed.tag != VAL_CONS || parsed.cons.car->tag != VAL_CONS) {
            printf("parse error\n"); free(line); continue;
        }
        Value exprs = *parsed.cons.car;  /* hd of [[Expr|Rest] FinalPos] */

        Value cur = exprs;
        while (cur.tag == VAL_CONS) {
            Value expr = *cur.cons.car;
            volatile int is_defun = is_defun_form(expr);

            CatchFrame cf;
            cf.parent = vm_catch_chain; cf.in_trap_error = 0;
            vm_catch_chain = &cf;
            volatile Value result; memset((void*)&result, 0, sizeof(result));
            result.tag = VAL_NIL;
            int err = 0;
            if (setjmp(cf.buf) == 0) {
                if (is_defun) {
                    /* register a defun in the Shen global-table via interp-eval.
                       NOTE: this requires the metacircular compile pipeline
                       (kl->zinc's non-primitive branch), which is not functional
                       in the reduced C-VM bundle yet (the "close the loop" open
                       item).  We report the outcome honestly rather than assume
                       success. */
                    Value r = call_closure1("interp-eval", expr);
                    result = r;
                } else {
                    /* evaluate an expression via eval-kl */
                    ValueArray s; va_init(&s);
                    va_push(&s, expr);
                    Value acc; memset(&acc, 0, sizeof(acc));
                    exec_primitive("eval-kl", &acc, &s);
                    va_free(&s);
                    result = acc;
                }
            } else {
                err = 1;
                result = cf.error_val;
            }
            vm_catch_chain = cf.parent;

            if (is_defun) {
                /* The defun form compiles to a [lambda ...] tagged closure; if
                   interp-eval succeeded, print the defun name it returns, else
                   report the registration error. */
                if (!err && result.tag == VAL_SYMBOL) {
                    printf("; registered ");
                    print_shen(result);
                } else {
                    printf("; defun registration failed: ");
                    print_shen(result);
                }
            } else {
                printf("=> ");
                print_shen(result);
            }
            cur = *cur.cons.cdr;
        }
        free(line);
    }
    printf("\nBye.\n");
}
#endif /* !ZINCTEST — meta-repl helpers are only used by zincvm main */

/* ------------------------------------------------------------------ */
/*  File reading                                                       */
/* ------------------------------------------------------------------ */

char *read_file_or_stdin(const char *path) {
    FILE *f = path ? fopen(path, "r") : stdin;
    if (!f) { fprintf(stderr, "error: cannot open '%s'\n", path); return NULL; }
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (len >= cap - 1) { cap *= 2; buf = realloc(buf, cap); }
        buf[len++] = (char)ch;
    }
    buf[len] = '\0';
    if (path) fclose(f);
    return buf;
}

/* Test runner functions (alarm_handler, run_test_timeout, run_test,
 * force_nursery_scavenge, gc_nursery_tests) moved to zinctest.c */

void init_globals(void) {
    const char *prims[] = {
        "+","-","*","/","=","<",">","<=",">=",
        "cons","hd","tl","cn","emptylist",
        "symbol?","boolean?","number?","string?","cons?",
        "error?","function?","stream?",
        "simple-error","trap-error","error-to-string",
        "eval-kl","absvector","<-address","address->",
        "n->string","string->n","str","tlstr","hdstr","pos",
        "intern","value","open","close","read-byte","write-byte",
        "set","get-time","read-file-as-string",
        "@p","fst","snd","gensym","variable?","newvar",
        "shen.fail!","fail",
        "stinput","stoutput", NULL
    };
    for (int i = 0; prims[i]; i++) global_set(prims[i], val_prim(prims[i]));
}

/* ------------------------------------------------------------------ */
/*  Bundle parser: load serialized closures into global table          */
/* ------------------------------------------------------------------ */

/*
 * Parse a bundle: ((name1 code1) (name2 code2) ...)
 * Each entry: (name_csexp code_csexp)
 *   where name_csexp is a csexp atom and code_csexp is a csexp list
 * Returns number of entries loaded (0 on error).
 */
int parse_bundle(const char *str) {
    ParseState ps;
    ps.p = str; ps.start = str;

    if (setjmp(parse_err_jmp)) {
        fprintf(stderr, "%s\n", parse_err_msg);
        return 0;
    }

    skip_ws(&ps);
    if (*ps.p != '(') {
        fprintf(stderr, "bundle error: expected outer '('\n");
        return 0;
    }
    ps.p++; /* skip '(' */

    int count = 0;
    while (1) {
        skip_ws(&ps);
        if (*ps.p == ')') { ps.p++; break; } /* end of bundle */
        if (*ps.p != '(') {
            fprintf(stderr, "bundle error: expected '(' for entry\n");
            return count;
        }
        ps.p++; /* skip '(' */

        /* Parse name atom */
        Value name_val = parse_csexp_atom(&ps);
        if (name_val.tag != VAL_SYMBOL) {
            fprintf(stderr, "bundle error: name must be a symbol\n");
            return count;
        }
        const char *name = name_val.sym.name;
        /* Keep full safe.* name -- primitives stay under short names */
        const char *key = name;


        /* Parse code list (a cur wrapping the closure body) */
        Instr *code = NULL;
        int code_len = parse_csexp_list(&ps, &code);
        if (code_len <= 0 || code == NULL) {
            fprintf(stderr, "bundle error: failed to parse code for '%s'\n", name);
            return count;
        }

        /* Unwrap the outer cur: use its closure_code as the lambda body */
        if (code_len < 1 || code[0].op != OP_CUR || code[0].closure_code == NULL) {
            fprintf(stderr, "bundle error: expected cur wrapper for '%s'\n", name);
            return count;
        }
        Instr *body_code = code[0].closure_code;
        int body_len = code[0].closure_len;

        /* Resolve jumps in the body */
        resolve_jumps(body_code, body_len);

        /* Create a closure from the body code (empty env) and store in globals */
        Value closure = val_lambda(body_code, body_len, NULL, 0);
        global_set(key, closure);
        /* code is GC-allocated — no free needed */

        /* Consume closing ')' of entry */
        skip_ws(&ps);
        if (*ps.p != ')') {
            fprintf(stderr, "bundle error: expected ')' to close entry '%s'\n", name);
            return count;
        }
        ps.p++;

        count++;
    }

    return count;
}

/* ------------------------------------------------------------------ */
/*  vm_load_bundle: shared bundle-bootstrap for zincvm and zinctest    */
/* ------------------------------------------------------------------ */

/* Parse a bundle string and set up the global-table environment
 * (keyword symbols, standard I/O streams, Shen global-table var).
 * Returns the number of closures loaded; 0 on error.
 * Does NOT free(buf) or call trace_resolve/gc_nursery_tests. */
int vm_load_bundle(const char *buf) {
    const char *p = buf;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '(' || *(p+1) != '(') {
        fprintf(stderr, "bundle error: not a bundle (expected ((...)))\n");
        return 0;
    }

    int n = parse_bundle(p);
    printf("Loaded %d closures into global table\n\n", n);
    fflush(stdout);

    /* Register ZINC pattern keywords as symbols. When zinc-c, zinc-t,
       normalize-term, debruijn etc. were self-compiled via set-toplevel,
       their patterns like [number X], [cons X Y], [lambda C E] etc.
       became bytecode with "global number", "global cons" etc. to
       obtain the tag symbol for structural matching at runtime.
       These must resolve to val_symbol, not val_prim or a closure. */
    const char *keywords[] = {
        "number", "symbol", "string", "boolean", "cons",
        "lambda", "function", "error", "absvector",
        "stream in", "stream out", "let", "if",
        "lookup", "freeze", "type", "defun", "define",
        "cond", "and", "or", "do", "fn",
        "list", "where",
        NULL
    };
    for (int i = 0; keywords[i]; i++)
        global_set(keywords[i], val_symbol(keywords[i]));

    /* Initialize standard I/O stream variables expected by the Shen OS.
       The bundled stinput/stoutput closures use (value *stinput*),
       (value *stoutput*) — these resolve to global_get("*stinput*") etc.
       shen.initialise-environment does NOT set them; the host port must. */
    {
        Value stin;  memset(&stin, 0, sizeof(stin));
        stin.tag = VAL_STREAM; stin.stream.file = stdin;  stin.stream.is_input = 1;
        global_set("*stinput*", stin);

        Value stout; memset(&stout, 0, sizeof(stout));
        stout.tag = VAL_STREAM; stout.stream.file = stdout; stout.stream.is_input = 0;
        global_set("*stoutput*", stout);

        Value sterr; memset(&sterr, 0, sizeof(sterr));
        sterr.tag = VAL_STREAM; sterr.stream.file = stderr; sterr.stream.is_input = 0;
        global_set("*sterror*", sterr);
    }

    /* Initialize the Shen global-table variable.  The metacircular
       interp's lookup-global reads (value global-table) to resolve
       non-primitive globals; it must start as an empty alist for
       interp-eval / set-toplevel to register new defuns at runtime. */
    global_set("global-table", val_nil());

    return n;
}

/* ------------------------------------------------------------------ */
/*  run_bytecode_file: execute a single csexp bytecode file             */
/* ------------------------------------------------------------------ */
#ifndef ZINCTEST
static void run_bytecode_file(const char *label, const char *src) {
    (void)label;  /* label is informational; not printed in production mode */
    Instr *code = NULL;
    int len = parse_bytecode(src, &code);
    if (len <= 0 || code == NULL) {
        printf("PARSE FAILED\n");
        return;
    }
    resolve_jumps(code, len);
    CatchFrame cf;
    cf.parent = vm_catch_chain;
    cf.in_trap_error = 0;
    vm_catch_chain = &cf;
    if (setjmp(cf.buf)) {
        vm_catch_chain = cf.parent;
        printf("ERROR: "); print_value(cf.error_val); printf("\n");
    } else {
        Value result = vm_exec(code, len);
        vm_catch_chain = cf.parent;
        print_value(result); printf("\n");
    }
}
#endif /* !ZINCTEST */

#ifndef ZINCTEST
int main(int argc, char **argv) {
    init_globals();
    {
        /* stack_base: a local in main, so the C-stack scan in collect()
         * knows where the root of the call stack is. */
        uintptr_t stack_base;
        gc_init(256UL * 1024 * 1024, &stack_base);
    }
    /* Register BSS/static data the GC must scan conservatively */
    gc_set_extra_roots(global_table, sizeof(global_table));
    gc_set_extra_roots(traced_code, sizeof(traced_code));
    gc_register_global_table(global_table, &global_table_len);
    gc_register_traced_code(traced_code, &num_traced);

    /* Scan for --trace <name> flags (before bundle load) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_add(argv[++i]);
        }
    }

    if (argc > 1) {
        char *buf = read_file_or_stdin(argv[1]);
        if (!buf) return 1;
        char *p = buf; while (*p && isspace((unsigned char)*p)) p++;

        /* Detect: if the second char (after '(') is '(' it's a bundle */
        if (*p == '(' && *(p+1) == '(') {
            /* Bundle format: ((name code) (name code) ...) */
            int n = vm_load_bundle(p);

            /* Verify heap integrity after bundle load */
            verify_heap();

            /* Resolve --trace function names to code pointers */
            if (num_traced > 0) trace_resolve();

            free(buf);
            if (n == 0) return 1;

            /* Find first non-flag arg after bundle (skip --trace pairs) */
            int ai = 2;
            while (ai < argc && strcmp(argv[ai], "--trace") == 0) ai += 2;

            /* -d <name>: decompile a bundled closure's bytecode */
            if (ai < argc && strcmp(argv[ai], "-d") == 0) {
                if (ai + 1 < argc) {
                    Value g = global_get(argv[ai + 1]);
                    if (g.tag == VAL_LAMBDA) {
                        printf("=== Decompile: %s ===\n", argv[ai + 1]);
                        printf("  code_len=%d  env_len=%d\n\n", g.lambda.code_len, g.lambda.env_len);
                        print_instr(g.lambda.code, g.lambda.code_len, 0);
                    } else if (g.tag == VAL_PRIM) {
                        printf("%s is a C primitive\n", argv[ai + 1]);
                    } else {
                        printf("%s: not found (tag=%d)\n", argv[ai + 1], g.tag);
                    }
                } else {
                    printf("Usage: %s <bundle> -d <function-name>\n", argv[0]);
                }
                return 0;
            }
            /* --meta-repl: run the meta-interpreter KLambda REPL
               (bypasses the Shen OS; uses bundled parse-exprs / eval-kl /
               interp-eval — all present in the reduced bundle). */
            if (ai < argc && strcmp(argv[ai], "--meta-repl") == 0) {
                meta_repl();
                return 0;
            }
            /* --repl: run the interactive Shen REPL */
            if (ai < argc && strcmp(argv[ai], "--repl") == 0) {
                printf("=== Shen REPL ===\n");
                fflush(stdout);

                Value init = global_get("shen.initialise");
                if (init.tag != VAL_LAMBDA) {
                    fprintf(stderr, "repl: shen.initialise not found (tag=%d)\n", init.tag);
                    return 1;
                }
                Value *env_init = GC_VALUE_ARRAY(init.lambda.env_len + 1);
                if (init.lambda.env_len > 0)
                    memcpy(env_init, init.lambda.env, init.lambda.env_len * sizeof(Value));
                env_init[init.lambda.env_len] = val_number(0);
                {
                    CatchFrame cf;
                    cf.parent = vm_catch_chain;
                    cf.in_trap_error = 0;
                    vm_catch_chain = &cf;
                    if (setjmp(cf.buf) == 0) {
                        vm_exec_env(init.lambda.code, init.lambda.code_len,
                                    env_init, init.lambda.env_len + 1);
                    }
                    vm_catch_chain = cf.parent;
                }
                printf("Shen ready.\n\n");
                fflush(stdout);

                Value repl = global_get("shen.repl");
                if (repl.tag != VAL_LAMBDA) {
                    fprintf(stderr, "repl: shen.repl not found\n");
                    return 1;
                }
                Value *env_repl = GC_VALUE_ARRAY(repl.lambda.env_len + 1);
                if (repl.lambda.env_len > 0)
                    memcpy(env_repl, repl.lambda.env, repl.lambda.env_len * sizeof(Value));
                env_repl[repl.lambda.env_len] = val_number(0);

                /* Set up REPL mode: intercept "error: empty stream" to
                   exit cleanly on EOF instead of looping forever. */
                repl_mode = 1;
                if (setjmp(repl_exit_jmp) == 0) {
                    CatchFrame cf;
                    cf.parent = vm_catch_chain;
                    cf.in_trap_error = 0;
                    vm_catch_chain = &cf;
                    if (setjmp(cf.buf) == 0) {
                        vm_exec_env(repl.lambda.code, repl.lambda.code_len,
                                    env_repl, repl.lambda.env_len + 1);
                    }
                    vm_catch_chain = cf.parent;
                }
                repl_mode = 0;

                printf("\nGoodbye.\n");
                return 0;
            }
            /* If another arg, run it as bytecode; otherwise show usage */
            if (ai < argc) {
                char *b2 = read_file_or_stdin(argv[ai]);
                if (b2) {
                    char *q = b2; while (*q && isspace((unsigned char)*q)) q++;
                    if (*q) run_bytecode_file(argv[ai], q);
                    free(b2);
                }
            } else {
                printf("Usage: %s <bundle> [--repl | --meta-repl | -d <name> | --trace <name>]\n",
                       argv[0]);
            }
        } else {
            /* Single bytecode list */
            if (*p) run_bytecode_file(argv[1], p); else printf("(empty file)\n");
            free(buf);
        }
        return 0;
    }

    printf("Usage: zincvm <bundle.csexp | bytecode.csexp> [--repl | --meta-repl | -d <name> | --trace <name>]\n");
    return 0;
}
#endif
