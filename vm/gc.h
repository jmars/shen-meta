/*
 * gc.h — Cheney mostly-copying collector API (Phase 1: single-space)
 *
 * Objects carry a type tag in the header so the scavenger knows how to
 * scan them.  The type tags are defined here; the scanning functions
 * (gc_scan_value, gc_evacuate) are implemented in zincvm.c because they
 * need the full Value/Instr/CallFrame type definitions.
 */

#ifndef ZINCVM_GC_H
#define ZINCVM_GC_H

#include <stdint.h>
#include <stddef.h>

/* ---- type tags stored in the header's repurposed "ptrs" field ---- */
enum {
    GC_TYPE_RAW             = 0,  /* char[] string/error data — no scan */
    GC_TYPE_VALUE           = 1,  /* single Value struct — scan by tag */
    GC_TYPE_VALUE_ARRAY     = 2,  /* Value[] — scan each by tag */
    GC_TYPE_INSTR_ARRAY     = 3,  /* Instr[] — scan operand + closure_code */
    GC_TYPE_CALLFRAME_ARRAY = 4   /* CallFrame[] — scan env + stack.data */
};

/* ---- public API ---- */

/* Initialise the collector.  heap_size is in bytes (must be a multiple of
 * PAGEBYTES=512). */
void  gc_init(uintptr_t heap_size);

/* Allocate zeroed memory that may contain GC-managed pointers.  `type_tag`
 * tells the collector how to scan the object's body.  May trigger a
 * collection.  Marked noinline so callers' live registers spill to stack. */
void *gc_alloc(size_t bytes, int type_tag);

/* Allocate directly in old-gen, bypassing the nursery bump allocator.
 * For large objects (frame_stack, big arrays) that would never fit in
 * the nursery and whose allocation would waste nursery space. */
void *gc_alloc_oldgen(size_t bytes, int type_tag);

/* Shorthand for gc_alloc(bytes, GC_TYPE_RAW).  For char buffers, error
 * messages, and other blobs that contain no GC-managed pointers. */
void *gc_alloc_atomic(size_t bytes);

/* ---- functions implemented in zincvm.c (need full type defs) ---- */

#include "zinctypes.h"

/* Evacuate a single pointer from from-space to to-space.
 * Called from the scavenger for each pointer field. */
void gc_evacuate(void **slot);

/* Scan a Value and evacuate all GC-managed pointers it contains.
 * Called from the scavenger for GC_TYPE_VALUE and GC_TYPE_VALUE_ARRAY. */
void gc_scan_value(Value *v);

/* Evacuate a from-space object to to-space.  Returns the new address.
 * Used by gc_evacuate.  NOT called directly by user code. */
void *gc_move(void *p);

/* Nursery predicate: true iff p points into the nursery region.
 * Used by the write barrier to detect old→nursery pointer stores. */
int gc_in_nursery(void *p);

/* Old-gen predicate: true iff p's page is in the live old-gen semi-space
 * (space == current_space).  Note this tests the space tag, not the address
 * range: under pin-in-place a promoted nursery page keeps its nursery-range
 * address but its space becomes current_space.  Used by the write barrier. */
int gc_in_oldgen(void *p);

/* Write-barrier remembered set: records old-gen vector element arrays
 * that may now contain nursery pointers after an address-> store.
 * Cleared at the end of each nursery scavenge and when a full collect
 * flips the semi-spaces (which moves the objects anyway). */
void gc_dirty_vectors_add(Value *data);
void gc_dirty_vectors_clear(void);

/* Instrumentation: how many times the write barrier recorded a dirty vector
 * (post-dedup).  Read by gc_nursery_tests() to assert the barrier fires. */
extern long gc_dirty_vectors_fired;

/* Instrumentation counters (GC Phase 2 Step 4 stress tests in zincvm.c).
 * gc_nursery_scavenge_count increments once per real scavenge (excludes
 * the exhausted-nursery short-circuit return).  gc_nursery_pages_reclaimed
 * accumulates the number of pages reset back to the free boundary each
 * time the bump cursor is rewound. */
extern long gc_nursery_scavenge_count;
extern long gc_nursery_pages_reclaimed;

extern long gc_preemptive_scavenge_count;
extern long gc_reactive_scavenge_count;
extern long gc_full_collect_count;

/* Live-set size: number of pages currently allocated in the active
 * semi-space.  Read by compaction tests to prove objects move (pages
 * shrink when live objects are packed into to-space instead of pinned). */
long gc_allocatedpages(void);

/* ---- precise-root API (Phase 3/4) --------------------------------- */

typedef enum { ROOT_PTR, ROOT_VALUE, ROOT_VALUE_ARRAY, ROOT_VALUE_VOLATILE } RootKind;
/* single GC pointer slot.  MUST point at the HEAD of a GC object (not an
 * interior/tail pointer into a multi-page array).  Under 4b.1 the full-collect
 * root scan EVACUATES ROOT_PTR via gc_evacuate → gc_move, which reads the
 * object header at *(ptr-1) to size the copy — an interior pointer would read
 * a garbage header (UB / heap corruption).  The pre-4b pinning API preserved
 * interior pointers (page-granular), so this head-only contract is NEW and
 * load-bearing.  All current call sites pass head pointers; keep it that way. */
void  gc_root_push_ptr(void **slot);
void  gc_root_push_value(Value *vslot);            /* by-value Value, interior ptrs rewritten */
void  gc_root_push_value_volatile(volatile Value *vslot); /* volatile-qualified Value */
void  gc_root_push_value_array(Value *base, int *np); /* N by-value Values */
void  gc_root_pop(void);                           /* pop one entry */
void  gc_root_pop_to(size_t watermark);            /* truncate (longjmp unwind) */
size_t gc_root_watermark(void);                    /* snapshot depth */
/* void* avoids GlobalEntry dependency (GlobalEntry is in zincvm.h,
 * which includes gc.h — circular include avoided). */
void  gc_register_global_table(void *table, int *len_p);
void  gc_register_traced_code(Instr **arr, int *np);

#endif /* ZINCVM_GC_H */
