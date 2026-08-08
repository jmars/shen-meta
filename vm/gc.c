/*
 * gc.c — Cheney mostly-copying collector
 *
 * Type-tag dispatch scavenger calling typed scanning functions
 * (gc_scan_value / gc_evacuate) provided by zincvm.c.
 *
 * Design notes:
 * - HEADER_PTRS repurposed as HEADER_TYPE (0-4, see gc.h)
 * - gc_alloc zeros the entire object body (not just pointer slots)
 * - Roots are precise-only: shadow stack + typed walkers.
 *   No conservative C-stack scan, no extra_roots fallback.
 * - gc_alloc marked __attribute__((noinline)) to spill registers
 * - SIGALRM blocked during collection (zincvm uses alarm for test timeouts)
 */

#include "gc.h"
#include "zinctypes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>
#include "zincvm.h"

/* ---- constants ---------------------------------------------------- */

#define PAGEBYTES  512
#define PAGEWORDS  (PAGEBYTES / sizeof(uintptr_t))
#define WORDBYTES  (sizeof(uintptr_t))

#define PAGE_to_GCP(p)  ((uintptr_t *)((uintptr_t)(p) * PAGEBYTES))
#define GCP_to_PAGE(p)  ((uintptr_t)(p) / PAGEBYTES)

#define MAKE_HEADER(words, ty)  (((uintptr_t)(ty) << 25) | ((uintptr_t)(words) << 1) | 1)
#define FORWARDED(hdr)          (((hdr) & 1) == 0)
#define HEADER_TYPE(hdr)        ((int)((hdr) >> 25 & 0xFFFFF))
#define HEADER_WORDS(hdr)       ((uintptr_t)((hdr) >> 1 & 0xFFFFFF))

#define OBJECT    0
#define CONTINUED 1

/* Nursery: a fixed 2 MB region at the start of the heap reserved for
 * generational collection (Phase 2).  Pages are tagged space==NURSERY
 * and are never selected by allocatepage's free-page scan.  Allocation
 * still goes exclusively through the old-gen path. */
#define NURSERY         3
#define NURSERY_BYTES   (2 * 1024 * 1024)
#define NURSERY_PAGES   (NURSERY_BYTES / PAGEBYTES)

/* Fire a nursery scavenge when free space drops to this fraction of the
 * nursery region, BEFORE the bump cursor exhausts it.  1/8 = 87.5% full.
 * Decouples the nursery trigger from the reactive not-enough-room path. */
#define NURSERY_SCAVENGE_FREE_LOWATER  (NURSERY_BYTES / 8)

/* ---- static state ------------------------------------------------- */

static uintptr_t  firstheappage;
static uintptr_t  lastheappage;
static uintptr_t  heappages;

static uintptr_t  freewords;
static uintptr_t *freep;
static uintptr_t  allocatedpages;
static uintptr_t  freepage;

/* Nursery region bounds (page indices).  Reserved in gc_init;
 * allocation still goes exclusively through the old-gen path. */
static uintptr_t  nursery_first, nursery_last;

/* Nursery bump-allocator state (Phase 2 Step 2).
 * nursery_cur advances forward; nursery_end is one past the last byte.
 * Initialised in gc_init. */
static char *nursery_cur;
static char *nursery_end;

/* page metadata (indexed by page number; allocated relative to firstheappage) */
static uintptr_t *space;   /* 0=free, 1=semi-space-1, 2=semi-space-2 */
static uintptr_t *gc_link;    /* Cheney queue links */
static uintptr_t *type_page; /* OBJECT / CONTINUED */
static uint8_t   *page_queued; /* 1 iff page currently in the Cheney queue (dedup) */

static uintptr_t  queue_head;
static uintptr_t  queue_tail;
static int        in_scavenge = 0;   /* guard against recursive collection */
static uintptr_t  current_space;
static uintptr_t  next_space;

/* Instrumentation counters (GC Phase 2 Step 4 stress tests in zincvm.c).
 * Non-static so zincvm.c can read them via gc.h. */
long gc_nursery_scavenge_count = 0;
long gc_nursery_pages_reclaimed = 0;

long gc_preemptive_scavenge_count = 0;
long gc_reactive_scavenge_count  = 0;
long gc_full_collect_count       = 0;

/* ---- opt-in observability tooling (--gc-verbose / --gc-check-closures /
 * ---- --gc-dump-roots argv flags).  Pure diagnostics; NO GC semantics change. */
static long gc_collect_seq    = 0;   /* per-collection sequence (nursery+fits) */
static int  gc_verbose        = 0;
static int  gc_check_closures = 0;
static int  gc_dump_roots     = 0;
static int  gc_stale_scan     = 0;
static uintptr_t gc_stack_top = 0;   /* page-aligned top of the C stack (main) */
static FILE *gc_log_fp        = NULL;

/* ---- opt-in tools added on top of the above (all pure diagnostics) ---- */
static int      gc_page_transition = 0;       /* --gc-page-transition */
static uintptr_t gc_page_transition_watch = 0; /* --gc-page-transition-watch (0 = watch all) */
static uintptr_t gc_watch_alloc = 0;           /* --gc-watch-alloc (0 = off) */
static int      gc_verify      = 0;           /* --gc-verify */

void gc_set_verbose(int on)        { gc_verbose        = on; }
void gc_set_check_closures(int on) { gc_check_closures = on; }
void gc_set_dump_roots(int on)     { gc_dump_roots     = on; }
void gc_set_stale_scan(int on)     { gc_stale_scan     = on; }
void gc_set_stack_top(uintptr_t top) { gc_stack_top    = top; }
void gc_set_log(const char *path) {
    gc_log_fp = fopen(path, "w");
    if (!gc_log_fp) {
        fprintf(stderr, "gc: cannot open log %s: %s\n", path, strerror(errno));
        exit(1);
    }
}

void gc_set_page_transition(int on) { gc_page_transition = on; }
void gc_set_page_transition_watch(uintptr_t page) {
    gc_page_transition = 1;   /* watching a page implies transitions are on */
    gc_page_transition_watch = page;
}
void gc_set_watch_alloc(uintptr_t addr) { gc_watch_alloc = addr; }
void gc_set_verify(int on)              { gc_verify      = on; }

/* Forward-declared so collect() (defined earlier) can call it.  Defined
 * alongside the other opt-in helpers below gc_stale_scan_stack. */
static void gc_verify_heap(const char *when);

/* All opt-in diagnostic output routes through this; defaults to stderr.
 * Fatal error exits and the ROOT_PTR interior-pointer fatal stay on stderr. */
#define GC_LOG (gc_log_fp ? gc_log_fp : stderr)

/* gc_check_closure: validate a closure's code/env headers are on live pages
 * with the expected type tags.  No-op unless gc_check_closures is set.  Uses
 * the static page/space/current_space state directly (all in this TU). */
void gc_check_closure(Value *cl, const char *where) {
    if (!gc_check_closures) return;
    if (!cl) { fprintf(GC_LOG, "GC-CHECK %s: NULL Value pointer\n", where); return; }
    if (cl->tag != VAL_LAMBDA) {
        fprintf(GC_LOG, "GC-CHECK %s: tag=%d (not VAL_LAMBDA=%d)\n",
                where, (int)cl->tag, (int)VAL_LAMBDA);
        return;
    }
    Instr *code = cl->lambda.code;
    if (code == NULL) {
        fprintf(GC_LOG, "GC-CHECK %s: lambda.code == NULL\n", where);
        return;
    }
    uintptr_t pg = GCP_to_PAGE(code);
    if (pg < firstheappage || pg > lastheappage) {
        fprintf(GC_LOG, "GC-CHECK %s: code ptr=%p page=%lu out of heap [%lu,%lu]\n",
                where, (void *)code, (unsigned long)pg,
                (unsigned long)firstheappage, (unsigned long)lastheappage);
        return;
    }
    if (space[pg] != NURSERY && space[pg] != current_space) {
        fprintf(GC_LOG, "GC-CHECK %s: code ptr=%p page=%lu space=%lu "
                "(expected NURSERY=%d or current=%lu) cl_env=%p env_len=%d code_len=%d\n",
                where, (void *)code, (unsigned long)pg,
                (unsigned long)space[pg], NURSERY, (unsigned long)current_space,
                (void *)cl->lambda.env, cl->lambda.env_len, cl->lambda.code_len);
        /* Decisive: does the SAME stale code pointer appear in the C global_table?
           If yes, the global-table typed-walker scan failed to evacuate it during
           the last full collect (a real GC bug in namespace-2 tracing). If no, the
           stale closure is a transient local copy (C-stack / env) not the table's. */
        if (global_table_len > 0) {
            GlobalEntry *gt = global_table;
            int gn = global_table_len;
            int found = 0;
            /* The interp's global-table (namespace 2) is a nested Shen cons
               list [[name, closure] ...] stored under gt["global-table"].closure.
               Recursively walk cons structures to find the failing .code. */
            #define WALK_SEARCH_MAX 200000
            int stack = 0;
            Value walk[256];
            for (int gi = 0; gi < gn && !found; gi++) {
                if (stack < 256) walk[stack++] = gt[gi].closure;
                int visited = 0;
                while (stack > 0 && !found && visited < WALK_SEARCH_MAX) {
                    Value v = walk[--stack];
                    visited++;
                    if (v.tag == VAL_LAMBDA && v.lambda.code == code) {
                        uintptr_t gpg = GCP_to_PAGE(v.lambda.code);
                        fprintf(GC_LOG, "  GC-CHECK table-hit: global[%d] name='%s' code=%p "
                                "page=%lu space=%lu (nested closure, matches stale code)\n",
                                gi, gt[gi].name ? gt[gi].name : "?", (void *)v.lambda.code,
                                (unsigned long)gpg,
                                (unsigned long)(gpg >= firstheappage && gpg <= lastheappage
                                                ? space[gpg] : 0));
                        found = 1;
                    } else if (v.tag == VAL_CONS) {
                        if (stack + 2 <= 256) {
                            walk[stack++] = *v.cons.car;
                            walk[stack++] = *v.cons.cdr;
                        }
                    }
                }
            }
            if (!found)
                fprintf(GC_LOG, "  GC-CHECK table-miss: stale code=%p NOT in any "
                        "global_table closure (transient local copy)\n", (void *)code);
        }
        /* Dump the raw instruction bytes so the failing closure can be
           identified even though the code page is dead space. */
        int n = cl->lambda.code_len;
        if (n > 0 && n <= 64) {
            fprintf(GC_LOG, "  GC-CHECK code bytes:");
            unsigned char *cb = (unsigned char *)code;
            for (int k = 0; k < n; k++) {
                if (k % 8 == 0) fprintf(GC_LOG, "\n    ");
                fprintf(GC_LOG, " %02x", cb[k]);
            }
            fprintf(GC_LOG, "\n");
        }
        return;
    }
    uintptr_t chdr = ((uintptr_t *)code)[-1];
    if (FORWARDED(chdr)) {
        fprintf(GC_LOG, "GC-CHECK %s: code ptr=%p header FORWARDED (fwd=0x%lx)\n",
                where, (void *)code, (unsigned long)chdr);
        return;
    }
    int cty = HEADER_TYPE(chdr);
    if (cty != GC_TYPE_INSTR_ARRAY) {
        fprintf(GC_LOG, "GC-CHECK %s: code ptr=%p header type=%d "
                "(expected GC_TYPE_INSTR_ARRAY=%d)\n",
                where, (void *)code, cty, GC_TYPE_INSTR_ARRAY);
        return;
    }
    if (cl->lambda.env != NULL) {
        uintptr_t epg = GCP_to_PAGE(cl->lambda.env);
        if (epg < firstheappage || epg > lastheappage) {
            fprintf(GC_LOG, "GC-CHECK %s: env ptr=%p page=%lu out of heap "
                    "[%lu,%lu]\n", where, (void *)cl->lambda.env,
                    (unsigned long)epg, (unsigned long)firstheappage,
                    (unsigned long)lastheappage);
            return;
        }
        if (space[epg] != NURSERY && space[epg] != current_space) {
            fprintf(GC_LOG, "GC-CHECK %s: env ptr=%p page=%lu space=%lu "
                    "(expected NURSERY=%d or current=%lu)\n",
                    where, (void *)cl->lambda.env, (unsigned long)epg,
                    (unsigned long)space[epg], NURSERY, (unsigned long)current_space);
            return;
        }
        uintptr_t ehdr = ((uintptr_t *)cl->lambda.env)[-1];
        int ety = HEADER_TYPE(ehdr);
        if (ety != GC_TYPE_VALUE_ARRAY) {
            fprintf(GC_LOG, "GC-CHECK %s: env ptr=%p header type=%d "
                    "(expected GC_TYPE_VALUE_ARRAY=%d)\n",
                    where, (void *)cl->lambda.env, ety, GC_TYPE_VALUE_ARRAY);
            return;
        }
    }
}

/* Per-allocation-class histogram, indexed by GC type tag (see gc.h).
 * Counts whole allocation requests through the public entry points
 * (gc_alloc / gc_alloc_oldgen / gc_alloc_atomic).  Raw hooks for a future,
 * data-driven revisit of the 4c BiBOP size-class decision: if a class shows
 * high churn, BiBOP may be worth re-evaluating for that class alone. */
unsigned long long gc_alloc_class_count[5] = {0};

const unsigned long long *gc_alloc_class_counts(void) {
    return gc_alloc_class_count;
}

/* raw pointers to malloc'd metadata for eventual teardown */
static char      *raw_heap_start;
static uintptr_t *raw_space_ptr;
static uintptr_t *raw_link_ptr;
static uintptr_t *raw_type_ptr;
static uint8_t   *raw_page_queued_ptr;
static size_t     heap_mmap_size;   /* actual mmap size of the heap */

/* ---- precise-root shadow stack (Phase 4a) ------------------------- */

typedef struct { RootKind kind; void *slot; int *np; } GcRoot;

static GcRoot *shadow_stack = NULL;
static size_t  shadow_len   = 0;
static size_t  shadow_cap   = 0;

/* ---- typed walker registrations (Phase 4a) ------------------------ */

static void   *reg_global_table     = NULL;
static int    *reg_global_table_len = NULL;
static Instr **reg_traced_code      = NULL;
static int    *reg_traced_code_len  = NULL;

/* pinned-page bitmap removed (4b.2 — copying scavenge, no pin-in-place) */

/* ---- nursery / old-gen predicates --------------------------------- */

int gc_in_nursery(void *p) {
    uintptr_t page = GCP_to_PAGE(p);
    return (page >= nursery_first && page <= nursery_last);
}

/* gc_in_oldgen: true iff p's page is in the live old-gen semi-space
 * (space == current_space). */
int gc_in_oldgen(void *p) {
    uintptr_t page = GCP_to_PAGE(p);
    return (page >= firstheappage && page <= lastheappage &&
            space[page] == current_space);
}

/* ---- write-barrier remembered set (Phase 2 Step 5) --------------- */

/* Dirty vectors: old-gen vector element arrays that may now contain
 * nursery pointers after an address-> store.  The remember set lets the
 * nursery scavenge scan only these arrays instead of every old-gen page.
 * An overflow flag acts as a capacity valve; on overflow we fall back to
 * a full old-gen OBJECT-page scan. */
#define DIRTY_VECTORS_MAX 8192
static Value **dirty_vectors = NULL;
static size_t dirty_vectors_count = 0;
static size_t dirty_vectors_cap = 0;
static int dirty_vectors_overflow = 0;

void gc_dirty_vectors_add(Value *data) {
    if (dirty_vectors_overflow) return;
    for (size_t i = 0; i < dirty_vectors_count; i++)
        if (dirty_vectors[i] == data) return;   /* dedup */
    if (dirty_vectors_count >= DIRTY_VECTORS_MAX) {
        dirty_vectors_overflow = 1;
        return;
    }
    if (dirty_vectors_count >= dirty_vectors_cap) {
        size_t nc = dirty_vectors_cap ? dirty_vectors_cap * 2 : 256;
        if (nc > DIRTY_VECTORS_MAX) nc = DIRTY_VECTORS_MAX;
        Value **np = (Value **)realloc(dirty_vectors, nc * sizeof(Value *));
        if (!np) { dirty_vectors_overflow = 1; return; }
        dirty_vectors = np; dirty_vectors_cap = nc;
    }
    dirty_vectors[dirty_vectors_count++] = data;
    gc_dirty_vectors_fired++;
}

void gc_dirty_vectors_clear(void) {
    dirty_vectors_count = 0;
    dirty_vectors_overflow = 0;
}

/* Instrumentation counter (Phase 2 Step 5 stress tests): how many times the
 * write barrier actually recorded a dirty vector (post-dedup, pre-overflow).
 * Lets gc_nursery_tests() assert deterministically that address-> of a
 * nursery-referencing value into an old-gen vector fires the barrier. */
long gc_dirty_vectors_fired = 0;

/* ---- write-barrier remembered set: dirty globals (site 2) --------- */

/* Dirty globals: a fixed 256-byte bitset (GLOBAL_TABLE_MAX bits = 2048).
 * Marked by global_set whenever a closure is stored into any global-table
 * slot; consulted by gc_scan_roots during nursery scavenges to skip
 * non-dirty globals (avoiding re-enqueuing hundreds of old-gen code/env
 * pages that haven't changed).  Cleared at scavenge-end and full-collect
 * start — same lifecycle as dirty_vectors.  No overflow path (the bitset
 * is sized exactly to GLOBAL_TABLE_MAX). */
_Static_assert(GLOBAL_TABLE_MAX % 64 == 0,
               "DIRTY_GLOBALS bitset requires GLOBAL_TABLE_MAX be a multiple of 64");
#define DIRTY_GLOBALS_MAX_BITS GLOBAL_TABLE_MAX
static uint64_t dirty_globals[DIRTY_GLOBALS_MAX_BITS / 64];
long gc_dirty_globals_fired  = 0;
long gc_dirty_globals_scanned = 0;

void gc_dirty_globals_mark(int idx) {
    if (idx < 0 || idx >= GLOBAL_TABLE_MAX) return;
    int word = idx / 64;
    uint64_t mask = 1ULL << (idx % 64);
    if (!(dirty_globals[word] & mask)) {
        dirty_globals[word] |= mask;
        gc_dirty_globals_fired++;
    }
}

int gc_dirty_globals_test(int idx) {
    if (idx < 0 || idx >= GLOBAL_TABLE_MAX) return 0;
    return (dirty_globals[idx / 64] >> (idx % 64)) & 1;
}

void gc_dirty_globals_clear(void) {
    memset(dirty_globals, 0, sizeof(dirty_globals));
}

/* ---- forward declarations ----------------------------------------- */

static void  collect(const char *trigger);
static void  collect_nursery(const char *trigger);
static void  allocatepage(uintptr_t pages);
static void *gcalloc_internal(size_t bytes, int type_tag);
static void *move_internal(uintptr_t *cp, int type_tag);
static void  gc_scan_roots(void);
static void  gc_stale_scan_stack(uintptr_t old_space);

/* ---- Cheney queue ------------------------------------------------- */

static uintptr_t next_page(uintptr_t page) {
    return (page == lastheappage) ? firstheappage : page + 1;
}

static void queue(uintptr_t page) {
    /* Dedup: a page must never be enqueued twice.  queue(P) when P is already
     * in the queue clobbers gc_link[P]=0, truncating the traversal and losing
     * every page that follows P.  Duplicate enqueues happen when gc_move's
     * "already to-space" branch (in_scavenge) re-queues a page already queued
     * by another reference to an object on it. */
    if (page_queued[page]) return;
    page_queued[page] = 1;
    if (queue_head != 0) {
        gc_link[queue_tail] = page;
        gc_link[page] = 0;
        queue_tail = page;
    } else {
        queue_head = page;
        gc_link[page] = 0;
        queue_tail = page;
    }
}

static void queue_reset(void) {
    queue_head = 0;
    queue_tail = 0;
    memset(page_queued + firstheappage, 0, (lastheappage - firstheappage + 1) * sizeof(uint8_t));
}

/* ---- typed scanning helpers (called from collect) ------------------ */

/* Forward-declared: implemented in zincvm.c */
void gc_scan_value(struct Value *v);
void gc_evacuate(void **slot);

/* evac_instr: scan a single Instr for GC pointers */
static void evac_instr(Instr *in) {
    gc_scan_value(&in->operand);          /* scan operand Value */
    gc_evacuate((void **)&in->closure_code); /* evacuate closure_code pointer */
}

/* ---- collector ---------------------------------------------------- */

static void collect(const char *trigger) {
    sigset_t   old_sig_set;

    /* Block SIGALRM during collection and restore the prior mask afterwards:
     * the VM uses alarm() for test timeouts and a signal during collection
     * would longjmp out of the scavenger and corrupt the heap. */
    {
        sigset_t block_set;
        sigemptyset(&block_set);
        sigaddset(&block_set, SIGALRM);
        sigprocmask(SIG_BLOCK, &block_set, &old_sig_set);
    }

    if (next_space != current_space) {
        fprintf(stderr, "gcalloc - Out of space during collect\n");
        exit(1);
    }

    gc_full_collect_count++;
    gc_collect_seq++;
    if (gc_verbose) {
        fprintf(GC_LOG, "[GC FULL #%ld] trigger=%s shadow_depth=%zu live_pages=%lu\n",
                gc_collect_seq, trigger, shadow_len, (unsigned long)allocatedpages);
    }

    /* Finalize any partial page */
    if (freewords != 0) {
        *freep = MAKE_HEADER(freewords, 0);
        freewords = 0;
    }

    /* ---- Phase 0: promote nursery survivors to old-gen ----
     * During a full collect with in_scavenge=0, gc_move returns nursery
     * objects unchanged and nursery pages are never queued for scanning.
     * So a nursery-resident closure whose .code points to an old-gen Instr
     * array would keep a stale .code pointer after that array is evacuated.
     * Promote all live nursery objects to old-gen first (in_scavenge=1,
     * nursery→old-gen via the existing gc_move path), THEN swap semi-spaces
     * and run the normal full collect.  After promotion all live objects
     * are old-gen, so the full collect sees no nursery pointers and
     * evacuates everything correctly.
     *
     * This is NOT a call to collect_nursery() — we skip the side effects
     * (counter bumps, dirty-vector/globals clears, nursery-page reset,
     * nursery-cursor rewind).  The full collect will handle those.
     *
     * gc_scan_roots() with in_scavenge=1 only scans dirty globals (as an
     * optimisation for normal nursery scavenges).  For promotion we must
     * also scan non-dirty globals — a non-dirty global may still reference
     * a nursery object that was stored via global_set between collections. */
    {
        in_scavenge = 1;
        queue_reset();
        gc_scan_roots();

        /* Scan non-dirty globals explicitly: every global-table entry
         * must be walked so nursery closures reachable only through
         * non-dirty globals are also promoted. */
        if (reg_global_table && reg_global_table_len) {
            GlobalEntry *gt = (GlobalEntry *)reg_global_table;
            int n = *reg_global_table_len;
            for (int i = 0; i < n; i++) {
                if (!gc_dirty_globals_test(i))
                    gc_scan_value(&gt[i].closure);
            }
        }

        /* Cheney drain: same pattern as collect_nursery */
        while (queue_head != 0) {
            uintptr_t qpg  = queue_head;
            uintptr_t *cp  = PAGE_to_GCP(qpg);

            while (GCP_to_PAGE(cp) == qpg && cp != freep) {
                uintptr_t hw = HEADER_WORDS(*cp);
                int ty = HEADER_TYPE(*cp);

                if (hw == 0) break;
                if (ty < 0 || ty > GC_TYPE_CALLFRAME_ARRAY) break;
                uintptr_t *body = cp + 1;

                switch (ty) {
                case 0: /* GC_TYPE_RAW */ break;

                case 1: /* GC_TYPE_VALUE */
                    gc_scan_value((Value *)body);
                    break;

                case 2: { /* GC_TYPE_VALUE_ARRAY */
                    uintptr_t body_bytes = (hw - 1) * WORDBYTES;
                    int count = (int)(body_bytes / sizeof(Value));
                    Value *arr = (Value *)body;
                    for (int j = 0; j < count; j++)
                        gc_scan_value(&arr[j]);
                    break;
                }

                case 3: { /* GC_TYPE_INSTR_ARRAY */
                    uintptr_t body_bytes = (hw - 1) * WORDBYTES;
                    int count = (int)(body_bytes / sizeof(Instr));
                    Instr *arr = (Instr *)body;
                    for (int j = 0; j < count; j++)
                        evac_instr(&arr[j]);
                    break;
                }

                case 4: { /* GC_TYPE_CALLFRAME_ARRAY */
                    uintptr_t body_bytes = (hw - 1) * WORDBYTES;
                    int count = (int)(body_bytes / sizeof(CallFrame));
                    CallFrame *arr = (CallFrame *)body;
                    for (int j = 0; j < count; j++) {
                        gc_evacuate((void **)&arr[j].code);
                        gc_evacuate((void **)&arr[j].env);
                        gc_evacuate((void **)&arr[j].stack.data);
                    }
                    break;
                }

                default: break;
                }

                cp += hw;
            }
            queue_head = gc_link[queue_head];
        }

        /* Promotion allocated in old-gen; finalise any partial page so the
         * full collect's gcalloc_internal starts from a clean slate in
         * to-space rather than consuming residual freewords on a from-space
         * page. */
        if (freewords != 0) {
            *freep = MAKE_HEADER(freewords, 0);
            freewords = 0;
        }

        in_scavenge = 0;
    }

    /* Swap semi-spaces */
    next_space = (current_space == 1) ? 2 : 1;
    allocatedpages = 0;
    queue_reset();
    gc_dirty_vectors_clear();
    gc_dirty_globals_clear();

    /* ---- root set ---- */

    gc_scan_roots();

    /* ---- Cheney scavenge ---- */

    while (queue_head != 0) {
        uintptr_t *cp = PAGE_to_GCP(queue_head);
        while (GCP_to_PAGE(cp) == queue_head && cp != freep) {
            uintptr_t hw = HEADER_WORDS(*cp);

            /* False-positive guard: a random stack value may look like a
             * valid header.  Validate by the type tag, NOT by a word-count
             * bound: the header type must be one of the 5 real tags.  (An
             * upper word bound like PAGEWORDS*2 would silently skip large
             * legitimate objects — e.g. the multi-MB frame_stack or grown
             * env/value arrays — leaving their interior pointers unevacuated
             * and dangling.  hw itself is bounded by HEADER_WORDS' 24-bit
             * field, and a real header always carries a valid type tag.) */
            int ty = HEADER_TYPE(*cp);
            if (hw == 0) break;                       /* NULL header — false */
            if (ty < 0 || ty > GC_TYPE_CALLFRAME_ARRAY) break;  /* false pos */
            uintptr_t *body = cp + 1;

            switch (ty) {
            case 0: /* GC_TYPE_RAW — no pointers */
                break;

            case 1: /* GC_TYPE_VALUE — single Value */
                gc_scan_value((Value *)body);
                break;

            case 2: { /* GC_TYPE_VALUE_ARRAY — Value[] */
                /* bytes = (hw - 1) * WORDBYTES; count = bytes / sizeof(Value) */
                uintptr_t body_bytes = (hw - 1) * WORDBYTES;
                int count = (int)(body_bytes / sizeof(Value));
                Value *arr = (Value *)body;
                for (int j = 0; j < count; j++)
                    gc_scan_value(&arr[j]);
                break;
            }

            case 3: { /* GC_TYPE_INSTR_ARRAY — Instr[] */
                uintptr_t body_bytes = (hw - 1) * WORDBYTES;
                int count = (int)(body_bytes / sizeof(Instr));
                Instr *arr = (Instr *)body;
                for (int j = 0; j < count; j++)
                    evac_instr(&arr[j]);
                break;
            }

            case 4: { /* GC_TYPE_CALLFRAME_ARRAY — CallFrame[] */
                uintptr_t body_bytes = (hw - 1) * WORDBYTES;
                int count = (int)(body_bytes / sizeof(CallFrame));
                CallFrame *arr = (CallFrame *)body;
                for (int j = 0; j < count; j++) {
                    gc_evacuate((void **)&arr[j].code);
                    gc_evacuate((void **)&arr[j].env);
                    gc_evacuate((void **)&arr[j].stack.data);
                }
                break;
            }

            default:
                /* Unknown type — skip (conservative: don't crash on
                 * false-positive header reads) */
                break;
            }

            cp += hw;
        }
        queue_head = gc_link[queue_head];
    }

    /* ---- finish ---- */

    current_space = next_space;

    /* Opt-in stale-reference scan: after the semi-space flip, the previous
     * from-space (now dead) may still hold pointers from stale C-stack
     * slots.  Purely observational; does NOT change collection semantics. */
    if (gc_stale_scan && gc_stack_top) {
        uintptr_t dead = (current_space == 1) ? 2 : 1;
        gc_stale_scan_stack(dead);
    }

    /* Opt-in heap-invariant verification (--gc-verify).  Purely diagnostic;
     * never aborts, only logs. */
    gc_verify_heap("post-collect");

    /* Restore the previous SIGALRM mask */
    sigprocmask(SIG_SETMASK, &old_sig_set, NULL);
}

/* ---- reusable C-stack backtrace helpers ---------------------------- */
/* Walk the frame-pointer chain from the current frame up to the
 * page-aligned top of the stack (gc_stack_top).  Captures (start,end,ret)
 * per frame so callers can attribute stack slots / return addresses to the
 * C function that owns them.  Shared by gc_stale_scan_stack (slot
 * attribution) and gc_backtrace (page-transition / alloc diagnostics).
 * Returns the number of frames captured.  -O0 debug builds keep frame
 * pointers; on x86-64 the saved frame ptr is at [fp] and the return
 * address at [fp+8]. */
#define GC_BT_MAX_FRAMES 64

typedef struct { uintptr_t start, end; uintptr_t ret; } GcFrameMap;

static int gc_collect_frames(GcFrameMap *out, int max_frames) {
    uintptr_t top = gc_stack_top;
    if (!top) return 0;
    int nf = 0;
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    for (int lvl = 0; lvl < max_frames && fp; lvl++) {
        uintptr_t next = *((uintptr_t *)fp);           /* saved frame ptr */
        uintptr_t ret  = *((uintptr_t *)fp + 1);       /* return address */
        if (fp < top && nf < max_frames) {
            out[nf].start = fp;
            out[nf].end   = (next && next > fp) ? next : top;
            out[nf].ret   = ret;
            nf++;
        }
        if (!next || next <= fp) break;                /* end of chain */
        fp = next;
    }
    return nf;
}

/* Print the current C call stack as raw return addresses (no ELF symbol
 * resolution — matches existing diagnostic behavior). */
static void gc_backtrace(FILE *out) {
    GcFrameMap fm[GC_BT_MAX_FRAMES];
    int nf = gc_collect_frames(fm, GC_BT_MAX_FRAMES);
    fprintf(out, "  frames=%d\n", nf);
    for (int i = 0; i < nf; i++)
        fprintf(out, "    frame[%d] [%p,%p) ret=%p\n", i,
                (void *)fm[i].start, (void *)fm[i].end, (void *)fm[i].ret);
}

/* ---- opt-in stale-reference scan (--gc-stale-scan) ------------------ */
/* Walks the C stack region [current frame, gc_stack_top) looking for word
 * values that point into the given dead old-gen semi-space `old_space` (or
 * the nursery).  These are "stale references": GC-managed pointers left in
 * dead C-stack slots that the precise-root shadow stack does NOT cover.
 * Purely diagnostic — no mutation, no semantics change. */
static void gc_stale_scan_stack(uintptr_t old_space) {
    volatile char marker;
    uintptr_t local = (uintptr_t)&marker;
    uintptr_t top = gc_stack_top;
    long hits = 0;
    FILE *out = GC_LOG;
    if (!top || local >= top) return;

    /* Frame map: shared walker attributes each stale hit to the C function
     * that owns that stack slot (its return address → addr2line). */
    GcFrameMap fm[GC_BT_MAX_FRAMES];
    int nf = gc_collect_frames(fm, GC_BT_MAX_FRAMES);

    fprintf(out, "[GC STALE-SCAN #%ld] scanning stack [%p, %p) old_space=%lu frames=%d\n",
            gc_collect_seq, (void *)local, (void *)top, (unsigned long)old_space, nf);
    if (nf > 0) {
        fprintf(out, "  FRAMES:\n");
        for (int i = 0; i < nf; i++)
            fprintf(out, "    frame[%d] [%p,%p) ret=%p\n", i,
                    (void *)fm[i].start, (void *)fm[i].end, (void *)fm[i].ret);
    }
    for (uintptr_t *sp = (uintptr_t *)((local + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1));
         (uintptr_t)sp < top; sp++) {
        uintptr_t w = *sp;
        uintptr_t pg = GCP_to_PAGE(w);
        if (pg < firstheappage || pg > lastheappage) continue;
        if (space[pg] != old_space) continue;
        uintptr_t hdr = ((uintptr_t *)w)[-1];
        int fwd = FORWARDED(hdr);
        long offset = (long)((uintptr_t)sp - local);
        int owner = -1;
        for (int i = 0; i < nf; i++)
            if ((uintptr_t)sp >= fm[i].start && (uintptr_t)sp < fm[i].end) { owner = i; break; }
        fprintf(out, "  STALE: stack=%p (local+%ld) frame=%d%s ptr=%p page=%lu %s hdr=0x%lx\n",
                (void *)sp, offset, owner, owner >= 0 ? "": "(below-local)",
                (void *)w, (unsigned long)pg, fwd ? "FORWARDED" : "dead-space",
                (unsigned long)hdr);
        hits++;
    }
    fprintf(out, "[GC STALE-SCAN #%ld] %ld potential stale references found\n",
            gc_collect_seq, hits);
}

/* ---- opt-in page-transition log (--gc-page-transition) -------------- */
/* Log every reclassification of a page's space[] slot (e.g. free→to-space
 * during allocatepage).  Purely diagnostic. */
static void gc_log_page_transition(uintptr_t page, int old_space, int new_space,
                                   const char *where) {
    if (!gc_page_transition) return;
    if (gc_page_transition_watch && page != gc_page_transition_watch) return;
    if (old_space == new_space) return;
    /* Bug-2 signal: only log recycling of a previously-allocated page
     * (old_space != 0).  Fresh free-page allocations (old_space==0) during
     * heap growth are noise and dominate the log. */
    if (!gc_page_transition_watch && old_space == 0) return;
    fprintf(GC_LOG, "[GC PAGE-TRANSITION #%ld] page=%lu %d->%d where=%s\n",
            gc_collect_seq, (unsigned long)page, old_space, new_space, where);
    gc_backtrace(GC_LOG);
}

/* ---- opt-in allocation watcher (--gc-watch-alloc <addr>) ------------- */
/* True iff an object whose body spans [body, body+body_bytes) covers the
 * watched address.  Purely diagnostic. */
static int gc_watch_hits(uintptr_t body, size_t body_bytes) {
    return gc_watch_alloc &&
           (uintptr_t)body <= gc_watch_alloc &&
           gc_watch_alloc < (uintptr_t)body + body_bytes;
}

/* ---- opt-in heap verification (--gc-verify) ------------------------- */
/* Walk the heap pages checking structural invariants.  Logs every violation
 * (never aborts) and prints a summary line.  Purely diagnostic.
 *
 * Invariants:
 *   1. a free page (space==0) must have type_page==0;
 *   2. a CONTINUED page must not follow a FREE page (its multi-page object
 *      always occupies contiguous, non-free pages in one space).
 *
 * The header walk (invariant 3, counts live objects) treats a FORWARDED or
 * invalid-type header as a page boundary and stops there, mirroring the
 * collector's own Cheney drain (which breaks at such headers).  This is
 * intentional: the collector legitimately reuses stale from-space pages, so
 * a live object may be followed by a leftover forwarding pointer that the
 * drain treats as end-of-page — NOT corruption.  The collector retains OLD
 * from-space pages tagged space==current_space whose head object was
 * evacuated (forwarding pointer at offset 0); those stale pages are skipped.
 * The walk stops at freep (the live-data boundary) so the trailing free
 * region of the current allocation page is never misread as a header. */
static void gc_verify_heap(const char *when) {
    if (!gc_verify) return;
    long pages = 0, objects = 0, errors = 0;
    /* Diagnostic caps: bound the page walk and the per-page object count
     * independently so a pathological heap cannot spin the verifier forever. */
    const long max_pages = 1000000;
    const long max_objects = 1000000;

    for (uintptr_t pg = firstheappage; pg <= lastheappage && pages < max_pages; pg++) {
        pages++;
        int sp = (int)space[pg];
        int tp = (int)type_page[pg];

        /* Invariant 1: free page must be untagged */
        if (sp == 0 && tp != 0) {
            fprintf(GC_LOG,
                    "[GC VERIFY #%ld] %s: error free page=%lu type_page=%d (space=0)\n",
                    gc_collect_seq, when, (unsigned long)pg, tp);
            errors++;
        }

        /* Invariant 2: a CONTINUED page must not follow a free page */
        if (tp == CONTINUED && pg > firstheappage && (int)space[pg - 1] == 0) {
            fprintf(GC_LOG,
                    "[GC VERIFY #%ld] %s: error CONTINUED page=%lu pred_space=0 space=%d\n",
                    gc_collect_seq, when, (unsigned long)pg, sp);
            errors++;
        }

        /* Count live objects on OBJECT pages in current_space. */
        if (sp == (int)current_space && tp == OBJECT) {
            uintptr_t *cp = PAGE_to_GCP(pg);
            /* Stale retained from-space page: head object evacuated, so the
             * first word is a forwarding pointer.  Benign — skip the page. */
            if (FORWARDED(*cp)) continue;
            while (GCP_to_PAGE(cp) == pg && cp != freep && objects < max_objects) {
                uintptr_t hdr = *cp;
                int ty = HEADER_TYPE(hdr);
                if (FORWARDED(hdr) || ty < 0 || ty > GC_TYPE_CALLFRAME_ARRAY ||
                    HEADER_WORDS(hdr) == 0)
                    break;   /* page boundary — same as the Cheney drain */
                objects++;
                cp += HEADER_WORDS(hdr);
            }
        }
    }

    fprintf(GC_LOG, "[GC VERIFY #%ld] %s - pages=%ld objects=%ld errors=%d\n",
            gc_collect_seq, when, pages, objects, (int)errors);
}

/* ---- nursery collection (Phase 4b.2 — copying scavenge) ------------- */

/* gc_scan_roots: walk the precise-root shadow stack + typed walkers.
 * This is the SOLE authoritative root source; there is no conservative
 * C-stack scan or extra_roots fallback.
 *
 * Roots are EVACUATED: gc_scan_value/gc_evacuate update root slots in
 * place.  During a full collect (in_scavenge=0), nursery pointers are
 * returned unchanged by gc_move (nursery is untouched).  During a nursery
 * scavenge (in_scavenge=1), nursery objects are copied to old-gen via
 * gc_move→move_internal.  The Cheney drain recursively scans all
 * reachable objects. */
static void gc_scan_roots(void) {
    /* 0. Root-set dump (opt-in --gc-dump-roots) */
    if (gc_dump_roots) {
        fprintf(GC_LOG, "[GC ROOTS %s #%ld] shadow_depth=%zu\n",
                in_scavenge ? "NURSERY" : "FULL", gc_collect_seq, shadow_len);
        for (size_t i = 0; i < shadow_len; i++) {
            GcRoot *r = &shadow_stack[i];
            fprintf(GC_LOG, "  [%zu] kind=%d slot=%p", i, (int)r->kind, r->slot);
            if (r->kind == ROOT_PTR) {
                void *p = *(void **)r->slot;
                if (p) {
                    uintptr_t pg = GCP_to_PAGE(p);
                    int ty = (pg >= firstheappage && pg <= lastheappage)
                             ? HEADER_TYPE(((uintptr_t *)p)[-1]) : -1;
                    fprintf(GC_LOG, " -> obj=%p page=%lu hdr_type=%d",
                            p, (unsigned long)pg, ty);
                    /* Root-liveness cross-check: a live root's object page
                     * must be in the nursery or the active old-gen semi-space.
                     * A page in the inactive (dead) semi-space means the root
                     * was NOT evacuated by the preceding collection — a real
                     * bug (stale root). */
                    int live = (pg >= firstheappage && pg <= lastheappage)
                             ? (space[pg] == NURSERY || space[pg] == current_space)
                             : 0;
                    if (!live) {
                        fprintf(GC_LOG,
                                " -> ** DEAD-SPACE ** (space=%lu current=%lu)",
                                (unsigned long)(pg >= firstheappage && pg <= lastheappage
                                                ? space[pg] : 0),
                                (unsigned long)current_space);
                    }
                } else {
                    fprintf(GC_LOG, " -> obj=NULL");
                }
            }
            fprintf(GC_LOG, "\n");
        }
    }

    /* 1. Shadow stack entries */
    for (size_t i = 0; i < shadow_len; i++) {
        GcRoot *r = &shadow_stack[i];
        switch (r->kind) {
        case ROOT_PTR: {
            /* ROOT_PTR must point at an object HEAD (see gc.h).  An
             * interior pointer into a multi-page object would make
             * gc_move read a garbage header at *(ptr-1) — UB / heap
             * corruption.  Cheap defense: a head page is never
             * CONTINUED (only tail pages are). */
            void *p = *(void **)r->slot;
            if (p) {
                uintptr_t pg = GCP_to_PAGE(p);
                if (pg >= firstheappage && pg <= lastheappage &&
                    type_page[pg] == CONTINUED) {
                    fprintf(stderr,
                            "gc: ROOT_PTR points into a multi-page object "
                            "tail (page %lu) — interior pointer as root; "
                            "only object HEAD pointers are valid roots\n",
                            (unsigned long)pg);
                    exit(1);
                }
            }
            gc_evacuate((void **)r->slot);
            break;
        }
        case ROOT_VALUE:
            gc_scan_value((Value *)r->slot);
            break;
        case ROOT_VALUE_VOLATILE: {
            volatile Value *vs = (volatile Value *)r->slot;
            Value tmp = *vs;
            gc_scan_value(&tmp);
            *vs = tmp;
            break;
        }
        case ROOT_VALUE_ARRAY: {
            Value *base = (Value *)r->slot;
            int n = *(r->np);
            for (int j = 0; j < n; j++)
                gc_scan_value(&base[j]);
            break;
        }
        }
    }

    /* 2. Typed walker: global_table closures.
     * During a nursery scavenge, skip non-dirty globals via the bitset
     * to avoid re-enqueuing hundreds of stable old-gen code/env pages.
     * Full collects always scan every global (bitset is cleared at start). */
    if (reg_global_table && reg_global_table_len) {
        GlobalEntry *gt = (GlobalEntry *)reg_global_table;
        int n = *reg_global_table_len;
        if (in_scavenge) {
            for (int i = 0; i < n; i++) {
                if (gc_dirty_globals_test(i)) {
                    gc_scan_value(&gt[i].closure);
                    gc_dirty_globals_scanned++;
                }
            }
        } else {
            for (int i = 0; i < n; i++)
                gc_scan_value(&gt[i].closure);
        }
    }

    /* 3. Typed walker: traced_code Instr arrays */
    if (reg_traced_code && reg_traced_code_len) {
        int n = *reg_traced_code_len;
        for (int i = 0; i < n; i++) {
            if (reg_traced_code[i])
                gc_evacuate((void **)&reg_traced_code[i]);
        }
    }
}

/* collect_nursery: copying nursery scavenge.
 *
 * Does NOT swap semi-spaces (current_space == next_space throughout).
 * Roots are scanned via gc_scan_roots() (precise shadow stack + typed
 * walkers).  Nursery survivors are COPIED to old-gen via gc_move →
 * move_internal, which allocates destination pages in next_space
 * (== current_space) and writes forwarding pointers in the nursery
 * source headers.  The write-barrier dirty-vectors remembered set is
 * scanned inline for old-gen→nursery references.
 *
 * After the Cheney drain, ALL nursery pages are reset to NURSERY and
 * the bump cursor is rewound to the nursery start — the nursery is
 * fully reusable every cycle. */
static void collect_nursery(const char *trigger) {
    sigset_t   old_sig_set;

    /* Guard against recursive entry */
    if (in_scavenge) {
        fprintf(stderr, "collect_nursery: re-entered during scavenge\n");
        exit(1);
    }

    /* Block SIGALRM during collection */
    {
        sigset_t block_set;
        sigemptyset(&block_set);
        sigaddset(&block_set, SIGALRM);
        sigprocmask(SIG_BLOCK, &block_set, &old_sig_set);
    }

    in_scavenge = 1;

    /* Finalize any partial old-gen page before scanning */
    if (freewords != 0) {
        *freep = MAKE_HEADER(freewords, 0);
        freewords = 0;
    }

    /* No semi-space swap; no reset of allocatedpages */

    /* Count this as a real scavenge. */
    gc_nursery_scavenge_count++;
    gc_collect_seq++;
    if (gc_verbose) {
        fprintf(GC_LOG, "[GC NURSERY #%ld] trigger=%s shadow_depth=%zu nursery_free=%zu\n",
                gc_collect_seq, trigger, shadow_len, (size_t)(nursery_end - nursery_cur));
    }

    /* Reset the Cheney queue */
    queue_reset();

    /* ---- root set ---- */

    gc_scan_roots();

    /* ---- scan dirty old-gen vectors (write-barrier remembered set) ---- */
    if (dirty_vectors_overflow) {
        for (uintptr_t pg = nursery_last + 1; pg <= lastheappage; pg++) {
            if (space[pg] == current_space && type_page[pg] == OBJECT)
                queue(pg);
        }
    } else {
        for (size_t k = 0; k < dirty_vectors_count; k++) {
            Value *data = dirty_vectors[k];
            if (!gc_in_oldgen(data)) continue;
            uintptr_t *cp = (uintptr_t *)data - 1;
            int ty = HEADER_TYPE(*cp);
            if (ty != GC_TYPE_VALUE_ARRAY) continue;
            uintptr_t hw = HEADER_WORDS(*cp);
            uintptr_t body_bytes = (hw - 1) * WORDBYTES;
            int count = (int)(body_bytes / sizeof(Value));
            for (int j = 0; j < count; j++)
                gc_scan_value(&data[j]);
        }
    }

    /* ---- Cheney scavenge ---- */
    /* Under 4b.2 copying scavenge, nursery objects are copied to old-gen
     * and the destination pages are queued via allocatepage.  The drain
     * processes old-gen pages; nursery pages are never queued. */

    while (queue_head != 0) {
        uintptr_t qpg  = queue_head;
        uintptr_t *cp  = PAGE_to_GCP(qpg);

        while (GCP_to_PAGE(cp) == qpg && cp != freep) {
            uintptr_t hw = HEADER_WORDS(*cp);
            int ty = HEADER_TYPE(*cp);

            /* False-positive guard (same as collect()) */
            if (hw == 0) break;
            if (ty < 0 || ty > GC_TYPE_CALLFRAME_ARRAY) break;
            uintptr_t *body = cp + 1;

            switch (ty) {
            case 0: /* GC_TYPE_RAW */ break;

            case 1: /* GC_TYPE_VALUE */
                gc_scan_value((Value *)body);
                break;

            case 2: { /* GC_TYPE_VALUE_ARRAY */
                uintptr_t body_bytes = (hw - 1) * WORDBYTES;
                int count = (int)(body_bytes / sizeof(Value));
                Value *arr = (Value *)body;
                for (int j = 0; j < count; j++)
                    gc_scan_value(&arr[j]);
                break;
            }

            case 3: { /* GC_TYPE_INSTR_ARRAY */
                uintptr_t body_bytes = (hw - 1) * WORDBYTES;
                int count = (int)(body_bytes / sizeof(Instr));
                Instr *arr = (Instr *)body;
                for (int j = 0; j < count; j++)
                    evac_instr(&arr[j]);
                break;
            }

            case 4: { /* GC_TYPE_CALLFRAME_ARRAY */
                uintptr_t body_bytes = (hw - 1) * WORDBYTES;
                int count = (int)(body_bytes / sizeof(CallFrame));
                CallFrame *arr = (CallFrame *)body;
                for (int j = 0; j < count; j++) {
                    gc_evacuate((void **)&arr[j].code);
                    gc_evacuate((void **)&arr[j].env);
                    gc_evacuate((void **)&arr[j].stack.data);
                }
                break;
            }

            default: break;
            }

            cp += hw;
        }
        queue_head = gc_link[queue_head];
    }

    /* ---- reset nursery: full reclaim ---- */
    /* Under copying scavenge, survivors have been copied to old-gen.
     * Reset ALL nursery pages to NURSERY and rewind the bump cursor
     * to the nursery start — the nursery is fully reusable. */
    {
        for (uintptr_t pg = nursery_first; pg <= nursery_last; pg++)
            space[pg] = NURSERY;
        gc_nursery_pages_reclaimed += NURSERY_PAGES;
        nursery_cur = (char *)PAGE_to_GCP(nursery_first);
    }

    /* Opt-in stale-reference scan: after the nursery reset, stale C-stack
     * slots may still point into the just-scavenged (now empty) nursery.
     * Purely observational; no semantics change. */
    if (gc_stale_scan && gc_stack_top) {
        gc_stale_scan_stack(NURSERY);
    }

    gc_dirty_vectors_clear();
    gc_dirty_globals_clear();

    /* Opt-in heap-invariant verification (--gc-verify).  Purely diagnostic;
     * never aborts, only logs. */
    gc_verify_heap("post-nursery");

    in_scavenge = 0;

    /* Restore the previous SIGALRM mask */
    sigprocmask(SIG_SETMASK, &old_sig_set, NULL);
}

/* ---- internal allocator ------------------------------------------- */

/* gcalloc_internal: allocate bytes with given type tag.  This is the
 * low-level bump allocator; it may trigger collect() but never calls
 * gc_alloc (which would be recursive).  Returns a pointer to the body
 * (past the header word). */
static void *gcalloc_internal(size_t bytes, int type_tag) {
    /* words needed: 1 header + ceiling(bytes / WORDBYTES) */
    uintptr_t words = (bytes + WORDBYTES - 1) / WORDBYTES + 1;

    if (words > 0xFFFFFF) {
        fprintf(stderr, "gcalloc: object too large (%lu bytes)\n",
                (unsigned long)bytes);
        exit(1);
    }

    while (words > freewords) {
        if (freewords != 0) {
            *freep = MAKE_HEADER(freewords, 0);
        }
        freewords = 0;
        allocatepage((words + PAGEWORDS - 1) / PAGEWORDS);
    }

    /* Write header */
    *freep = MAKE_HEADER(words, type_tag);

    /* Zero the body */
    memset(freep + 1, 0, (words - 1) * WORDBYTES);

    uintptr_t *object = freep + 1;

    /* Opt-in allocation watcher (--gc-watch-alloc).  Purely diagnostic. */
    if (gc_watch_hits((uintptr_t)object, bytes)) {
        fprintf(GC_LOG,
                "[GC WATCH-ALLOC #%ld] ALLOC body=%p bytes=%zu tag=%d in_scavenge=%d current=%lu next=%lu\n",
                gc_collect_seq, (void *)object, bytes, type_tag, in_scavenge,
                (unsigned long)current_space, (unsigned long)next_space);
        gc_backtrace(GC_LOG);
    }

    if (words < PAGEWORDS) {
        freewords -= words;
        freep += words;
    } else {
        freewords = 0;
    }

    return object;
}

/* ---- page allocation ---------------------------------------------- */

/* Minimum heap size: 16MB. Never shrink below this. */
#define MIN_HEAP_PAGES 32768

/* ---- dynamic heap growth / shrinkage (within the VAS reservation) -- */

/* Grow the logical heap by doubling.  Requires the mmap reservation in
 * gc_init to be large enough (4GB of VAS is reserved so growth is a
 * pure bookkeeping step, no mremap).  Creates fresh space==0 pages that
 * the collector can use during a scavenge. */
static int grow_heap(uintptr_t pages_needed) {
    uintptr_t new_heappages = heappages * 2;
    size_t new_heap_size = new_heappages * PAGEBYTES;

    uintptr_t min_needed = (allocatedpages + pages_needed + 512) * 2;
    if (new_heappages < min_needed) {
        new_heappages = min_needed;
        new_heap_size = new_heappages * PAGEBYTES;
    }

    /* Fits within the mmap reservation — pure logical growth. */
    if (new_heap_size + PAGEBYTES - 1 <= heap_mmap_size) {
        uintptr_t *new_space = realloc(raw_space_ptr, new_heappages * sizeof(uintptr_t));
        uintptr_t *new_link  = realloc(raw_link_ptr,  new_heappages * sizeof(uintptr_t));
        uintptr_t *new_type  = realloc(raw_type_ptr,  new_heappages * sizeof(uintptr_t));
        uint8_t   *new_pq    = realloc(raw_page_queued_ptr, new_heappages * sizeof(uint8_t));
        if (!new_space || !new_link || !new_type || !new_pq) return -1;

        raw_space_ptr = new_space;
        raw_link_ptr  = new_link;
        raw_type_ptr  = new_type;
        raw_page_queued_ptr = new_pq;
        space  = new_space - firstheappage;
        gc_link = new_link  - firstheappage;
        type_page = new_type  - firstheappage;
        page_queued = new_pq - firstheappage;

        uintptr_t old_last = lastheappage;
        lastheappage = firstheappage + new_heappages - 1;
        heappages = new_heappages;
        for (uintptr_t i = old_last + 1; i <= lastheappage; i++) {
            space[i] = 0; gc_link[i] = 0; type_page[i] = 0; page_queued[i] = 0;
        }
        return 0;
    }

    fprintf(stderr, "[gc] grow_heap: need %zu MB but reservation is %zu MB\n",
            new_heap_size / (1024 * 1024), heap_mmap_size / (1024 * 1024));
    return -1;
}

/* Old-gen full-collect triggers.  read heappages live so grow_heap is tracked. */
static inline uintptr_t oldgen_collect_threshold(void)   { return heappages / 4; }
static inline uintptr_t oldgen_collect_lastresort(void)  { return heappages / 2; }

/* ---- allocatepage --------------------------------------------------- */

static void allocatepage(uintptr_t pages) {
    uintptr_t free;
    uintptr_t firstpage;
    uintptr_t allpages;
    int retried = 0;

retry:
    /* Trigger a collection before allocating new pages.  collect() must NOT
     * be re-entered from within an in-progress collection: during the
     * scavenge phase next_space != current_space, and collect() would hit
     * its re-entry guard and abort.  In that case we simply allocate from
     * to-space directly (the normal Cheney in-scavenge allocation path —
     * gc_move copies live objects here). */
    if (current_space == next_space &&  /* not mid-collection */
        !in_scavenge &&                /* not during nursery scavenge */
        allocatedpages + pages >= oldgen_collect_lastresort()) {
        collect("LASTRESORT");
        if (allocatedpages + pages >= oldgen_collect_lastresort()) {
            if (!retried && grow_heap(pages) == 0) {
                retried = 1;
                goto retry;
            }
            fprintf(stderr,
                    "gcalloc - Out of memory: need %lu pages, "
                    "live set is %lu pages "
                    "(semi-space capacity %lu pages)\n",
                    (unsigned long)pages,
                    (unsigned long)allocatedpages,
                    (unsigned long)(heappages / 2));
            exit(1);
        }
    }

    free = 0;
    allpages = heappages;

    /* Scan cyclically from freepage looking for `pages` consecutive
     * free (not in current_space, not in next_space, not nursery) pages. */
    while (allpages--) {
        if (space[freepage] != current_space &&
            space[freepage] != next_space &&
            space[freepage] != NURSERY)
        {
            if (free++ == 0)
                firstpage = freepage;

            if (free == pages) {
                freep = PAGE_to_GCP(firstpage);

                if (current_space != next_space || in_scavenge)
                    queue(firstpage);

                freewords = pages * PAGEWORDS;
                allocatedpages += pages;
                freepage = next_page(freepage);
                {
                    int old = (int)space[firstpage];
                    space[firstpage] = next_space;
                    type_page[firstpage] = OBJECT;
                    gc_log_page_transition(firstpage, old, (int)next_space,
                                           "allocatepage-obj");
                }

                while (--pages) {
                    int old = (int)space[firstpage + 1];
                    space[++firstpage] = next_space;
                    type_page[firstpage] = CONTINUED;
                    gc_log_page_transition(firstpage, old, (int)next_space,
                                           "allocatepage-cont");
                }
                return;
            }
        } else {
            free = 0;
        }
        freepage = next_page(freepage);
        if (freepage == firstheappage)
            free = 0;  /* wrapped around — restart contiguous count */
    }

    /* Scan exhausted — try growing (once) then retry.  Growth creates fresh
     * space==0 pages usable during a scavenge (the from-space pages of the
     * current collection are not reclaimed until the next one). */
    if (!retried && grow_heap(pages) == 0) {
        retried = 1;
        goto retry;
    }

    fprintf(stderr,
            "gcalloc - Unable to allocate %lu pages in a %lu page heap\n",
            (unsigned long)pages, (unsigned long)heappages);
    exit(1);
}

/* ---- move --------------------------------------------------------- */

/* move_internal: copy an object from from-space to to-space.
 * Preserves the type tag.  Returns the new body pointer. */
static void *move_internal(uintptr_t *cp, int type_tag) {
    uintptr_t header;
    uintptr_t cnt;
    uintptr_t *np;
    uintptr_t *to;
    uintptr_t *from;

    if (cp == NULL) return NULL;

    header = cp[-1];
    if (FORWARDED(header)) {
        return (void *)header;  /* header IS the forwarding pointer */
    }

    /* Allocate in to-space with the same type */
    np = gcalloc_internal((HEADER_WORDS(header) - 1) * WORDBYTES, type_tag);

    to   = np - 1;
    from = cp - 1;
    cnt  = HEADER_WORDS(header);

    /* Copy header + body */
    while (cnt--)
        *to++ = *from++;

    /* Opt-in allocation watcher (--gc-watch-alloc) — the evacuated copy.
     * Purely diagnostic. */
    if (gc_watch_hits((uintptr_t)np, (HEADER_WORDS(header) - 1) * WORDBYTES)) {
        fprintf(GC_LOG,
                "[GC WATCH-ALLOC #%ld] EVACUATE old_body=%p (page=%lu) -> new_body=%p (page=%lu) tag=%d\n",
                gc_collect_seq, (void *)cp, (unsigned long)GCP_to_PAGE(cp),
                (void *)np, (unsigned long)GCP_to_PAGE(np), type_tag);
        gc_backtrace(GC_LOG);
    }

    /* Write forwarding pointer in old header */
    cp[-1] = (uintptr_t)np;

    return np;
}

/* gc_move: public evacuation function.  Extracts the type tag from
 * the header and delegates to move_internal. */
void *gc_move(void *p) {
    uintptr_t *cp;
    uintptr_t page;
    uintptr_t header;

    if (p == NULL) return NULL;

    cp   = (uintptr_t *)p;
    page = GCP_to_PAGE(cp);

    /* Not in heap at all? */
    if (page < firstheappage || page > lastheappage)
        return p;

    /* Already in to-space? */
    if (space[page] == next_space) {
        /* During a nursery scavenge, old-gen objects (space==current_space==next_space)
         * must be queued for scanning — their bodies may contain nursery pointers
         * that need evacuation.  During a full collect, the Cheney queue already
         * covers all to-space pages. */
        if (in_scavenge) queue(page);
        return p;
    }

    /* Nursery object: copy to old-gen during a nursery scavenge,
     * leave untouched during a full collect. */
    if (gc_in_nursery(p)) {
        if (!in_scavenge) return p;          /* full collect: nursery untouched */
        if (space[page] != NURSERY) {
            /* Already promoted this scavenge OR old-gen in nursery range */
            uintptr_t header = cp[-1];
            if (FORWARDED(header)) return (void *)header;   /* already evacuated */
            return p;                                       /* old-gen, stay */
        }
        /* Nursery survivor: copy to old-gen (current_space), leave forwarding ptr. */
        uintptr_t header = cp[-1];
        if (FORWARDED(header)) return (void *)header;        /* re-visit via two refs */
        return move_internal(cp, HEADER_TYPE(header));
    }

    header = cp[-1];

    /* Already forwarded? */
    if (FORWARDED(header))
        return (void *)header;

    return move_internal(cp, HEADER_TYPE(header));
}

/* ---- public API --------------------------------------------------- */

void gc_init(uintptr_t heap_size) {
    char *heap;
    uintptr_t i;
    uintptr_t page_count;

    page_count = heap_size / PAGEBYTES;

    /* Reserve a larger mmap than the initial heap so we can grow logically
     * without mremap.  The extra VAS costs nothing on Linux (lazy commit).
     * Reserve 4GB to give the heap room to grow through several doublings
     * (256MB → 512MB → 1GB → 2GB → 4GB) without needing mremap at all. */
    heap_mmap_size = (heap_size * 16 > (4096ULL * 1024 * 1024))
                     ? heap_size * 16 + PAGEBYTES - 1
                     : 4096ULL * 1024 * 1024 + PAGEBYTES - 1;
    raw_heap_start = mmap(NULL, heap_mmap_size,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS,
                          -1, 0);
    if (raw_heap_start == MAP_FAILED) {
        fprintf(stderr, "gc_init: mmap failed for %zu bytes\n",
                (unsigned long)heap_mmap_size);
        exit(1);
    }

    heap = raw_heap_start;

    /* Page-align the heap start (should already be aligned from mmap) */
    if ((uintptr_t)heap & (PAGEBYTES - 1)) {
        heap += PAGEBYTES - ((uintptr_t)heap & (PAGEBYTES - 1));
    }

    firstheappage = GCP_to_PAGE(heap);
    lastheappage  = firstheappage + page_count - 1;
    heappages     = page_count;

    /* Allocate page metadata arrays */
    uintptr_t *space_ptr = calloc(page_count, sizeof(uintptr_t));
    uintptr_t *link_ptr  = calloc(page_count, sizeof(uintptr_t));
    uintptr_t *type_ptr  = calloc(page_count, sizeof(uintptr_t));
    uint8_t   *pq_ptr    = calloc(page_count, sizeof(uint8_t));

    if (!space_ptr || !link_ptr || !type_ptr || !pq_ptr) {
        fprintf(stderr, "gc_init: metadata alloc failed\n");
        exit(1);
    }

    /* Index them relative to firstheappage */
    space      = space_ptr - firstheappage;
    gc_link    = link_ptr  - firstheappage;
    type_page  = type_ptr  - firstheappage;
    page_queued = pq_ptr   - firstheappage;

    raw_space_ptr = space_ptr;
    raw_link_ptr  = link_ptr;
    raw_type_ptr  = type_ptr;
    raw_page_queued_ptr = pq_ptr;

    for (i = firstheappage; i <= lastheappage; i++) {
        space[i] = 0;
        gc_link[i]  = 0;
        type_page[i] = 0;
        page_queued[i] = 0;
    }

    /* Carve out the nursery region at the start of the heap.
     * Pages are tagged NURSERY and are never selected by
     * allocatepage's free-page scan.  Allocation still goes
     * exclusively through the old-gen path — the nursery is
     * reserved but not yet used for bump allocation. */
    nursery_first = firstheappage;
    nursery_last  = firstheappage + NURSERY_PAGES - 1;
    for (i = nursery_first; i <= nursery_last; i++)
        space[i] = NURSERY;

    /* Initialise the nursery bump allocator.  nursery_cur points to the
     * first byte of the nursery region; nursery_end is one past the last
     * byte.  The nursery is a contiguous 2 MB region — all 4096 pages
     * tagged NURSERY, no interleaving with old-gen pages. */
    nursery_cur = (char *)PAGE_to_GCP(nursery_first);
    nursery_end = (char *)PAGE_to_GCP(nursery_last + 1);

    current_space = 1;
    next_space    = 1;
    freepage      = firstheappage + NURSERY_PAGES;   /* start after nursery */
    allocatedpages = 0;
    queue_head     = 0;
}

__attribute__((noinline))
void *gc_alloc(size_t bytes, int type_tag) {
    /* Count every allocation request at this public entry point, by class.
     * type_tag is in [0,4] (GC_TYPE_RAW..GC_TYPE_CALLFRAME_ARRAY); guard the
     * index defensively in case a caller passes an out-of-range tag. */
    if ((unsigned)type_tag < 5) gc_alloc_class_count[type_tag]++;
    /* ---- nursery fast path (Phase 4b.2 — copying scavenge) ---- */
    /* The nursery bump path only handles SINGLE-page objects.  A multi-page
     * alloc (total > PAGEBYTES) must not go through the nursery: the bump
     * allocator writes the header and memsets the body across its whole page
     * range without checking space[] on intermediate pages, so it can
     * straddle a page already promoted to current_space (holding a live
     * closure's code array) and zero it.  Multi-page allocs fall through to
     * gcalloc_internal (old-gen), where allocatepage finds contiguous free
     * pages and never writes to a promoted neighbor.  This is where
     * multi-page objects end up after their first scavenge anyway. */
    if (bytes <= NURSERY_BYTES / 8 &&
        (((bytes + WORDBYTES - 1) / WORDBYTES + 1) * WORDBYTES) <= PAGEBYTES) {
        uintptr_t words = (bytes + WORDBYTES - 1) / WORDBYTES + 1;
        size_t total = words * WORDBYTES;
        int nursery_tried = 0;

        /* Pre-emptive nursery scavenge: fire when free space drops below
         * NURSERY_SCAVENGE_FREE_LOWATER, BEFORE the bump cursor is exhausted.
         * This decouples the scavenge trigger from the reactive path. */
        if (!in_scavenge &&
            (size_t)(nursery_end - nursery_cur) <= NURSERY_SCAVENGE_FREE_LOWATER) {
            collect_nursery("PREEMPTIVE");
            nursery_tried = 1;
            gc_preemptive_scavenge_count++;
        }

    nursery_retry:

        /* No-straddle guard: if this allocation would cross a nursery page
         * boundary, bump the cursor to the next page start and re-skip
         * (the next page may be a false-positive-pinned promoted page).
         * A straddling object's tail body lands at offset 0 of the next
         * page; when a later fresh object also starts there
         * (type_page=OBJECT), the scavenge drain scans it from offset 0,
         * misreads the tail body as a header -- a VAL_NUMBER's zero
         * padding => HEADER_WORDS==0 => the `if (hw == 0) break;` guard
         * (gc.c:907) aborts the page scan immediately, skipping every real
         * object on the page, truncating the Cheney trace and reclaiming
         * still-live nursery cells.  Page-aligning single-page objects
         * eliminates the straddle.  Objects larger than a page are left to
         * straddle (multi-page CONTINUED, scanned from their OBJECT head,
         * which always begins with a real header). */
        if (total <= (size_t)PAGEBYTES && nursery_cur < nursery_end) {
            uintptr_t s_addr = (uintptr_t)nursery_cur;
            uintptr_t e_addr = s_addr + total;
            if (GCP_to_PAGE(e_addr - 1) != GCP_to_PAGE(s_addr)) {
                nursery_cur = (char *)PAGE_to_GCP(GCP_to_PAGE(s_addr) + 1);
                goto nursery_retry;
            }
        }

        if ((size_t)(nursery_end - nursery_cur) >= total) {
            uintptr_t *header = (uintptr_t *)nursery_cur;
            *header = MAKE_HEADER(words, type_tag);

            /* Zero the body (matching gcalloc_internal semantics) */
            memset(header + 1, 0, (words - 1) * WORDBYTES);

            void *body = header + 1;

            /* Opt-in allocation watcher (--gc-watch-alloc) — nursery fast
             * path.  Purely diagnostic. */
            if (gc_watch_hits((uintptr_t)body, bytes)) {
                fprintf(GC_LOG,
                        "[GC WATCH-ALLOC #%ld] ALLOC-NURSERY body=%p bytes=%zu tag=%d\n",
                        gc_collect_seq, body, bytes, type_tag);
                gc_backtrace(GC_LOG);
            }

            nursery_cur += total;

            /* Set type_page markers for the nursery object.
             * Nursery objects are single-page (total <= PAGEBYTES) so the
             * forward loop never executes.  Kept for metadata consistency. */
            {
                uintptr_t first_page = GCP_to_PAGE(header);
                uintptr_t last_page  = GCP_to_PAGE((uintptr_t)nursery_cur - 1);
                type_page[first_page] = OBJECT;
                for (uintptr_t pg = first_page + 1; pg <= last_page; pg++)
                    type_page[pg] = CONTINUED;
            }

            return body;
        }

        /* Nursery full — collect and retry once. */
        if (!nursery_tried) {
            collect_nursery("REACTIVE");
            nursery_tried = 1;
            gc_reactive_scavenge_count++;
            goto nursery_retry;
        }

        /* No nursery space available — fall through to old-gen */
    }

    /* ---- old-gen path ---- */
    /* Trigger collection before allocation if the current semi-space
     * is getting full.  allocatepage() also triggers collect() as a
     * last resort, but pre-emptive collection here improves throughput
     * and keeps the heap from filling to the brink. */
    if (allocatedpages > 0 && allocatedpages > oldgen_collect_threshold() && !in_scavenge)
        collect("THRESHOLD");

    return gcalloc_internal(bytes, type_tag);
}

void *gc_alloc_atomic(size_t bytes) {
    return gc_alloc(bytes, GC_TYPE_RAW);
}

__attribute__((noinline))
void *gc_alloc_oldgen(size_t bytes, int type_tag) {
    /* Count every allocation request at this public entry point, by class. */
    if ((unsigned)type_tag < 5) gc_alloc_class_count[type_tag]++;
    /* Force allocation through the old-gen path, bypassing the nursery
     * entirely.  Used for large objects (frame_stack, big arrays) that
     * would never fit in the nursery and would fragment it. */
    if (allocatedpages > 0 && allocatedpages > oldgen_collect_threshold() && !in_scavenge)
        collect("ALLOC");

    return gcalloc_internal(bytes, type_tag);
}

long gc_allocatedpages(void) {
    return (long)allocatedpages;
}

/* Nursery instrumentation accessors (Phase 4b.2). */

int gc_nursery_is_empty(void) {
    /* Under copying scavenge, after a scavenge all nursery pages are
     * NURSERY-tagged.  (nursery_cur may have advanced past the start
     * due to post-scavenge allocations — check only page tags.) */
    for (uintptr_t pg = nursery_first; pg <= nursery_last; pg++)
        if (space[pg] != NURSERY) return 0;
    return 1;
}

long gc_nursery_capacity_pages(void) {
    return NURSERY_PAGES;
}

int gc_nursery_no_other_space(void) {
    uintptr_t other = (current_space == 1) ? 2 : 1;
    for (uintptr_t pg = nursery_first; pg <= nursery_last; pg++)
        if (space[pg] == other) return 0;
    return 1;
}

/* ---- precise-root API (Phase 4a) --------------------------------- */

/* Push/pop on the process-global shadow stack.  The stack is malloc'd
 * (C heap, not GC heap — never scanned/evacuated by the collector). */

#define SHADOW_STACK_INIT_CAP 64

static void shadow_stack_grow(void) {
    size_t nc = shadow_cap ? shadow_cap * 2 : SHADOW_STACK_INIT_CAP;
    GcRoot *np = (GcRoot *)realloc(shadow_stack, nc * sizeof(GcRoot));
    if (!np) { fprintf(stderr, "gc_root_push: realloc failed\n"); exit(1); }
    shadow_stack = np; shadow_cap = nc;
}

void gc_root_push_ptr(void **slot) {
    if (shadow_len >= shadow_cap) shadow_stack_grow();
    shadow_stack[shadow_len].kind = ROOT_PTR;
    shadow_stack[shadow_len].slot = slot;
    shadow_stack[shadow_len].np   = NULL;
    shadow_len++;
}

void gc_root_push_value(Value *vslot) {
    if (shadow_len >= shadow_cap) shadow_stack_grow();
    shadow_stack[shadow_len].kind = ROOT_VALUE;
    shadow_stack[shadow_len].slot = vslot;
    shadow_stack[shadow_len].np   = NULL;
    shadow_len++;
}

void gc_root_push_value_volatile(volatile Value *vslot) {
    if (shadow_len >= shadow_cap) shadow_stack_grow();
    shadow_stack[shadow_len].kind = ROOT_VALUE_VOLATILE;
    shadow_stack[shadow_len].slot = (void *)vslot;
    shadow_stack[shadow_len].np   = NULL;
    shadow_len++;
}

void gc_root_push_value_array(Value *base, int *np) {
    if (shadow_len >= shadow_cap) shadow_stack_grow();
    shadow_stack[shadow_len].kind = ROOT_VALUE_ARRAY;
    shadow_stack[shadow_len].slot = base;
    shadow_stack[shadow_len].np   = np;
    shadow_len++;
}

void gc_root_pop(void) {
    if (shadow_len) shadow_len--;
}

void gc_root_pop_to(size_t watermark) {
    shadow_len = watermark;  /* truncate — for longjmp unwind */
}

size_t gc_root_watermark(void) {
    return shadow_len;
}

void gc_register_global_table(void *table, int *len_p) {
    reg_global_table     = table;
    reg_global_table_len = len_p;
}

void gc_register_traced_code(Instr **arr, int *np) {
    reg_traced_code     = arr;
    reg_traced_code_len = np;
}
