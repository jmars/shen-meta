/*
 * gc.c — Cheney mostly-copying collector (Phase 1: single-space)
 *
 * Adapted from vendor/bartlett-gc/gc.c — replaces the uniform pointer-count
 * scan with a type-tag dispatch so the scavenger calls typed scanning
 * functions (gc_scan_value / gc_evacuate) provided by zincvm.c.
 *
 * Key differences from Bartlett:
 * - HEADER_PTRS repurposed as HEADER_TYPE (0-4, see gc.h)
 * - gc_alloc zeros the entire object body (not just pointer slots)
 * - No grow/shrink — fixed heap; OOM prints diagnostic and exits
 * - No global_ptr array (zincvm uses extra_roots for global_table)
 * - Public API: gc_alloc, gc_alloc_atomic, gc_realloc, gc_init, gc_move
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

/* page metadata (indexed by page number; allocated relative to firstheappage) */
static uintptr_t *space;   /* 0=free, 1=semi-space-1, 2=semi-space-2 */
static uintptr_t *gc_link;    /* Cheney queue links */
static uintptr_t *type_page; /* OBJECT / CONTINUED */

static uintptr_t  queue_head;
static uintptr_t  queue_tail;
static uintptr_t  current_space;
static uintptr_t  next_space;

/* raw pointers to malloc'd metadata for eventual teardown */
static char      *raw_heap_start;
static uintptr_t *raw_space_ptr;
static uintptr_t *raw_link_ptr;
static uintptr_t *raw_type_ptr;
static size_t     heap_mmap_size;   /* actual mmap size of the heap */

/* ---- pinned-page bitmap ------------------------------------------- */

static uint64_t *pinned_bits;
static size_t    pinned_bits_words;

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

/* ---- register-spill jmp_buf --------------------------------------- */

static jmp_buf gc_reg_buf;

/* ---- forward declarations ----------------------------------------- */

static void  collect(void);
static void  allocatepage(uintptr_t pages);
static void *gcalloc_internal(size_t bytes, int type_tag);
static void *move_internal(uintptr_t *cp, int type_tag);

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
        while (page > firstheappage && type_page[page] == CONTINUED) {
            allocatedpages++;
            space[page] = next_space;
            page_set_pinned(page);
            page--;
        }
        space[page] = next_space;
        allocatedpages++;
        page_set_pinned(page);
        queue(page);
    }
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

    /* ---- root set ---- */

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
 * gc_init to be large enough (Bartlett reserves 4GB of VAS so growth is a
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
        allocatedpages + pages >= heappages / 2) {
        collect();
        if (allocatedpages + pages >= heappages / 2) {
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
     * free (not in current_space, not in next_space, not pinned) pages. */
    while (allpages--) {
        if (space[freepage] != current_space &&
            space[freepage] != next_space &&
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

    /* Index them relative to firstheappage (Bartlett's trick) */
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

    stackbase = (uintptr_t *)stack_base;

    current_space = 1;
    next_space    = 1;
    freepage      = firstheappage;
    allocatedpages = 0;
    queue_head     = 0;

    /* Pinned-page bitmap */
    pinned_bits_words = (heap_mmap_size / PAGEBYTES + 63) / 64;
    pinned_bits = calloc(pinned_bits_words, sizeof(uint64_t));
    if (!pinned_bits) {
        fprintf(stderr, "gc_init: pinned bitmap alloc failed\n");
        exit(1);
    }

    n_extra_roots = 0;
}

__attribute__((noinline))
void *gc_alloc(size_t bytes, int type_tag) {
    /* Trigger collection before allocation if the current semi-space
     * is getting full.  allocatepage() also triggers collect() as a
     * last resort, but pre-emptive collection here improves throughput
     * and keeps the heap from filling to the brink. */
    if (allocatedpages > 0 && allocatedpages > heappages / 4)
        collect();

    return gcalloc_internal(bytes, type_tag);
}

void *gc_alloc_atomic(size_t bytes) {
    return gc_alloc(bytes, GC_TYPE_RAW);
}

__attribute__((noinline))
void *gc_realloc(void *old, size_t old_bytes, size_t new_bytes, int type_tag) {
    void *newp = gc_alloc(new_bytes, type_tag);
    if (old_bytes > 0 && old != NULL) {
        size_t copy = old_bytes < new_bytes ? old_bytes : new_bytes;
        memcpy(newp, old, copy);
    }
    return newp;
}
