/*
 * gc.c — Cheney mostly-copying collector (Phase 1: single-space)
 *
 * Replaces the uniform pointer-count scan of a Bartlett-style collector with
 * a type-tag dispatch so the scavenger calls typed scanning functions
 * (gc_scan_value / gc_evacuate) provided by zincvm.c.
 *
 * Design notes:
 * - HEADER_PTRS repurposed as HEADER_TYPE (0-4, see gc.h)
 * - gc_alloc zeros the entire object body (not just pointer slots)
 * - No grow/shrink — fixed heap; OOM prints diagnostic and exits
 * - No global_ptr array (zincvm uses extra_roots for global_table)
 * - Public API: gc_alloc, gc_alloc_atomic, gc_init, gc_move
 * - gc_alloc marked __attribute__((noinline)) to spill registers
 * - SIGALRM blocked during collection (zincvm uses alarm for test timeouts)
 */

#include "gc.h"
#include "zinctypes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <signal.h>
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

#define STACKINC  sizeof(uintptr_t)

/* ---- static state ------------------------------------------------- */

static uintptr_t *stackbase;

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

/* raw pointers to malloc'd metadata for eventual teardown */
static char      *raw_heap_start;
static uintptr_t *raw_space_ptr;
static uintptr_t *raw_link_ptr;
static uintptr_t *raw_type_ptr;
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

/* ---- pinned-page bitmap ------------------------------------------- */

static uint64_t *pinned_bits;
static size_t    pinned_bits_words;

#ifdef GC_ROOTS_DIFF
static uint64_t *precise_pinned;         /* pages pinned by precise roots */
static int       gc_diff_phase = 0;     /* 0=off, 1=precise, 2=conservative */
static long      gc_diff_missed = 0;    /* pages pinned only by conservative scan */
static uintptr_t gc_diff_missed_pages[16]; /* first few missed page #s */
static int       gc_diff_missed_count = 0;

static void gc_diff_record_page(uintptr_t page) {
    if (page < firstheappage || page > lastheappage) return;
    size_t idx = page - firstheappage;
    if (gc_diff_phase == 1) {
        precise_pinned[idx / 64] |= (uint64_t)1 << (idx % 64);
    } else if (gc_diff_phase == 2) {
        if (!((precise_pinned[idx / 64] >> (idx % 64)) & 1)) {
            gc_diff_missed++;
            /* Dedupe for the report array */
            int found = 0;
            for (int i = 0; i < gc_diff_missed_count; i++)
                if (gc_diff_missed_pages[i] == page) { found = 1; break; }
            if (!found && gc_diff_missed_count < 16)
                gc_diff_missed_pages[gc_diff_missed_count++] = page;
        }
    }
}
#endif

static int page_is_pinned(uintptr_t page) {
    if (!pinned_bits) return 0;
    if (page < firstheappage || page > lastheappage) return 0;
    size_t idx = page - firstheappage;
    return (pinned_bits[idx / 64] >> (idx % 64)) & 1;
}

static void page_set_pinned(uintptr_t page) {
    if (!pinned_bits) return;
    if (page < firstheappage || page > lastheappage) return;
    size_t idx = page - firstheappage;
    pinned_bits[idx / 64] |= (uint64_t)1 << (idx % 64);
}

static void pinned_clear_all(void) {
    if (pinned_bits)
        memset(pinned_bits, 0, pinned_bits_words * sizeof(uint64_t));
}

static void pinned_clear_nursery(void) {
    if (!pinned_bits) return;
    for (uintptr_t p = nursery_first; p <= nursery_last; p++) {
        size_t idx = p - firstheappage;
        pinned_bits[idx / 64] &= ~((uint64_t)1 << (idx % 64));
    }
}

/* ---- nursery / old-gen predicates --------------------------------- */

int gc_in_nursery(void *p) {
    uintptr_t page = GCP_to_PAGE(p);
    return (page >= nursery_first && page <= nursery_last);
}

/* gc_in_oldgen: true iff p's page is in the live old-gen semi-space
 * (space == current_space).  Under pin-in-place, a nursery page promoted
 * by a scavenge keeps its nursery-range ADDRESS but its space becomes
 * current_space, so the correct old-gen test is the space tag, NOT the
 * address range (a page > nursery_last would miss promoted-in-place
 * arrays, which is exactly the case the write barrier must cover). */
int gc_in_oldgen(void *p) {
    uintptr_t page = GCP_to_PAGE(p);
    return (page >= firstheappage && page <= lastheappage &&
            space[page] == current_space);
}

/* first_free_nursery_page: return the first page in the nursery
 * region that still has space==NURSERY, or nursery_last+1 if the
 * entire nursery has been promoted. */
static uintptr_t first_free_nursery_page(void) {
    uintptr_t pg = nursery_first;
    while (pg <= nursery_last && space[pg] != NURSERY)
        pg++;
    return pg;
}

/* ---- extra roots -------------------------------------------------- */

#define MAX_EXTRA_ROOTS 8
static struct { void *start; size_t size; } extra_roots[MAX_EXTRA_ROOTS];
static int n_extra_roots = 0;

void gc_set_extra_roots(void *start, size_t size) {
    if (n_extra_roots >= MAX_EXTRA_ROOTS) {
        fprintf(stderr, "gc_set_extra_roots: too many ranges (max %d)\n",
                MAX_EXTRA_ROOTS);
        exit(1);
    }
    extra_roots[n_extra_roots].start = start;
    extra_roots[n_extra_roots].size  = size;
    n_extra_roots++;
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

/* ---- register-spill jmp_buf --------------------------------------- */

static jmp_buf gc_reg_buf;

/* ---- forward declarations ----------------------------------------- */

static void  collect(void);
static void  collect_nursery(void);
static void  allocatepage(uintptr_t pages);
static void *gcalloc_internal(size_t bytes, int type_tag);
static void *move_internal(uintptr_t *cp, int type_tag);
static void  gc_scan_roots(int use_nursery);

/* ---- Cheney queue ------------------------------------------------- */

static uintptr_t next_page(uintptr_t page) {
    return (page == lastheappage) ? firstheappage : page + 1;
}

static void queue(uintptr_t page) {
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

/* ---- move / pin --------------------------------------------------- */

/* pin_page: mark a page and its CONTINUED predecessors as in next_space
 * so they survive this collection.  Also sets the pinned bitmap so
 * these pages stay pinned even after a space swap. */
static void pin_page(uintptr_t page) {
    if (page >= firstheappage &&
        page <= lastheappage &&
        space[page] == current_space)
    {
        /* Save the original input page: the backward walk below
         * decrements `page` to the OBJECT (head) page, and the forward
         * walk must resume from the INPUT page, not the head.  If the
         * input is a tail page (a raw C-stack pointer into the middle
         * or tail of a multi-page object), the backward walk already
         * pinned [head .. input]; starting the forward walk at head+1
         * would find space[head+1]==next_space and stop after zero
         * iterations, leaving pages BEYOND the input unpinned. */
        uintptr_t input = page;
        while (page > firstheappage && type_page[page] == CONTINUED) {
            allocatedpages++;
            space[page] = next_space;
            page_set_pinned(page);
#ifdef GC_ROOTS_DIFF
            gc_diff_record_page(page);
#endif
            page--;
        }
        space[page] = next_space;
        allocatedpages++;
        page_set_pinned(page);
#ifdef GC_ROOTS_DIFF
        gc_diff_record_page(page);
#endif
        queue(page);

        /* Forward-walk CONTINUED tail pages of the same multi-page
         * object and pin them in place.  They are scanned as part of
         * the head page's body during the Cheney drain (cp += hw
         * crosses the page boundary), so DO NOT queue them — only pin
         * so they survive the post-flip free-page scan.  Without this
         * a multi-page old-gen object reached via its head page would
         * have its tail pages reclaimed and overwritten after collect.
         * Start at input+1 (not head+1): the backward walk pinned
         * [head..input] already, and the pages beyond the input tail
         * page are the ones that still need pinning. */
        uintptr_t t = input + 1;
        while (t <= lastheappage &&
               type_page[t] == CONTINUED &&
               space[t] == current_space) {
            space[t] = next_space;
            allocatedpages++;
            page_set_pinned(t);
#ifdef GC_ROOTS_DIFF
            gc_diff_record_page(t);
#endif
            t++;
        }
    }
}

/* pin_nursery_page: pin a nursery page in place (no copy, no flip).
 * Walks backward to find the OBJECT (head) page, then forward through
 * all CONTINUED successors, promoting every page from NURSERY to
 * current_space.  Only the OBJECT page is queued for scanning. */
static void pin_nursery_page(uintptr_t page) {
    if (page < nursery_first || page > nursery_last) return;
    if (space[page] != NURSERY) return;

    /* Walk backward to the OBJECT (head) page */
    uintptr_t head = page;
    while (head > nursery_first && type_page[head] == CONTINUED)
        head--;

    /* Pin the head page */
    space[head] = current_space;
    page_set_pinned(head);
#ifdef GC_ROOTS_DIFF
    gc_diff_record_page(head);
#endif
    queue(head);
    allocatedpages++;

    /* Walk forward, pinning CONTINUED successors */
    uintptr_t p = head + 1;
    while (p <= nursery_last && type_page[p] == CONTINUED &&
           space[p] == NURSERY) {
        space[p] = current_space;
        page_set_pinned(p);
#ifdef GC_ROOTS_DIFF
        gc_diff_record_page(p);
#endif
        allocatedpages++;
        p++;
    }

    /* Defensive-only: the nursery fast path is gated so a single-page
     * object is the maximum that can ever be allocated in the nursery
     * (gc.c gc_alloc: total <= PAGEBYTES).  Multi-page objects fall
     * through to gcalloc_internal / old-gen and can never land in the
     * nursery, so type_page is never CONTINUED in the nursery range and
     * the forward walk above never executes.  Kept for symmetry with
     * pin_page's forward walk, not because nursery objects can span pages. */
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

static void collect(void) {
    uintptr_t *fp;
    uintptr_t  i;
    sigset_t   old_sig_set;

    /* spill callee-saved registers to the stack so the conservative
     * C-stack scan can find them */
    (void)setjmp(gc_reg_buf);

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

    /* Finalize any partial page */
    if (freewords != 0) {
        *freep = MAKE_HEADER(freewords, 0);
        freewords = 0;
    }

    /* Swap semi-spaces */
    next_space = (current_space == 1) ? 2 : 1;
    allocatedpages = 0;
    queue_head = 0;
    pinned_clear_all();
    gc_dirty_vectors_clear();

    /* ---- root set ---- */

#ifdef GC_ROOTS_DIFF
    /* Reset diff state */
    memset(precise_pinned, 0, pinned_bits_words * sizeof(uint64_t));
    gc_diff_missed = 0;
    gc_diff_missed_count = 0;

    /* Phase 1: precise roots only */
    gc_diff_phase = 1;
    gc_scan_roots(0);
    gc_diff_phase = 2;

    /* Phase 2: conservative C-stack scan + extra roots */
    for (fp = (uintptr_t *)(&fp);
         fp <= stackbase;
         fp = (uintptr_t *)((char *)fp + STACKINC))
    {
        pin_page(GCP_to_PAGE(*fp));
    }

    for (i = 0; i < (uintptr_t)n_extra_roots; i++) {
        uintptr_t *p   = (uintptr_t *)extra_roots[i].start;
        uintptr_t *end = (uintptr_t *)((char *)extra_roots[i].start +
                                       extra_roots[i].size);
        for (; p < end; p++) {
            pin_page(GCP_to_PAGE(*p));
        }
    }

    gc_diff_phase = 0;

    /* Diagnostic report */
    {
        uintptr_t pinned_count = 0;
        if (pinned_bits) {
            for (size_t bi = 0; bi < pinned_bits_words; bi++) {
                uint64_t w = pinned_bits[bi];
                while (w) { pinned_count += (w & 1); w >>= 1; }
            }
        }
        fprintf(stderr, "GC_ROOTS_DIFF: collect(): conservative pinned pages = %zu, "
                "precise roots = %zu, "
                "global_table_len = %d, num_traced = %d\n",
                (size_t)pinned_count, shadow_len,
                reg_global_table_len ? *reg_global_table_len : 0,
                reg_traced_code_len ? *reg_traced_code_len : 0);
        if (gc_diff_missed == 0) {
            fprintf(stderr, "GC_ROOTS_DIFF: P_cons subset P_prec (no missed roots)\n");
        } else {
            fprintf(stderr, "GC_ROOTS_DIFF: %ld pages pinned only by conservative scan (list):",
                    gc_diff_missed);
            for (int di = 0; di < gc_diff_missed_count; di++)
                fprintf(stderr, " %lu", (unsigned long)gc_diff_missed_pages[di]);
            fprintf(stderr, "\n");
        }
    }
#else
    /* 1. conservative C-stack scan */
    for (fp = (uintptr_t *)(&fp);
         fp <= stackbase;
         fp = (uintptr_t *)((char *)fp + STACKINC))
    {
        pin_page(GCP_to_PAGE(*fp));
    }

    /* 2. extra root ranges (global_table, traced_code, etc.) */
    for (i = 0; i < (uintptr_t)n_extra_roots; i++) {
        uintptr_t *p   = (uintptr_t *)extra_roots[i].start;
        uintptr_t *end = (uintptr_t *)((char *)extra_roots[i].start +
                                       extra_roots[i].size);
        for (; p < end; p++) {
            pin_page(GCP_to_PAGE(*p));
        }
    }

    /* 3. Precise roots (additive pinning — 4a invariant) */
    gc_scan_roots(0);
#endif

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

    /* Restore the previous SIGALRM mask */
    sigprocmask(SIG_SETMASK, &old_sig_set, NULL);
}

/* ---- nursery collection (Phase 2 Step 3 — pin-in-place) ------------- */

/* Root-scan dispatcher for the nursery scavenge: routes nursery pages
 * to pin_nursery_page and old-gen pages directly to the Cheney queue.
 * Does NOT call pin_page because in a no-flip scavenge next_space ==
 * current_space and pin_page would double-count allocatedpages. */
static void nursery_root_pin(uintptr_t page) {
    if (page < firstheappage || page > lastheappage) return;
    if (page >= nursery_first && page <= nursery_last) {
        if (space[page] == NURSERY) {
            pin_nursery_page(page);
        } else if (space[page] != 0) {
            /* Already promoted (current_space or the other
             * semi-space from a prior cycle).  Queue for scanning
             * so we find this object's refs into new nursery pages. */
#ifdef GC_ROOTS_DIFF
            gc_diff_record_page(page);
#endif
            queue(page);
        }
    } else if (space[page] == current_space) {
#ifdef GC_ROOTS_DIFF
        gc_diff_record_page(page);
#endif
        queue(page);
    }
}

/* gc_pin_value: pin all GC-managed pages reachable from a Value's
 * interior pointers.  Mirrors gc_scan_value's field list exactly.
 * use_nursery=1 → nursery_root_pin; use_nursery=0 → pin_page.
 * ADDITIVE only — never evacuates (4a invariant). */
static void gc_pin_value(Value *v, int use_nursery) {
    switch (v->tag) {
    case VAL_CONS:
        if (v->cons.car) {
            uintptr_t pg = GCP_to_PAGE(v->cons.car);
            if (use_nursery) nursery_root_pin(pg); else pin_page(pg);
        }
        if (v->cons.cdr) {
            uintptr_t pg = GCP_to_PAGE(v->cons.cdr);
            if (use_nursery) nursery_root_pin(pg); else pin_page(pg);
        }
        break;
    case VAL_LAMBDA:
        if (v->lambda.code) {
            uintptr_t pg = GCP_to_PAGE(v->lambda.code);
            if (use_nursery) nursery_root_pin(pg); else pin_page(pg);
        }
        if (v->lambda.env) {
            uintptr_t pg = GCP_to_PAGE(v->lambda.env);
            if (use_nursery) nursery_root_pin(pg); else pin_page(pg);
        }
        break;
    case VAL_VECTOR:
        if (v->vector.data) {
            uintptr_t pg = GCP_to_PAGE(v->vector.data);
            if (use_nursery) nursery_root_pin(pg); else pin_page(pg);
        }
        break;
    case VAL_STRING:
        if (v->str.data) {
            uintptr_t pg = GCP_to_PAGE(v->str.data);
            if (use_nursery) nursery_root_pin(pg); else pin_page(pg);
        }
        break;
    case VAL_ERROR:
        if (v->error.message) {
            uintptr_t pg = GCP_to_PAGE(v->error.message);
            if (use_nursery) nursery_root_pin(pg); else pin_page(pg);
        }
        break;
    /* VAL_NUMBER, VAL_SYMBOL, VAL_BOOLEAN, VAL_NIL, VAL_MARK,
     * VAL_PRIM, VAL_STREAM contain no GC-managed pointers */
    default:
        break;
    }
}

/* gc_scan_roots: walk the precise-root shadow stack + typed walkers,
 * pinning all referenced pages.  ADDITIVE only — never evacuates
 * (4a invariant).  Called AFTER the conservative scan.
 * use_nursery=1 → nursery_root_pin (for collect_nursery);
 * use_nursery=0 → pin_page (for collect). */
static void gc_scan_roots(int use_nursery) {
    /* 1. Shadow stack entries */
    for (size_t i = 0; i < shadow_len; i++) {
        GcRoot *r = &shadow_stack[i];
        switch (r->kind) {
        case ROOT_PTR: {
            void *p = *(void **)r->slot;
            if (p) {
                uintptr_t pg = GCP_to_PAGE(p);
                if (use_nursery) nursery_root_pin(pg); else pin_page(pg);
            }
            break;
        }
        case ROOT_VALUE:
            gc_pin_value((Value *)r->slot, use_nursery);
            break;
        case ROOT_VALUE_VOLATILE: {
            volatile Value *vs = (volatile Value *)r->slot;
            Value tmp = *vs;
            gc_pin_value(&tmp, use_nursery);
            break;
        }
        case ROOT_VALUE_ARRAY: {
            Value *base = (Value *)r->slot;
            int n = *(r->np);
            for (int j = 0; j < n; j++)
                gc_pin_value(&base[j], use_nursery);
            break;
        }
        }
    }

    /* 2. Typed walker: global_table closures */
    if (reg_global_table && reg_global_table_len) {
        GlobalEntry *gt = (GlobalEntry *)reg_global_table;
        int n = *reg_global_table_len;
        for (int i = 0; i < n; i++)
            gc_pin_value(&gt[i].closure, use_nursery);
    }

    /* 3. Typed walker: traced_code Instr arrays */
    if (reg_traced_code && reg_traced_code_len) {
        int n = *reg_traced_code_len;
        for (int i = 0; i < n; i++) {
            if (reg_traced_code[i]) {
                uintptr_t pg = GCP_to_PAGE(reg_traced_code[i]);
                if (use_nursery) nursery_root_pin(pg); else pin_page(pg);
            }
        }
    }
}

/* collect_nursery: no-flip pin-in-place nursery scavenge.
 *
 * Does NOT swap semi-spaces, does NOT reset allocatedpages, does NOT
 * clear old-gen pinned bits.  Roots (C stack + extra_roots) are scanned
 * conservatively, then the write-barrier dirty-vectors remembered set
 * (Phase 2 Step 5) is scanned inline for old-gen→nursery references.
 * Nursery survivors are pinned in place (space ← current_space).
 * Finally the bump cursor is reset to the first still-NURSERY page. */
static void collect_nursery(void) {
    uintptr_t *fp;
    uintptr_t  i;
    sigset_t   old_sig_set;
    uintptr_t  saved_allocatedpages;   /* for post-scavenge assertion */

    /* Guard against recursive entry */
    if (in_scavenge) {
        fprintf(stderr, "collect_nursery: re-entered during scavenge\n");
        exit(1);
    }

    /* Spill callee-saved registers to the stack */
    (void)setjmp(gc_reg_buf);

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

    /* Short-circuit: if the nursery has no NURSERY-tagged pages at all,
     * there are no nursery objects to evacuate and no old-gen→nursery
     * references to scan.  Just reset the bump cursor and return. */
    if (first_free_nursery_page() > nursery_last) {
        nursery_cur = nursery_end;
        in_scavenge = 0;
        sigprocmask(SIG_SETMASK, &old_sig_set, NULL);
        return;
    }

    /* Count this as a real scavenge (past the exhausted-nursery short-circuit). */
    gc_nursery_scavenge_count++;

    /* Clear only the nursery portion of the pinned bitmap */
    pinned_clear_nursery();

    /* Reset the Cheney queue */
    queue_head = 0;

    saved_allocatedpages = allocatedpages;

    /* ---- root set ---- */

#ifdef GC_ROOTS_DIFF
    /* Reset diff state */
    memset(precise_pinned, 0, pinned_bits_words * sizeof(uint64_t));
    gc_diff_missed = 0;
    gc_diff_missed_count = 0;

    /* Phase 1: precise roots only */
    gc_diff_phase = 1;
    gc_scan_roots(1);
    gc_diff_phase = 2;

    /* Phase 2: conservative C-stack scan + extra roots */
    for (fp = (uintptr_t *)(&fp);
         fp <= stackbase;
         fp = (uintptr_t *)((char *)fp + STACKINC))
    {
        nursery_root_pin(GCP_to_PAGE(*fp));
    }

    for (i = 0; i < (uintptr_t)n_extra_roots; i++) {
        uintptr_t *p   = (uintptr_t *)extra_roots[i].start;
        uintptr_t *end = (uintptr_t *)((char *)extra_roots[i].start +
                                       extra_roots[i].size);
        for (; p < end; p++) {
            nursery_root_pin(GCP_to_PAGE(*p));
        }
    }

    gc_diff_phase = 0;

    /* Diagnostic report */
    {
        uintptr_t pinned_count = 0;
        if (pinned_bits) {
            for (size_t bi = 0; bi < pinned_bits_words; bi++) {
                uint64_t w = pinned_bits[bi];
                while (w) { pinned_count += (w & 1); w >>= 1; }
            }
        }
        if (gc_diff_missed == 0) {
            fprintf(stderr, "GC_ROOTS_DIFF: collect_nursery(): P_cons subset P_prec (no missed roots) "
                    "(total pinned=%zu, precise roots=%zu)\n",
                    (size_t)pinned_count, shadow_len);
        } else {
            fprintf(stderr, "GC_ROOTS_DIFF: collect_nursery(): %ld pages pinned only by conservative scan (list):",
                    gc_diff_missed);
            for (int di = 0; di < gc_diff_missed_count; di++)
                fprintf(stderr, " %lu", (unsigned long)gc_diff_missed_pages[di]);
            fprintf(stderr, " (total pinned=%zu, precise roots=%zu)\n",
                    (size_t)pinned_count, shadow_len);
        }
    }
#else
    /* 1. Conservative C-stack scan */
    for (fp = (uintptr_t *)(&fp);
         fp <= stackbase;
         fp = (uintptr_t *)((char *)fp + STACKINC))
    {
        nursery_root_pin(GCP_to_PAGE(*fp));
    }

    /* 2. Extra root ranges (global_table, traced_code, etc.) */
    for (i = 0; i < (uintptr_t)n_extra_roots; i++) {
        uintptr_t *p   = (uintptr_t *)extra_roots[i].start;
        uintptr_t *end = (uintptr_t *)((char *)extra_roots[i].start +
                                       extra_roots[i].size);
        for (; p < end; p++) {
            nursery_root_pin(GCP_to_PAGE(*p));
        }
    }

    /* 3. Precise roots (additive pinning — 4a invariant) */
    gc_scan_roots(1);
#endif

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

    /* ---- queue previously-promoted nursery pages ---- */
    /* Nursery pages promoted in a prior scavenge cycle (before a
     * full collect flipped current_space) have space == other_space.
     * They contain live objects that may reference newly-allocated
     * nursery pages and must be scanned. */
    {
        uintptr_t other_space = (current_space == 1) ? 2 : 1;
        for (uintptr_t pg = nursery_first; pg <= nursery_last; pg++) {
            if (space[pg] == other_space && type_page[pg] == OBJECT)
                queue(pg);
        }
    }

    /* ---- Cheney scavenge ---- */
    /* Nursery pages stop at nursery_cur (the allocation frontier);
     * old-gen pages stop at freep (the old-gen allocation frontier). */

    while (queue_head != 0) {
        uintptr_t qpg  = queue_head;
        int is_nursery = (qpg >= nursery_first && qpg <= nursery_last);
        uintptr_t *cp  = PAGE_to_GCP(qpg);
        uintptr_t *limit;

        if (is_nursery)
            limit = (uintptr_t *)nursery_cur;
        else
            limit = freep;

        while (GCP_to_PAGE(cp) == qpg && cp != limit) {
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

    /* ---- reset nursery bump cursor ---- */
    {
        uintptr_t pg = first_free_nursery_page();
        char *new_cur;
        if (pg <= nursery_last)
            new_cur = (char *)PAGE_to_GCP(pg);
        else
            new_cur = nursery_end;
        /* Pages freed by rewinding the cursor back to the first free page.
         * The cursor only ever moves backward here (to the first page that is
         * still NURSERY), so reclaimed = old cursor - new cursor. */
        if (new_cur < nursery_cur)
            gc_nursery_pages_reclaimed += (long)((nursery_cur - new_cur) / PAGEBYTES);
        nursery_cur = new_cur;
    }

    /* ---- assertions ---- */

    /* Invariant 1: every nursery page is NURSERY, current_space, or
     * the other semi-space.  The "other" semi-space (1 or 2, whichever
     * is not current_space) holds nursery pages that were promoted in
     * a previous nursery scavenge before a full collect flipped
     * current_space.  These pages are still live and must be accepted. */
    {
        uintptr_t other_space = (current_space == 1) ? 2 : 1;
        for (uintptr_t pg = nursery_first; pg <= nursery_last; pg++) {
            if (space[pg] != NURSERY &&
                space[pg] != current_space &&
                space[pg] != other_space) {
                fprintf(stderr,
                        "collect_nursery: invariant violation: "
                        "nursery page %lu has space=%lu "
                        "(expected %lu=NURSERY, %lu=current, or %lu=other)\n",
                        (unsigned long)pg, (unsigned long)space[pg],
                        (unsigned long)NURSERY, (unsigned long)current_space,
                        (unsigned long)other_space);
                exit(1);
            }
        }
    }

    /* Invariant 2: allocatedpages never shrinks during a scavenge */
    if (allocatedpages < saved_allocatedpages) {
        fprintf(stderr,
                "collect_nursery: invariant violation: "
                "allocatedpages shrank from %lu to %lu\n",
                (unsigned long)saved_allocatedpages,
                (unsigned long)allocatedpages);
        exit(1);
    }

    gc_dirty_vectors_clear();

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
        if (!new_space || !new_link || !new_type) return -1;

        raw_space_ptr = new_space;
        raw_link_ptr  = new_link;
        raw_type_ptr  = new_type;
        space  = new_space - firstheappage;
        gc_link = new_link  - firstheappage;
        type_page = new_type  - firstheappage;

        uintptr_t old_last = lastheappage;
        lastheappage = firstheappage + new_heappages - 1;
        heappages = new_heappages;
        for (uintptr_t i = old_last + 1; i <= lastheappage; i++) {
            space[i] = 0; gc_link[i] = 0; type_page[i] = 0;
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
        collect();
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
     * free (not in current_space, not in next_space, not nursery,
     * not pinned) pages. */
    while (allpages--) {
        if (space[freepage] != current_space &&
            space[freepage] != next_space &&
            space[freepage] != NURSERY &&
            !page_is_pinned(freepage))
        {
            if (free++ == 0)
                firstpage = freepage;

            if (free == pages) {
                freep = PAGE_to_GCP(firstpage);

                if (current_space != next_space)
                    queue(firstpage);

                freewords = pages * PAGEWORDS;
                allocatedpages += pages;
                freepage = next_page(freepage);
                space[firstpage] = next_space;
                type_page[firstpage] = OBJECT;

                while (--pages) {
                    space[++firstpage] = next_space;
                    type_page[firstpage] = CONTINUED;
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
    if (space[page] == next_space)
        return p;

    /* Nursery object: pin in place, never copy.  During a nursery
     * scavenge (in_scavenge set), promote still-NURSERY pages to
     * current_space so they survive.  During a full collect the
     * nursery is untouched — just return p. */
    if (gc_in_nursery(p)) {
        if (in_scavenge && space[page] == NURSERY)
            pin_nursery_page(page);
        return p;
    }

    header = cp[-1];

    /* Already forwarded? */
    if (FORWARDED(header))
        return (void *)header;

    return move_internal(cp, HEADER_TYPE(header));
}

/* ---- public API --------------------------------------------------- */

void gc_init(uintptr_t heap_size, void *stack_base) {
    char *heap;
    uintptr_t i;
    uintptr_t page_count;

    page_count = heap_size / PAGEBYTES;

    /* Reserve a larger mmap than the initial heap so we can grow logically
     * without mremap.  The extra VAS costs nothing on Linux (lazy commit).
     * Reserve 4GB to give the heap room to grow through several doublings
     * (256MB → 512MB → 1GB → 2GB → 4GB) without needing mremap at all.
     * Even with conservative stack scan false positives inflating the live
     * set during deep call chains, 4GB is plenty of headroom. */
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

    if (!space_ptr || !link_ptr || !type_ptr) {
        fprintf(stderr, "gc_init: metadata alloc failed\n");
        exit(1);
    }

    /* Index them relative to firstheappage */
    space      = space_ptr - firstheappage;
    gc_link    = link_ptr  - firstheappage;
    type_page  = type_ptr  - firstheappage;

    raw_space_ptr = space_ptr;
    raw_link_ptr  = link_ptr;
    raw_type_ptr  = type_ptr;

    for (i = firstheappage; i <= lastheappage; i++) {
        space[i] = 0;
        gc_link[i]  = 0;
        type_page[i] = 0;
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

    stackbase = (uintptr_t *)stack_base;

    current_space = 1;
    next_space    = 1;
    freepage      = firstheappage + NURSERY_PAGES;   /* start after nursery */
    allocatedpages = 0;
    queue_head     = 0;

    /* Pinned-page bitmap */
    pinned_bits_words = (heap_mmap_size / PAGEBYTES + 63) / 64;
    pinned_bits = calloc(pinned_bits_words, sizeof(uint64_t));
    if (!pinned_bits) {
        fprintf(stderr, "gc_init: pinned bitmap alloc failed\n");
        exit(1);
    }

#ifdef GC_ROOTS_DIFF
    precise_pinned = calloc(pinned_bits_words, sizeof(uint64_t));
    if (!precise_pinned) {
        fprintf(stderr, "gc_init: precise_pinned bitmap alloc failed\n");
        exit(1);
    }
#endif

    n_extra_roots = 0;
}

__attribute__((noinline))
void *gc_alloc(size_t bytes, int type_tag) {
    /* ---- nursery fast path (Phase 2 Step 3) ---- */
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
        if (!in_scavenge && first_free_nursery_page() <= nursery_last &&
            (size_t)(nursery_end - nursery_cur) <= NURSERY_SCAVENGE_FREE_LOWATER) {
            collect_nursery();
            nursery_tried = 1;
            gc_preemptive_scavenge_count++;
        }

    nursery_retry:
        /* Skip promoted (non-NURSERY) pages that were pinned by a
         * prior collection.  This keeps the bump cursor on free space. */
        while (nursery_cur < nursery_end) {
            uintptr_t pg = GCP_to_PAGE(nursery_cur);
            if (pg > nursery_last) break;
            if (space[pg] == NURSERY) break;
            nursery_cur = (char *)PAGE_to_GCP(pg + 1);
        }

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
            nursery_cur += total;

            /* Set type_page markers for multi-page nursery objects.
             * Mirrors allocatepage's old-gen CONTINUED logic so that
             * pin_nursery_page's backward/forward walk correctly
             * identifies all pages of a large object. */
            {
                uintptr_t first_page = GCP_to_PAGE(header);
                uintptr_t last_page  = GCP_to_PAGE((uintptr_t)nursery_cur - 1);
                type_page[first_page] = OBJECT;
                for (uintptr_t pg = first_page + 1; pg <= last_page; pg++)
                    type_page[pg] = CONTINUED;
            }

            return body;
        }

        /* Nursery full — collect and retry once.
         * Only call collect_nursery() if the nursery actually has
         * NURSERY-tagged pages to scavenge.  Once all nursery pages
         * are promoted, the nursery is permanently exhausted and every
         * subsequent collect_nursery() would be a wasted full old-gen
         * scan — skip straight to the old-gen path. */
        if (!nursery_tried && first_free_nursery_page() <= nursery_last) {
            collect_nursery();
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
        collect();

    return gcalloc_internal(bytes, type_tag);
}

void *gc_alloc_atomic(size_t bytes) {
    return gc_alloc(bytes, GC_TYPE_RAW);
}

__attribute__((noinline))
void *gc_alloc_oldgen(size_t bytes, int type_tag) {
    /* Force allocation through the old-gen path, bypassing the nursery
     * entirely.  Used for large objects (frame_stack, big arrays) that
     * would never fit in the nursery and would fragment it. */
    if (allocatedpages > 0 && allocatedpages > oldgen_collect_threshold() && !in_scavenge)
        collect();

    return gcalloc_internal(bytes, type_tag);
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
