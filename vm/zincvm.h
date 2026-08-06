/*
 * zincvm.h — shared types, macros, externs, and prototypes for zincvm.c and
 *            zinctest.c.  Split out of zincvm.c to enable a separate test binary.
 */

#ifndef ZINCVM_VM_H
#define ZINCVM_VM_H

#include <setjmp.h>
#include <stdint.h>
#include "gc.h"

/* ------------------------------------------------------------------ */
/*  GC allocation helpers                                              */
/* ------------------------------------------------------------------ */

#define GC_VALUE()            ((Value*)gc_alloc(sizeof(Value), GC_TYPE_VALUE))
#define GC_STR(len)           ((char*)gc_alloc_atomic((len) + 1))
#define GC_VALUE_ARRAY(n)     ((Value*)gc_alloc((n) * sizeof(Value), GC_TYPE_VALUE_ARRAY))

/* ------------------------------------------------------------------ */
/*  Heap verification (no-op in production)                            */
/* ------------------------------------------------------------------ */

#define verify_heap() ((void)0)

/* ------------------------------------------------------------------ */
/*  Global table                                                       */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  Error handling: CatchFrame                                         */
/* ------------------------------------------------------------------ */

/* Per-catch-site linked list of stack-allocated catch frames.
   Replaces the global jmp_buf + manual save/restore stack design.
   Each catch site declares a local CatchFrame, links it to the chain,
   and setjmps into cf.buf.  On error, vm_throw writes the error value
   and longjmps to the chain head.  The frame is unlinked FIRST on the
   error path so a simple-error raised inside a handler propagates to
   the ENCLOSING frame, not back to itself. */
typedef struct CatchFrame {
    jmp_buf          buf;
    Value            error_val;
    int              in_trap_error;   /* 1 while running a trap-error BODY */
    struct CatchFrame *parent;
} CatchFrame;

/* ------------------------------------------------------------------ */
/*  Extern globals                                                     */
/* ------------------------------------------------------------------ */

extern CatchFrame *vm_catch_chain;
extern GlobalEntry global_table[GLOBAL_TABLE_MAX];
extern int global_table_len;
extern int trace_counter;
extern int trace_limit;
extern int num_traced;
#define MAX_TRACED 32
extern Instr *traced_code[MAX_TRACED];

/* ------------------------------------------------------------------ */
/*  Value constructors                                                 */
/* ------------------------------------------------------------------ */

Value val_number(long n);
Value val_string(const char *data, int len);
Value val_string_from(Value *src_slot, int off, int len);
Value val_symbol(const char *name);
Value val_boolean(int b);
Value val_cons(Value car, Value cdr);
Value val_nil(void);
Value val_lambda(Instr *code, int code_len, Value *env, int env_len);
Value val_vector(int size);

/* ------------------------------------------------------------------ */
/*  Debug printing                                                     */
/* ------------------------------------------------------------------ */

void print_value(Value v);
void print_instr(Instr *code, int len, int indent);

/* ------------------------------------------------------------------ */
/*  Parser / jump resolution                                           */
/* ------------------------------------------------------------------ */

int  parse_bytecode(const char *str, Instr **out);
void resolve_jumps(Instr *code, int len);

/* ------------------------------------------------------------------ */
/*  VM execution                                                       */
/* ------------------------------------------------------------------ */

Value vm_exec(Instr *code, int code_len);
Value vm_exec_env(Instr *code, int code_len, Value *init_env, int init_env_len);

/* ------------------------------------------------------------------ */
/*  Global table                                                       */
/* ------------------------------------------------------------------ */

void  global_set(const char *name, Value v);
Value global_get(const char *name);

/* ------------------------------------------------------------------ */
/*  Tracing                                                            */
/* ------------------------------------------------------------------ */

void trace_add(const char *name);
void trace_resolve(void);

/* ------------------------------------------------------------------ */
/*  Initialization / bundle loading                                    */
/* ------------------------------------------------------------------ */

void init_globals(void);
int  parse_bundle(const char *str);
char *read_file_or_stdin(const char *path);

/* vm_load_bundle: parse a bundle string, register ZINC pattern keywords
 * as symbols, set up stdin/stdout/stderr stream variables, and initialise the
 * Shen global-table variable.  Returns the number of closures loaded.
 * Does NOT free(buf) or call trace_resolve/gc_nursery_tests — those are
 * the caller's responsibility. */
int vm_load_bundle(const char *buf);

#endif /* ZINCVM_VM_H */
