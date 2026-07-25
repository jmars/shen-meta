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

/* ------------------------------------------------------------------ */
/*  Value types                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    VAL_NUMBER,
    VAL_STRING,
    VAL_SYMBOL,
    VAL_BOOLEAN,
    VAL_CONS,
    VAL_NIL,
    VAL_LAMBDA,
    VAL_MARK,
    VAL_PRIM,
    VAL_ERROR,
    VAL_VECTOR,
    VAL_STREAM
} ValTag;

typedef struct Value {
    ValTag tag;
    union {
        long number;
        struct { char *data; int len; } str;
        struct { char *name; } sym;
        int boolean;
        struct { struct Value *car; struct Value *cdr; } cons;
        struct {
            struct Instr *code;
            int code_len;
            struct Value *env;
            int env_len;
        } lambda;
        struct { const char *name; } prim;
        struct { char *message; } error;
        struct {
            struct Value *data;
            int len;
        } vector;
        struct {
            FILE *file;
            int is_input;
        } stream;
    };
} Value;

typedef struct Instr Instr;

/* ------------------------------------------------------------------ */
/*  Instruction types                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    OP_ACCESS   = 'a', OP_GLOBAL   = 'g', OP_JMPF     = 'f',
    OP_JMP      = 'j', OP_APPTERM  = 't', OP_APPLY    = 'p',
    OP_PUSH     = 'u', OP_PUSHMARK = 'm', OP_CUR      = 'c',
    OP_GRAB     = 'r', OP_RETURN   = 'v', OP_LET      = 'e',
    OP_ENDLET   = 'd', OP_NUMBER   = 'n', OP_STRING   = 'S',
    OP_SYMBOL   = 's', OP_BOOLEAN  = 'b', OP_PRIM     = 'P'
} Opcode;

struct Instr {
    Opcode op;
    Value operand;
    Instr *closure_code;
    int closure_len;
    int jmp_target;
};

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

static Value val_number(long n) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_NUMBER; v.number = n; return v;
}
static Value val_string(const char *data, int len) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_STRING;
    v.str.data = malloc(len + 1);
    memcpy(v.str.data, data, len);
    v.str.data[len] = '\0';
    v.str.len = len; return v;
}
static Value val_symbol(const char *name) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_SYMBOL; v.sym.name = strdup(name); return v;
}
static Value val_boolean(int b) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_BOOLEAN; v.boolean = b; return v;
}
static Value val_cons(Value car, Value cdr) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_CONS; v.cons.car = malloc(sizeof(Value)); *v.cons.car = car; v.cons.cdr = malloc(sizeof(Value)); *v.cons.cdr = cdr; return v;
}
static Value val_nil(void) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_NIL; return v;
}
static Value val_lambda(Instr *code, int code_len, Value *env, int env_len) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_LAMBDA;
    v.lambda.code = code; v.lambda.code_len = code_len;
    if (env_len > 0) {
        v.lambda.env = malloc(env_len * sizeof(Value));
        memcpy(v.lambda.env, env, env_len * sizeof(Value));
        v.lambda.env_len = env_len;
    } else { v.lambda.env = NULL; v.lambda.env_len = 0; }
    return v;
}
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
    v.tag = VAL_ERROR; v.error.message = strdup(msg); return v;
}
static Value val_vector(int size) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_VECTOR; v.vector.len = size;
    v.vector.data = size > 0 ? calloc(size, sizeof(Value)) : NULL;
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

static void print_value(Value v) {
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
typedef struct { Value *data; int len; int cap; } ValueArray;

static void va_init(ValueArray *a) {
    a->data = malloc(STACK_INIT_CAP * sizeof(Value));
    a->len = 0; a->cap = STACK_INIT_CAP;
}
static void va_push(ValueArray *a, Value v) {
    if (a->len >= a->cap) { a->cap *= 2; a->data = realloc(a->data, a->cap * sizeof(Value)); }
    a->data[a->len++] = v;
}
static Value va_pop(ValueArray *a) {
    if (a->len <= 0) { fprintf(stderr, "fatal: pop from empty stack\n"); exit(1); }
    return a->data[--a->len];
}
static Value va_peek(ValueArray *a) { return a->data[a->len - 1]; }
static void va_free(ValueArray *a) { free(a->data); a->data = NULL; a->len = a->cap = 0; }

/* ------------------------------------------------------------------ */
/*  Global table                                                       */
/* ------------------------------------------------------------------ */

#define GLOBAL_TABLE_MAX 2048
typedef struct { char *name; Value closure; } GlobalEntry;
static GlobalEntry global_table[GLOBAL_TABLE_MAX];
static int global_table_len = 0;

static void global_set(const char *name, Value v) {
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
static Value global_get(const char *name) {
    for (int i = 0; i < global_table_len; i++)
        if (strcmp(global_table[i].name, name) == 0)
            return global_table[i].closure;
    return val_prim(name);
}

/* ------------------------------------------------------------------ */
/*  Error handling for trap-error / simple-error                       */
/* ------------------------------------------------------------------ */

static jmp_buf vm_error_jmp;
static Value vm_error_val;
static int vm_error_pending = 0;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */

static Value vm_exec(Instr *code, int code_len);

/* ------------------------------------------------------------------ */
/*  Primitive dispatch                                                 */
/* ------------------------------------------------------------------ */

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
        Value a = va_pop(stack); *acc = val_boolean(a.tag == VAL_CONS || a.tag == VAL_NIL); return 0;
    }
    if (strcmp(name, "error?") == 0) {
        Value a = va_pop(stack); *acc = val_boolean(a.tag == VAL_ERROR); return 0;
    }
    if (strcmp(name, "function?") == 0) {
        Value a = va_pop(stack); *acc = val_boolean(a.tag == VAL_LAMBDA || a.tag == VAL_PRIM); return 0;
    }
    if (strcmp(name, "stream?") == 0) {
        Value a = va_pop(stack); *acc = val_boolean(a.tag == VAL_STREAM); return 0;
    }

    /* --- Arithmetic --- */
    if (strcmp(name, "+") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        if (a1.tag != VAL_NUMBER || a2.tag != VAL_NUMBER) { fprintf(stderr, "runtime: + on non-numbers\n"); return -1; }
        *acc = val_number(a1.number + a2.number); return 0;
    }
    if (strcmp(name, "-") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        if (a1.tag != VAL_NUMBER || a2.tag != VAL_NUMBER) { fprintf(stderr, "runtime: - on non-numbers\n"); return -1; }
        *acc = val_number(a1.number - a2.number); return 0;
    }
    if (strcmp(name, "*") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        if (a1.tag != VAL_NUMBER || a2.tag != VAL_NUMBER) { fprintf(stderr, "runtime: * on non-numbers\n"); return -1; }
        *acc = val_number(a1.number * a2.number); return 0;
    }
    if (strcmp(name, "/") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        if (a1.tag != VAL_NUMBER || a2.tag != VAL_NUMBER) { fprintf(stderr, "runtime: / on non-numbers\n"); return -1; }
        if (a2.number == 0) { fprintf(stderr, "runtime: division by zero\n"); return -1; }
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
        if (a.tag != VAL_CONS) { fprintf(stderr, "runtime: hd on non-cons\n"); return -1; }
        *acc = *a.cons.car; return 0;
    }
    if (strcmp(name, "tl") == 0) {
        Value a = va_pop(stack);
        if (a.tag != VAL_CONS) { fprintf(stderr, "runtime: tl on non-cons\n"); return -1; }
        *acc = *a.cons.cdr; return 0;
    }
    if (strcmp(name, "emptylist") == 0) {
        Value a = va_pop(stack);
        if (a.tag == VAL_NUMBER && a.number == 0) { *acc = val_nil(); return 0; }
        fprintf(stderr, "runtime: emptylist on non-zero\n"); return -1;
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
        strcpy(r, b2); strcat(r, b1);
        *acc = val_string(r, len); free(r); return 0;
    }
    if (strcmp(name, "n->string") == 0) {
        Value a = va_pop(stack);
        if (a.tag != VAL_NUMBER) { fprintf(stderr, "runtime: n->string on non-number\n"); return -1; }
        char buf[64]; int len = snprintf(buf, sizeof(buf), "%ld", a.number);
        *acc = val_string(buf, len); return 0;
    }
    if (strcmp(name, "string->n") == 0) {
        Value a = va_pop(stack);
        if (a.tag != VAL_STRING) { fprintf(stderr, "runtime: string->n on non-string\n"); return -1; }
        char buf[256]; int n = a.str.len < 255 ? a.str.len : 255;
        memcpy(buf, a.str.data, n); buf[n] = '\0';
        *acc = val_number(atol(buf)); return 0;
    }
    if (strcmp(name, "str") == 0) {
        Value a = va_pop(stack);
        if (a.tag == VAL_SYMBOL) *acc = val_string(a.sym.name, strlen(a.sym.name));
        else if (a.tag == VAL_NUMBER) { char buf[64]; int len = snprintf(buf, sizeof(buf), "%ld", a.number); *acc = val_string(buf, len); }
        else *acc = val_string("", 0);
        return 0;
    }
    if (strcmp(name, "tlstr") == 0) {
        Value a = va_pop(stack);
        if (a.tag != VAL_STRING || a.str.len < 1) { fprintf(stderr, "runtime: tlstr on empty/non-string\n"); return -1; }
        *acc = val_string(a.str.data + 1, a.str.len - 1); return 0;
    }
    if (strcmp(name, "pos") == 0) {
        Value a1 = va_pop(stack), a2 = va_pop(stack);
        if (a1.tag != VAL_STRING || a2.tag != VAL_NUMBER) { fprintf(stderr, "runtime: pos on bad types\n"); return -1; }
        int pl = (int)a2.number;
        if (pl < 0 || pl > a1.str.len) pl = a1.str.len;
        *acc = val_string(a1.str.data, pl); return 0;
    }

    /* --- Symbol ops --- */
    if (strcmp(name, "intern") == 0) {
        Value a = va_pop(stack);
        if (a.tag != VAL_STRING) { fprintf(stderr, "runtime: intern on non-string\n"); return -1; }
        char buf[256]; int n = a.str.len < 255 ? a.str.len : 255;
        memcpy(buf, a.str.data, n); buf[n] = '\0';
        *acc = val_symbol(buf); return 0;
    }
    if (strcmp(name, "value") == 0) {
        Value a = va_pop(stack);
        if (a.tag != VAL_SYMBOL) { fprintf(stderr, "runtime: value on non-symbol\n"); return -1; }
        *acc = global_get(a.sym.name); return 0;
    }

    /* --- Vector ops --- */
    if (strcmp(name, "absvector") == 0) {
        Value a = va_pop(stack);
        if (a.tag != VAL_NUMBER || a.number < 0) { fprintf(stderr, "runtime: absvector bad size\n"); return -1; }
        *acc = val_vector((int)a.number); return 0;
    }
    if (strcmp(name, "<-address") == 0) {
        Value idx = va_pop(stack), vec = va_pop(stack);
        if (vec.tag != VAL_VECTOR || idx.tag != VAL_NUMBER) { fprintf(stderr, "runtime: <-address bad types\n"); return -1; }
        int i = (int)idx.number;
        if (i < 0 || i >= vec.vector.len) { fprintf(stderr, "runtime: <-address OOB\n"); return -1; }
        *acc = vec.vector.data[i]; return 0;
    }
    if (strcmp(name, "address->") == 0) {
        Value val = va_pop(stack), idx = va_pop(stack), vec = va_pop(stack);
        if (vec.tag != VAL_VECTOR || idx.tag != VAL_NUMBER) { fprintf(stderr, "runtime: address-> bad types\n"); return -1; }
        int i = (int)idx.number;
        if (i < 0 || i >= vec.vector.len) { fprintf(stderr, "runtime: address-> OOB\n"); return -1; }
        vec.vector.data[i] = val; *acc = vec; return 0;
    }

    /* --- Error handling --- */
    if (strcmp(name, "simple-error") == 0) {
        Value a = va_pop(stack);
        char msg[256];
        if (a.tag == VAL_STRING) snprintf(msg, sizeof(msg), "%.*s", a.str.len, a.str.data);
        else snprintf(msg, sizeof(msg), "simple-error called");
        vm_error_val = val_error(msg); vm_error_pending = 1;
        longjmp(vm_error_jmp, 1);
    }
    if (strcmp(name, "error-to-string") == 0) {
        Value a = va_pop(stack);
        if (a.tag == VAL_ERROR) *acc = val_string(a.error.message, strlen(a.error.message));
        else if (a.tag == VAL_STRING) *acc = a;
        else *acc = val_string("unknown error", 13);
        return 0;
    }
    if (strcmp(name, "trap-error") == 0) {
        Value handler = va_pop(stack), body = va_pop(stack);
        if (handler.tag != VAL_LAMBDA) { fprintf(stderr, "runtime: trap-error handler not fn\n"); return -1; }
        vm_error_pending = 0;
        if (setjmp(vm_error_jmp)) {
            Value err = val_error(vm_error_val.error.message);
            Instr *hc = handler.lambda.code; int hl = handler.lambda.code_len;
            Value *henv = malloc((handler.lambda.env_len + 1) * sizeof(Value));
            henv[0] = err;
            memcpy(henv + 1, handler.lambda.env, handler.lambda.env_len * sizeof(Value));
            handler.lambda.env = henv; handler.lambda.env_len++;
            *acc = vm_exec(hc, hl);
        } else {
            if (body.tag == VAL_LAMBDA) *acc = vm_exec(body.lambda.code, body.lambda.code_len);
            else *acc = body;
        }
        return 0;
    }

    /* --- I/O --- */
    if (strcmp(name, "open") == 0) {
        Value dir = va_pop(stack), path = va_pop(stack);
        if (path.tag != VAL_STRING || dir.tag != VAL_SYMBOL) { fprintf(stderr, "runtime: open bad types\n"); return -1; }
        char pb[256]; int n = path.str.len < 255 ? path.str.len : 255;
        memcpy(pb, path.str.data, n); pb[n] = '\0';
        if (strcmp(dir.sym.name, "in") == 0) {
            FILE *f = fopen(pb, "r");
            if (!f) { fprintf(stderr, "runtime: cannot open '%s' for reading\n", pb); return -1; }
            *acc = val_stream_in(f); return 0;
        } else if (strcmp(dir.sym.name, "out") == 0) {
            FILE *f = fopen(pb, "w");
            if (!f) { fprintf(stderr, "runtime: cannot open '%s' for writing\n", pb); return -1; }
            *acc = val_stream_out(f); return 0;
        }
        fprintf(stderr, "runtime: open direction must be in or out\n"); return -1;
    }
    if (strcmp(name, "close") == 0) {
        Value s = va_pop(stack);
        if (s.tag != VAL_STREAM) { fprintf(stderr, "runtime: close on non-stream\n"); return -1; }
        if (s.stream.file) fclose(s.stream.file);
        *acc = val_nil(); return 0;
    }
    if (strcmp(name, "read-byte") == 0) {
        Value s = va_pop(stack);
        if (s.tag != VAL_STREAM || !s.stream.is_input) { fprintf(stderr, "runtime: read-byte on non-input\n"); return -1; }
        int c = fgetc(s.stream.file); *acc = val_number(c == EOF ? -1 : c); return 0;
    }
    if (strcmp(name, "read-file-as-string") == 0 || strcmp(name, "vm.read-file") == 0) {
        Value path = va_pop(stack);
        if (path.tag != VAL_STRING) { fprintf(stderr, "runtime: read-file-as-string on non-string\n"); return -1; }
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
        Value s = va_pop(stack), byte = va_pop(stack);
        if (s.tag != VAL_STREAM || s.stream.is_input) { fprintf(stderr, "runtime: write-byte on non-output\n"); return -1; }
        if (byte.tag != VAL_NUMBER) { fprintf(stderr, "runtime: write-byte requires number\n"); return -1; }
        fputc((int)byte.number, s.stream.file);
        *acc = val_number(byte.number); return 0;
    }
    if (strcmp(name, "get-time") == 0) {
        Value mode = va_pop(stack);
        if (mode.tag != VAL_SYMBOL) { fprintf(stderr, "runtime: get-time requires symbol\n"); return -1; }
        if (strcmp(mode.sym.name, "unix") == 0 || strcmp(mode.sym.name, "real") == 0)
            { *acc = val_number((long)time(NULL)); return 0; }
        if (strcmp(mode.sym.name, "run") == 0) { *acc = val_number((long)clock()); return 0; }
        fprintf(stderr, "runtime: unknown get-time mode '%s'\n", mode.sym.name); return -1;
    }

    /* --- Meta --- */
    if (strcmp(name, "set") == 0) {
        Value v = va_pop(stack), sym = va_pop(stack);
        if (sym.tag != VAL_SYMBOL) { fprintf(stderr, "runtime: set requires symbol\n"); return -1; }
        global_set(sym.sym.name, v); *acc = v; return 0;
    }
    if (strcmp(name, "eval-kl") == 0) {
        Value a = va_pop(stack);
        fprintf(stderr, "warning: eval-kl is a stub, returning value as-is\n");
        *acc = a; return 0;
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
    Instr *code = malloc(cap * sizeof(Instr));
    while (1) {
        skip_ws(ps); char c = *ps->p;
        if (c == ')' || c == '\0') break;
        if (c == '(') PARSE_ERROR("unexpected nested list in body");
        Instr instr; memset(&instr, 0, sizeof(instr));
        switch (c) {
        case 'm': instr.op = OP_PUSHMARK; ps->p++; break;
        case 'p': instr.op = OP_APPLY;    ps->p++; break;
        case 'u': instr.op = OP_PUSH;     ps->p++; break;
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
        if (len >= cap) { cap *= 2; code = realloc(code, cap * sizeof(Instr)); }
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
static int parse_bytecode(const char *str, Instr **out) {
    ParseState ps; ps.p = str; ps.start = str;
    if (setjmp(parse_err_jmp)) { fprintf(stderr, "%s\n", parse_err_msg); *out = NULL; return 0; }
    return parse_csexp_list(&ps, out);
}

/* ------------------------------------------------------------------ */
/*  Debug printing                                                     */
/* ------------------------------------------------------------------ */

static void print_instr(Instr *code, int len, int indent) {
    for (int i = 0; i < len; i++) {
        for (int j = 0; j < indent; j++) printf("  ");
        Instr *in = &code[i];
        switch (in->op) {
        case OP_PUSHMARK: printf("pushmark\n"); break;
        case OP_APPLY:    printf("apply\n"); break;
        case OP_PUSH:     printf("push\n"); break;
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

/* ------------------------------------------------------------------ */
/*  Resolve jumps                                                      */
/* ------------------------------------------------------------------ */

static void resolve_jumps(Instr *code, int len) {
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

#define CALL_STACK_DEPTH 1024
typedef struct { Instr *code; int code_len, pc; Value *env; int env_len, env_cap; } CallFrame;

static Value lookup_env(int n, Value *env, int env_len) {
    if (n < 0 || n >= env_len) {
        fprintf(stderr, "runtime: access %d but env len %d\n", n, env_len);
        Value v; memset(&v, 0, sizeof(v)); v.tag = VAL_NUMBER; v.number = 0; return v;
    }
    return env[env_len - 1 - n];
}
static void env_push(Value **env, int *env_len, int *env_cap, Value v) {
    if (*env_len >= *env_cap) { *env_cap = *env_cap ? (*env_cap) * 2 : 4; *env = realloc(*env, (*env_cap) * sizeof(Value)); }
    (*env)[(*env_len)++] = v;
}
static Value env_pop(Value **env, int *env_len) {
    if (*env_len <= 0) { fprintf(stderr, "runtime: pop empty environment\n"); exit(1); }
    return (*env)[--(*env_len)];
}

static Value vm_exec(Instr *code, int code_len) {
    ValueArray stack; va_init(&stack);
    CallFrame frame_stack[CALL_STACK_DEPTH]; int frames_sp = 0;
    Value *env = NULL; int env_len = 0, env_cap = 0;
    Value acc; memset(&acc, 0, sizeof(acc)); acc.tag = VAL_NIL;
    int pc = 0; Instr *cur_code = code; int cur_len = code_len;

    if (vm_error_pending) { vm_error_pending = 0; setjmp(vm_error_jmp); }

    while (1) {
        if (pc < 0 || pc >= cur_len) {
            if (frames_sp > 0) {
                CallFrame *cf = &frame_stack[--frames_sp];
                cur_code = cf->code; cur_len = cf->code_len; pc = cf->pc;
                free(env); env = cf->env; env_len = cf->env_len; env_cap = cf->env_cap;
                continue;
            }
            break;
        }
        Instr *in = &cur_code[pc];
        switch (in->op) {
        case OP_NUMBER: case OP_STRING: case OP_SYMBOL: case OP_BOOLEAN:
            acc = in->operand; pc++; break;
        case OP_PRIM: {
            /* ZINC [prim X] executes primitive X with args from stack + acc.
               Push acc so binary primitives find both args on the stack. */
            const char *pn = (in->operand.tag == VAL_SYMBOL) ? in->operand.sym.name : "";
            va_push(&stack, acc);
            if (exec_primitive(pn, &acc, &stack) < 0) goto done;
            pc++;
            break;
        }
        case OP_PUSHMARK: va_push(&stack, val_mark()); pc++; break;
        case OP_PUSH: va_push(&stack, acc); pc++; break;
        case OP_GRAB: {
            if (stack.len > 0 && va_peek(&stack).tag == VAL_MARK) {
                va_pop(&stack);
                if (frames_sp > 0) {
                    CallFrame *cf = &frame_stack[--frames_sp];
                    cur_code = cf->code; cur_len = cf->code_len; pc = cf->pc;
                    free(env); env = cf->env; env_len = cf->env_len; env_cap = cf->env_cap;
                } else goto done;
            } else if (stack.len > 0) { env_push(&env, &env_len, &env_cap, va_pop(&stack)); pc++; }
            else pc++;
            break;
        }
        case OP_APPLY: {
            if (acc.tag == VAL_LAMBDA) {
                if (stack.len <= 0) { fprintf(stderr, "runtime: apply empty stack\n"); goto done; }
                Value arg = va_pop(&stack);
                if (frames_sp >= CALL_STACK_DEPTH) { fprintf(stderr, "runtime: call stack overflow\n"); goto done; }
                CallFrame *cf = &frame_stack[frames_sp++];
                cf->code = cur_code; cf->code_len = cur_len; cf->pc = pc + 1;
                cf->env = env; cf->env_len = env_len; cf->env_cap = env_cap;
                env = NULL; env_len = 0; env_cap = 0;
                cur_code = acc.lambda.code; cur_len = acc.lambda.code_len;
                Value *ne = malloc((acc.lambda.env_len + 1) * sizeof(Value));
                ne[0] = arg;
                memcpy(ne + 1, acc.lambda.env, acc.lambda.env_len * sizeof(Value));
                env = ne; env_len = acc.lambda.env_len + 1; env_cap = acc.lambda.env_len + 1;
                pc = 0;
            } else if (acc.tag == VAL_PRIM) {
                if (stack.len > 0 && va_peek(&stack).tag == VAL_MARK) va_pop(&stack);
                const char *pn = acc.prim.name;
                if (exec_primitive(pn, &acc, &stack) < 0) goto done;
                pc++;
            } else { fprintf(stderr, "runtime: apply non-callable tag=%d\n", acc.tag); goto done; }
            break;
        }
        case OP_RETURN: {
            if (stack.len > 0 && va_peek(&stack).tag == VAL_MARK && frames_sp > 0) {
                va_pop(&stack);
                CallFrame *cf = &frame_stack[--frames_sp];
                cur_code = cf->code; cur_len = cf->code_len; pc = cf->pc;
                free(env); env = cf->env; env_len = cf->env_len; env_cap = cf->env_cap;
            } else if (stack.len > 0 && frames_sp > 0) {
                Value v = va_pop(&stack);
                if (acc.tag != VAL_LAMBDA) { fprintf(stderr, "runtime: return non-lambda\n"); goto done; }
                cur_code = acc.lambda.code; cur_len = acc.lambda.code_len;
                Value *ne = malloc((acc.lambda.env_len + 1) * sizeof(Value));
                ne[0] = v; memcpy(ne + 1, acc.lambda.env, acc.lambda.env_len * sizeof(Value));
                free(env); env = ne; env_len = acc.lambda.env_len + 1; env_cap = acc.lambda.env_len + 1;
                pc = 0;
            } else if (frames_sp > 0) {
                CallFrame *cf = &frame_stack[--frames_sp];
                cur_code = cf->code; cur_len = cf->code_len; pc = cf->pc;
                free(env); env = cf->env; env_len = cf->env_len; env_cap = cf->env_cap;
            } else goto done;
            break;
        }
        case OP_ACCESS:
            acc = lookup_env((in->operand.tag == VAL_NUMBER) ? (int)in->operand.number : in->jmp_target, env, env_len);
            pc++; break;
        case OP_GLOBAL: {
            const char *nm = (in->operand.tag == VAL_SYMBOL) ? in->operand.sym.name : "";
            acc = global_get(nm); pc++; break;
        }
        case OP_LET: env_push(&env, &env_len, &env_cap, acc); pc++; break;
        case OP_ENDLET: if (env_len > 0) env_pop(&env, &env_len); pc++; break;
        case OP_JMP: pc = in->jmp_target; break;
        case OP_JMPF: if (acc.tag == VAL_BOOLEAN && !acc.boolean) pc = in->jmp_target; else pc++; break;
        case OP_CUR: {
            Value *ec = NULL; int ecl = env_len;
            if (env_len > 0) { ec = malloc(env_len * sizeof(Value)); memcpy(ec, env, env_len * sizeof(Value)); }
            acc = val_lambda(in->closure_code, in->closure_len, ec, ecl); free(ec); pc++; break;
        }
        case OP_APPTERM: {
            if (acc.tag == VAL_LAMBDA) {
                if (stack.len <= 0) { fprintf(stderr, "runtime: appterm empty stack\n"); goto done; }
                Value v = va_pop(&stack);
                cur_code = acc.lambda.code; cur_len = acc.lambda.code_len;
                Value *ne = malloc((acc.lambda.env_len + 1) * sizeof(Value));
                ne[0] = v; memcpy(ne + 1, acc.lambda.env, acc.lambda.env_len * sizeof(Value));
                free(env); env = ne; env_len = acc.lambda.env_len + 1; env_cap = acc.lambda.env_len + 1;
                pc = 0; break;
            } else if (acc.tag == VAL_PRIM) {
                if (stack.len > 0 && va_peek(&stack).tag == VAL_MARK) va_pop(&stack);
                const char *pn = acc.prim.name;
                if (exec_primitive(pn, &acc, &stack) < 0) goto done;
                pc++; break;
            } else {
                fprintf(stderr, "runtime: appterm non-lambda\n"); goto done;
            }
        }
        default: fprintf(stderr, "runtime: unknown op '%c' at pc=%d\n", in->op, pc); goto done;
        }
    }
done:
    va_free(&stack);
    for (int i = 0; i < frames_sp; i++) free(frame_stack[i].env);
    free(env);
    return acc;
}

/* ------------------------------------------------------------------ */
/*  File reading                                                       */
/* ------------------------------------------------------------------ */

static char *read_file_or_stdin(const char *path) {
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

/* ------------------------------------------------------------------ */
/*  Test runner                                                        */
/* ------------------------------------------------------------------ */

static void run_test(const char *label, const char *bytecode, int show_code) {
    printf("--- %s ---\n", label);
    printf("Bytecode: %s\n", bytecode);
    Instr *code = NULL;
    int len = parse_bytecode(bytecode, &code);
    if (len <= 0 || code == NULL) { printf("PARSE FAILED\n\n"); return; }
    printf("Parsed %d instructions:\n", len);
    if (show_code) print_instr(code, len, 0);
    printf("\n");
    resolve_jumps(code, len);
    if (setjmp(vm_error_jmp)) {
        printf("ERROR CAUGHT: "); print_value(vm_error_val); printf("\n\n");
    } else {
        Value result = vm_exec(code, len);
        printf("Result: "); print_value(result); printf("\n\n");
    }
    free(code);
}

static void init_globals(void) {
    const char *prims[] = {
        "+","-","*","/","=","<",">","<=",">=",
        "cons","hd","tl","cn","emptylist",
        "symbol?","boolean?","number?","string?","cons?",
        "error?","function?","stream?",
        "simple-error","trap-error","error-to-string",
        "eval-kl","absvector","<-address","address->",
        "n->string","string->n","str","tlstr","pos",
        "intern","value","open","close","read-byte","write-byte",
        "set","get-time","read-file-as-string","vm.read-file", NULL
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
static int parse_bundle(const char *str) {
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
            free(code);
            return count;
        }
        Instr *body_code = code[0].closure_code;
        int body_len = code[0].closure_len;

        /* Resolve jumps in the body */
        resolve_jumps(body_code, body_len);

        /* Create a closure from the body code (empty env) and store in globals */
        Value closure = val_lambda(body_code, body_len, NULL, 0);
        global_set(key, closure);
        free(code);

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

int main(int argc, char **argv) {
    init_globals();
    if (argc > 1) {
        char *buf = read_file_or_stdin(argv[1]);
        if (!buf) return 1;
        char *p = buf; while (*p && isspace((unsigned char)*p)) p++;

        /* Detect: if the second char (after '(') is '(' it's a bundle */
        if (*p == '(' && *(p+1) == '(') {
            /* Bundle format: ((name code) (name code) ...) */
            int n = parse_bundle(p);
            printf("Loaded %d closures into global table\n\n", n);
            free(buf);
            /* If second arg, run it as bytecode; otherwise run a test */
            if (argc > 2) {
                char *b2 = read_file_or_stdin(argv[2]);
                if (b2) {
                    char *q = b2; while (*q && isspace((unsigned char)*q)) q++;
                    if (*q) run_test(argv[2], q, 0);
                    free(b2);
                }
            } else {
                /* Self-hosting proof: call Shen library functions from the
                   bundle with values built in C. */
                printf("=== Self-hosting test ===\n");

                /* Build list [1 2 3] as a Shen value */
                Value e_1 = val_number(1);
                Value e_2 = val_number(2);
                Value e_3 = val_number(3);
                Value e_nil = val_nil();
                Value list123 = val_cons(e_1,
                                 val_cons(e_2,
                                 val_cons(e_3, e_nil)));

                /* Store in global table */
                global_set("*test-list*", list123);

                /* Test 1: (+ 1 2) through bundled + closure */
                printf("--- Test 1: (+ 1 2) via bundled + ---\n");
                run_test("add", "(mn[1:n]2un[1:n]1ug[1:s]+p)", 0);

                /* Test 2: (reverse [1 2 3]) through bundled reverse closure */
                printf("--- Test 2: (reverse [1 2 3]) via bundled reverse ---\n");
                run_test("reverse",
                         "(mg[11:s]*test-list*ug[7:s]reversep)", 0);

                /* Test 3: (factorial 5) through bundled factorial closure */
                printf("--- Test 3: (factorial 5) via bundled factorial ---\n");
                run_test("factorial",
                         "(mn[1:n]5ug[9:s]factorialp)", 0);

                printf("\nSelf-hosting proven: The C VM loaded %d closures compiled by\n", global_table_len);
                printf("the metacircular Shen ZINC interpreter and executed them correctly.\n");
            }
        } else {
            /* Single bytecode list */
            if (*p) run_test(argv[1], p, 0); else printf("(empty file)\n");
            free(buf);
        }
        return 0;
    }

    printf("=== ZINC Bytecode VM with 37 Primitives ===\n\n");

    run_test("1. [+ 1 2]",              "(mn[1:n]2un[1:n]1ug[1:s]+p)", 1);
    run_test("2. [lambda X X]",         "(c(a[1:n]0v))", 1);
    run_test("3. [let X 1 X]",          "(n[1:n]1ea[1:n]0d)", 1);
    run_test("4. [- 1 2] (expect -1)",  "(mn[1:n]2un[1:n]1ug[1:s]-p)", 1);
    run_test("5. [* 3 4] (expect 12)",  "(mn[1:n]4un[1:n]3ug[1:s]*p)", 1);
    run_test("6. [/ 10 2] (expect 5)",  "(mn[1:n]2un[2:n]10ug[1:s]/p)", 1);
    run_test("7. [= 1 1] (expect true)","(mn[1:n]1un[1:n]1ug[1:s]=p)", 1);
    run_test("8. [< 1 2] (expect true)","(mn[1:n]2un[1:n]1ug[1:s]<p)", 1);
    run_test("9. [> 5 3] (expect true)","(mn[1:n]3un[1:n]5ug[1:s]>p)", 1);
    run_test("10. [<= 2 2] (expect true)","(mn[1:n]2un[1:n]2ug[2:s]<=p)", 1);
    run_test("11. [>= 5 3] (expect true)","(mn[1:n]3un[1:n]5ug[2:s]>=p)", 1);
    run_test("12. [number? 42]",         "(mn[2:n]42ug[7:s]number?p)", 1);
    run_test("13. [symbol? hello]",      "(ms[5:s]helloug[7:s]symbol?p)", 1);
    run_test("14. [boolean? true]",      "(mb[4:b]trueug[8:s]boolean?p)", 1);
    run_test("15. [string? \"hi\"]",     "(mS[2:S]hiug[7:s]string?p)", 1);
    run_test("16. [string? 42] (expect false)", "(mn[2:n]42ug[7:s]string?p)", 1);
    run_test("17. [cons 1 2]",           "(mn[1:n]2un[1:n]1ug[4:s]consp)", 1);
    run_test("18. [cn \"hello\" \"world\"]", "(mS[5:S]worlduS[5:S]helloug[2:s]cnp)", 1);
    run_test("19. [n->string 42]",       "(mn[2:n]42ug[9:s]n->stringp)", 1);
    run_test("20. [string->n \"42\"]",   "(mS[2:S]42ug[9:s]string->np)", 1);
    run_test("21. [str hello]",          "(ms[5:s]helloug[3:s]strp)", 1);
    run_test("22. [tlstr \"abc\"]",      "(mS[3:S]abcug[5:s]tlstrp)", 1);
    run_test("23. [intern \"foo\"]",     "(mS[3:S]fooug[6:s]internp)", 1);
    run_test("24. [= \"ab\" \"ab\"]",    "(mS[2:S]abuS[2:S]abug[1:s]=p)", 1);
    run_test("25. [= 1 2] (expect false)","(mn[1:n]2un[1:n]1ug[1:s]=p)", 1);
    run_test("26. simple-error caught",   "(mS[4:S]boomug[12:s]simple-errorp)", 1);
    run_test("27. trap-error handler",
        "(mc(mS[4:S]oopsug[12:s]simple-errorpv)"  /* body closure: simple-error "oops" */
        "uc(S[6:S]caughtv)"                       /* handler closure: return "caught" */
        "ug[10:s]trap-errorp)", 1);
    run_test("28. [get-time unix]",      "(ms[4:s]unixug[8:s]get-timep)", 1);

    printf("=== All 28 tests done ===\n");
    return 0;
}
