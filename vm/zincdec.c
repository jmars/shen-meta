/*
 * zincdec.c — ZINC bytecode decompiler (standalone)
 *
 * Reads a globals.csexp bundle and decompiles individual closures.
 * Three output formats:
 *   --raw   (default)  Human-readable opcode names
 *   --asm              Disassembly with addresses
 *   --shen             Shen list syntax (feedable to interp.shen's interp)
 *
 * Usage: ./zincdec <bundle> <function-name> [--raw|--asm|--shen]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>

/* zincdec is GC-free: plain calloc/malloc.  It only parses bundles,
 * decompiles closures, and prints — no VM execution, no GC needed. */
#define GC_VALUE()        ((Value*)calloc(1, sizeof(Value)))
#define GC_STR(len)       ((char*)malloc((len) + 1))

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

typedef struct Instr Instr;

typedef struct Value {
    ValTag tag;
    union {
        long number;
        struct { char *data; int len; } str;
        struct { char *name; } sym;
        int boolean;
        struct { struct Value *car; struct Value *cdr; } cons;
        struct {
            Instr *code;
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
            int is_string;
        } stream;
    };
} Value;

/* ------------------------------------------------------------------ */
/*  Opcodes & Instr                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    OP_ACCESS   = 'a', OP_GLOBAL   = 'g', OP_JMPF     = 'f',
    OP_JMP      = 'j', OP_APPTERM  = 't', OP_APPLY    = 'p',
    OP_PUSHMARK = 'm', OP_CUR      = 'c',
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

/* --- Value helpers --- */
static Value val_number(long n) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_NUMBER; v.number = n; return v;
}
static Value val_string(const char *data, int len) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_STRING;
    v.str.data = GC_STR(len);
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
static Value val_lambda(Instr *code, int code_len, Value *env, int env_len) {
    /* env arrays are GC-allocated via gcalloc so GC traces captured
       Values when the closure is reachable (e.g. via global_table). */
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_LAMBDA;
    v.lambda.code = code; v.lambda.code_len = code_len;
    if (env_len > 0) {
        v.lambda.env = (Value*)calloc(env_len, sizeof(Value));
        memset(v.lambda.env, 0, env_len * sizeof(Value));
        memcpy(v.lambda.env, env, env_len * sizeof(Value));
        v.lambda.env_len = env_len;
    } else { v.lambda.env = NULL; v.lambda.env_len = 0; }
    return v;
}
static Value val_prim(const char *name) {
    Value v; memset(&v, 0, sizeof(v));
    v.tag = VAL_PRIM; v.prim.name = name; return v;
}


/* --- print_value --- */
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

/* --- Global table --- */
#define GLOBAL_TABLE_MAX 2048
typedef struct { char *name; Value closure; } GlobalEntry;

/* GC scans global_table as raw uintptr_t words (conservative scan).
   These assertions ensure pointer fields are at aligned offsets and
   the struct size is a word multiple — breaking either would cause
   the GC to silently miss pointers. */
_Static_assert(sizeof(GlobalEntry) % sizeof(uintptr_t) == 0,
               "GlobalEntry must be word-multiple for GC scan");
_Static_assert(_Alignof(GlobalEntry) >= sizeof(uintptr_t),
               "GlobalEntry must be word-aligned for GC scan");

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

/* --- Parse helpers --- */
static void skip_ws(ParseState *ps) {
    while (isspace((unsigned char)*ps->p)) ps->p++;
}
static int parse_int(ParseState *ps) {
    int n = 0;
    if (!isdigit((unsigned char)*ps->p)) PARSE_ERROR("expected digit");
    while (isdigit((unsigned char)*ps->p)) { n = n * 10 + (*ps->p - '0'); ps->p++; }
    return n;
}

/* --- csexp parser --- */
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

/* --- resolve_jumps --- */
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

/* --- read_file_or_stdin --- */
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

/* --- init_globals --- */
static void init_globals(void) {
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

/* --- parse_bundle --- */
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



/* ------------------------------------------------------------------ */
/*  Value → Shen emitter                                              */
/* ------------------------------------------------------------------ */

static void emit_shen_value(Value v) {
    switch (v.tag) {
    case VAL_NUMBER: printf("%ld", v.number); break;
    case VAL_STRING: printf("\"%.*s\"", v.str.len, v.str.data); break;
    case VAL_SYMBOL: printf("%s", v.sym.name); break;
    case VAL_BOOLEAN: printf(v.boolean ? "true" : "false"); break;
    case VAL_NIL: printf("[]"); break;
    default: printf("???");
    }
}

/* ------------------------------------------------------------------ */
/*  Decompile: raw format                                              */
/* ------------------------------------------------------------------ */

static void decompile_raw(Instr *code, int len, int indent) {
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
        case OP_JMPF:     printf("jmpf "); print_value(in->operand);
                          printf(" (tgt=%d)\n", in->jmp_target); break;
        case OP_JMP:      printf("jmp ");  print_value(in->operand);
                          printf(" (tgt=%d)\n", in->jmp_target); break;
        case OP_NUMBER:   printf("number "); print_value(in->operand); printf("\n"); break;
        case OP_STRING:   printf("string "); print_value(in->operand); printf("\n"); break;
        case OP_SYMBOL:   printf("symbol "); print_value(in->operand); printf("\n"); break;
        case OP_BOOLEAN:  printf("boolean "); print_value(in->operand); printf("\n"); break;
        case OP_PRIM:     printf("prim "); print_value(in->operand); printf("\n"); break;
        case OP_CUR:
            printf("cur (code=%d):\n", in->closure_len);
            decompile_raw(in->closure_code, in->closure_len, indent + 1);
            for (int j = 0; j < indent; j++) printf("  ");
            printf("endcur\n");
            break;
        default: printf("??? (op=%c)\n", in->op);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Decompile: asm format                                              */
/* ------------------------------------------------------------------ */

static void decompile_asm(Instr *code, int len, int base_addr, int indent) {
    int addr = base_addr;
    for (int i = 0; i < len; i++) {
        Instr *in = &code[i];
        for (int j = 0; j < indent; j++) printf("  ");

        int tgt_addr = -1;
        if (in->op == OP_JMP || in->op == OP_JMPF)
            tgt_addr = base_addr + in->jmp_target;

        printf("%04x: ", addr);
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
        case OP_JMPF:
            printf("jmpf "); print_value(in->operand);
            printf("  ; -> %04x\n", tgt_addr); break;
        case OP_JMP:
            printf("jmp "); print_value(in->operand);
            printf("   ; -> %04x\n", tgt_addr); break;
        case OP_NUMBER:   printf("number "); print_value(in->operand); printf("\n"); break;
        case OP_STRING:   printf("string "); print_value(in->operand); printf("\n"); break;
        case OP_SYMBOL:   printf("symbol "); print_value(in->operand); printf("\n"); break;
        case OP_BOOLEAN:  printf("boolean "); print_value(in->operand); printf("\n"); break;
        case OP_PRIM:     printf("prim "); print_value(in->operand); printf("\n"); break;
        case OP_CUR:
            printf("cur (code=%d):\n", in->closure_len);
            decompile_asm(in->closure_code, in->closure_len, 0, indent + 1);
            for (int j = 0; j < indent; j++) printf("  ");
            printf("      endcur\n");
            break;
        default: printf("??? (op=%c)\n", in->op);
        }
        addr++;
    }
}

/* ------------------------------------------------------------------ */
/*  Decompile: shen format                                             */
/* ------------------------------------------------------------------ */

static void decompile_shen_instr(Instr *in, int indent) {
    for (int j = 0; j < indent; j++) printf("  ");
    switch (in->op) {
    case OP_PUSHMARK: printf("pushmark\n"); break;
    case OP_APPLY:    printf("apply\n"); break;
    case OP_GRAB:     printf("grab\n"); break;
    case OP_RETURN:   printf("return\n"); break;
    case OP_LET:      printf("let\n"); break;
    case OP_ENDLET:   printf("endlet\n"); break;
    case OP_APPTERM:  printf("appterm\n"); break;
    case OP_ACCESS:   printf("[access "); emit_shen_value(in->operand); printf("]\n"); break;
    case OP_GLOBAL:   printf("[global "); emit_shen_value(in->operand); printf("]\n"); break;
    case OP_JMPF:     printf("[jmpf "); emit_shen_value(in->operand); printf("]\n"); break;
    case OP_JMP:      printf("[jmp ");  emit_shen_value(in->operand); printf("]\n"); break;
    case OP_NUMBER:   printf("[number "); emit_shen_value(in->operand); printf("]\n"); break;
    case OP_STRING:   printf("[string "); emit_shen_value(in->operand); printf("]\n"); break;
    case OP_SYMBOL:   printf("[symbol "); emit_shen_value(in->operand); printf("]\n"); break;
    case OP_BOOLEAN:  printf("[boolean "); emit_shen_value(in->operand); printf("]\n"); break;
    case OP_PRIM:     printf("[prim "); emit_shen_value(in->operand); printf("]\n"); break;
    case OP_CUR:
        printf("[cur\n");
        for (int k = 0; k < in->closure_len; k++)
            decompile_shen_instr(&in->closure_code[k], indent + 1);
        for (int j = 0; j < indent; j++) printf("  ");
        printf("]\n");
        break;
    default: printf("[??? %c]\n", in->op);
    }
}

static void decompile_shen(Instr *code, int len) {
    printf("[\n");
    for (int i = 0; i < len; i++)
        decompile_shen_instr(&code[i], 1);
    printf("]\n");
}

/* ------------------------------------------------------------------ */
/*  Decompile: csexp format (raw wire format the parser reads)         */
/* ------------------------------------------------------------------ */

/* Emit a csexp operand: [len:type]value */
static void emit_csexp_operand(Value v) {
    switch (v.tag) {
    case VAL_NUMBER: {
        char buf[32]; int n = snprintf(buf, sizeof(buf), "%ld", v.number);
        printf("[%d:n]%s", n, buf); break;
    }
    case VAL_STRING:
        printf("[%d:S]%.*s", v.str.len, v.str.len, v.str.data); break;
    case VAL_SYMBOL:
        printf("[%d:s]%s", (int)strlen(v.sym.name), v.sym.name); break;
    case VAL_BOOLEAN: {
        const char *s = v.boolean ? "true" : "false";
        printf("[%d:b]%s", (int)strlen(s), s); break;
    }
    default: printf("[0:n]0"); break;
    }
}

static void decompile_csexp_instr(Instr *in) {
    switch (in->op) {
    case OP_PUSHMARK: printf("m"); break;
    case OP_APPLY:    printf("p"); break;
    case OP_GRAB:     printf("r"); break;
    case OP_RETURN:   printf("v"); break;
    case OP_LET:      printf("e"); break;
    case OP_ENDLET:   printf("d"); break;
    case OP_APPTERM:  printf("t"); break;
    case OP_ACCESS:   printf("a"); emit_csexp_operand(in->operand); break;
    case OP_GLOBAL:   printf("g"); emit_csexp_operand(in->operand); break;
    case OP_JMPF:     printf("f"); emit_csexp_operand(in->operand); break;
    case OP_JMP:      printf("j"); emit_csexp_operand(in->operand); break;
    case OP_NUMBER:   printf("n"); emit_csexp_operand(in->operand); break;
    case OP_STRING:   printf("S"); emit_csexp_operand(in->operand); break;
    case OP_SYMBOL:   printf("s"); emit_csexp_operand(in->operand); break;
    case OP_BOOLEAN:  printf("b"); emit_csexp_operand(in->operand); break;
    case OP_PRIM:     printf("P"); emit_csexp_operand(in->operand); break;
    case OP_CUR:
        printf("c(");
        for (int k = 0; k < in->closure_len; k++)
            decompile_csexp_instr(&in->closure_code[k]);
        printf(")"); break;
    default: printf("?"); break;
    }
}

static void decompile_csexp(Instr *code, int len) {
    printf("(");
    for (int i = 0; i < len; i++)
        decompile_csexp_instr(&code[i]);
    printf(")\n");
}

/* ------------------------------------------------------------------ */
/*  Curried-call detector                                              */
/* ------------------------------------------------------------------ */

/* A curried partial-application call ((f x) y) compiles to ZINC where the
 * inner and outer applies land ADJACENT in the linear instruction stream:
 *   m y m x f p p
 * Full-arity code always has argument computation (or a mark) between two
 * applies, so two consecutive apply/appterm opcodes are the hallmark of a
 * curried call the C VM cannot run.  Returns the number of curried pairs
 * found; recurses into OP_CUR sub-closures. */
static int scan_curried(Instr *code, int len, const char *name,
                        int *prev_was_apply, int *out_count) {
    int local_prev = (prev_was_apply ? *prev_was_apply : 0);
    int found = 0;
    for (int i = 0; i < len; i++) {
        Instr *in = &code[i];
        switch (in->op) {
        case OP_APPLY:
        case OP_APPTERM:
            if (local_prev) {
                fprintf(stderr, "  %s: curried call at instr %d (%c after %c)\n",
                        name, i, in->op,
                        (i > 0 ? code[i-1].op : '?'));
                found++;
            }
            local_prev = 1;
            break;
        case OP_CUR:
            /* sub-closures reset adjacency for their own stream, but a
               closure-returning sub-apply is still a curried candidate */
            found += scan_curried(in->closure_code, in->closure_len, name,
                                  NULL, NULL);
            local_prev = 0;
            break;
        default:
            local_prev = 0;
            break;
        }
    }
    if (out_count) *out_count = found;
    return found;
}

static int scan_all_curried(void) {
    int total = 0, closures_with = 0, scanned = 0;
    for (int i = 0; i < global_table_len; i++) {
        Value v = global_table[i].closure;
        if (v.tag != VAL_LAMBDA) continue;
        scanned++;
        int cnt = 0;
        scan_curried(v.lambda.code, v.lambda.code_len,
                     global_table[i].name, NULL, &cnt);
        if (cnt) { total += cnt; closures_with++; }
    }
    fprintf(stderr, "Scanned %d closures: %d curried call(s) in %d closure(s)\n",
            scanned, total, closures_with);
    return total;
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s <bundle> <function-name> [--raw|--asm|--shen|--csexp]\n"
        "       %s <bundle> --curried\n"
        "\n"
        "Decompile a bundled closure's ZINC bytecode, or scan the whole bundle.\n"
        "\n"
        "Output formats:\n"
        "  --raw   (default) Human-readable opcode names with operands\n"
        "  --asm             Disassembly listing with hex addresses\n"
        "  --shen            Shen list syntax for interp.shen's interp\n"
        "  --csexp           Raw csexp wire format (feedable to parse_bytecode)\n"
        "\n"
        "Whole-bundle modes:\n"
        "  --curried         Flag any curried partial-application calls (the C VM\n"
        "                    cannot run them); exit 1 if any found\n"
        "\n"
        "Examples:\n"
        "  %s globals.csexp +\n"
        "  %s globals.csexp read-from-string --asm\n"
        "  %s globals.csexp shen.repl --shen\n"
        "  %s globals.csexp reverse --csexp\n"
        "  %s globals.csexp --curried\n",
        prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    init_globals();

    if (argc < 3) { usage(argv[0]); return 1; }

    const char *bundle_path = argv[1];
    const char *func_name = argv[2];
    const char *format = "--raw";

    if (argc > 3) {
        format = argv[3];
        if (strcmp(format, "--raw") && strcmp(format, "--asm") &&
            strcmp(format, "--shen") && strcmp(format, "--csexp")) {
            fprintf(stderr, "error: unknown format '%s'\n", format);
            usage(argv[0]); return 1;
        }
    }

    char *buf = read_file_or_stdin(bundle_path);
    if (!buf) return 1;
    char *p = buf; while (*p && isspace((unsigned char)*p)) p++;

    if (!(*p == '(' && *(p+1) == '(')) {
        fprintf(stderr, "error: '%s' not a bundle\n", bundle_path);
        free(buf); return 1;
    }

    int n = parse_bundle(p);
    free(buf);
    if (n <= 0) { fprintf(stderr, "error: parse failed\n"); return 1; }
    fprintf(stderr, "Loaded %d closures\n", n);

    /* --curried: scan all closures for curried partial-application calls */
    if (!strcmp(func_name, "--curried")) {
        return scan_all_curried() ? 1 : 0;
    }

    /* Pattern keywords as symbols */
    { const char *kws[] = {"number","string","symbol","cons","nil","boolean",
        "lambda","function","prim","vector","stream",
        "true","false","error","absvector","unit", NULL};
      for (int i = 0; kws[i]; i++) global_set(kws[i], val_symbol(kws[i])); }

    Value g = global_get(func_name);
    if (g.tag == VAL_LAMBDA) {
        int fmt_shen  = !strcmp(format, "--shen");
        int fmt_asm   = !strcmp(format, "--asm");
        int fmt_csexp = !strcmp(format, "--csexp");
        if (!fmt_shen && !fmt_csexp) printf("=== %s ===\n  code_len=%d  env_len=%d\n\n",
                          func_name, g.lambda.code_len, g.lambda.env_len);
        if (fmt_asm)         decompile_asm(g.lambda.code, g.lambda.code_len, 0, 0);
        else if (fmt_shen)   decompile_shen(g.lambda.code, g.lambda.code_len);
        else if (fmt_csexp)  decompile_csexp(g.lambda.code, g.lambda.code_len);
        else                 decompile_raw(g.lambda.code, g.lambda.code_len, 0);
    } else if (g.tag == VAL_PRIM) {
        printf("%s is a C primitive\n", func_name);
    } else {
        fprintf(stderr, "%s: not found (tag=%d)\n", func_name, g.tag);
        return 1;
    }
    return 0;
}
