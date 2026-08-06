/*
 * zinctypes.h — shared type definitions for the ZINC VM and GC
 *
 * Included by both zincvm.c and gc.c so the collector can scan
 * typed objects (Value, Instr arrays, CallFrame arrays).
 */

#ifndef ZINCVM_TYPES_H
#define ZINCVM_TYPES_H

#include <stdio.h>
#include <stdint.h>

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
            FILE *file;         /* NULL for string streams */
            int is_input;
            int is_string;      /* 1 = string-backed, 0 = FILE-backed */
        } stream;
    };
} Value;

/* ------------------------------------------------------------------ */
/*  Instruction types                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    OP_ACCESS   = 'a', OP_GLOBAL   = 'g', OP_JMPF     = 'f',
    OP_JMP      = 'j', OP_APPTERM  = 't', OP_APPLY    = 'p',
    OP_PUSHMARK = 'm', OP_CUR      = 'c',
    OP_GRAB     = 'r', OP_RETURN   = 'v', OP_LET      = 'e',
    OP_ENDLET   = 'd', OP_NUMBER   = 'n', OP_STRING   = 'S',
    OP_SYMBOL   = 's', OP_BOOLEAN  = 'b', OP_PRIM     = 'P'
} Opcode;

typedef struct Instr {
    Opcode op;
    Value operand;
    struct Instr *closure_code;
    int closure_len;
    int jmp_target;
} Instr;

/* ------------------------------------------------------------------ */
/*  Call frame (for GC scanning)                                       */
/* ------------------------------------------------------------------ */

#define CALL_STACK_DEPTH 65536
typedef struct { Value *data; int len; int cap; } ValueArray;

typedef struct {
    Instr *code;
    int code_len;
    int pc;
    Value *env;
    int env_len;
    int env_cap;
    ValueArray stack;
} CallFrame;

/* ---- load-bearing size classes (Phase 3/4 BiBOP) ----
 * These MUST stay true: the collector's size-class routing (if/when added)
 * assumes fixed strides.  Build-verified: Value=40, Instr=64, CallFrame=48. */
_Static_assert(sizeof(Value) == 40,   "Value size class is 40B");
_Static_assert(sizeof(Instr) == 64,   "Instr size class is 64B");
_Static_assert(sizeof(CallFrame) == 48, "CallFrame size class is 48B");
_Static_assert(sizeof(ValueArray) == 16, "ValueArray is 16B");
_Static_assert(sizeof(uintptr_t) == 8, "Phase 3/4 assumes LP64");

#endif /* ZINCVM_TYPES_H */
