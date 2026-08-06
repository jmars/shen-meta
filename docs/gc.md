# GC Plan for zincvm — moving generational collector

Status: approved design. Companion doc: `moving-gc-validation.md` (advisor
validation, hazards, full line references).

## Why

`vm/zincvm.c` currently uses Boehm GC (non-moving, conservative). Shen is a
purely functional language: the ONLY mutable heap values are **vectors**
(mutable element array) and the **global table**. Everything else (cons, car,
cdr, closures, symbols, strings, streams) is immutable.

Immutability means an old object can never come to point at a young object
unless something mutates. We exploit that to build a cheap moving generational
collector with a **write barrier that fires at only two sites**.

Confirmed mutation surface (independently traced):
- `vm/zincvm.c:927` — `address->` primitive: `vec.vector.data[i] = val;` (the
  only write to any GC-managed pointer array).
- `vm/zincvm.c:334` — `global_set()`, the single funnel for global-table writes;
  reached at runtime only via the `set` primitive (line 1161). Other `global_set`
  calls are init/startup only.

## Target design

```
nursery (small contiguous semi-space, bump allocation)
   │  scavenge: scan roots + young→young; promote survivors
   ▼
old gen: collected infrequently (no barrier → free to pick)
   └─ mark-sweep (v1), then BiBOP size-class pages + compaction (v2)
roots: precise (stack + globals), no conservative pinning
barrier: ONLY at `address->` vector write + `global_set`
remembered set: dirty-vectors list + dirty-globals bitset
```

Goals, in priority order:
- Fast: bump allocation, tiny frequent nursery scavenges, no barrier on the
  cons/car/cdr hot path.
- Low memory: small nursery; old gen compacting/region-based (~1.4× live)
  rather than the 2× that full semi-space demands.
- Bounded, predictable pauses: frequent tiny nursery flips + rare controlled
  old-gen passes.

## Phased implementation

### Phase 0 — Preparation (no GC change)
Gate: `make test && make test-debug` still pass on Boehm.

- [ ] Add `volatile` to `Value` locals read after `longjmp`: `handler`
      (`trap-error`, ~line 969), `result` (`eval-kl`, ~line 1185),
      `Instr *code` (`run_test_timeout`, ~line 1787).
- [ ] Write `scan_value(Value *v)` — dispatch on `tag`; scan `cons.car/cdr`,
      `lambda.code/env`, `vector.data`. No-op for string/symbol/prim/error/
      stream/number/boolean/nil/mark (they hold non-GC pointers).
- [ ] Add `gc_root_register()`/`gc_root_unregister()` for `global_table`,
      `frame_stack`, `traced_code`, `vm_catch_chain`.
- [ ] Audit + comment the 11 allocate-then-read sites (`GC_VALUE_ARRAY` then
      `memcpy(X.lambda.env, ...)`): lines 983, 998, 1197, 1210, 1223, 1616,
      1714, 2047, 2070, 2284, 2315.

### Phase 1 — Single-space Cheney stop-and-copy
Gate: 34 release + 39 debug + self-hosting + 100K-cons stress +
forwarding-pointer test.

- [ ] Collector core (new `vm/gc.c` or inline): two mmap'd semi-spaces, bump
      alloc, Cheney evacuation, forwarding via header word or address table.
- [ ] Replace all `GC_MALLOC`/`GC_VALUE`/`GC_STR`/`GC_VALUE_ARRAY` with
      `gc_alloc`/`gc_alloc_atomic`; decide scanned-vs-atomic per site
      (`Instr[]` = scanned, `frame_stack` = scanned, string/symbol/prim data =
      atomic).
- [ ] Root scanning: conservative C-stack scan with pinning of ambiguous
      roots + precise scan of the registered fixed roots.
- [ ] Fix nested `val_cons` hazard (`vm/zincvm.c:161-163`): force stack spill
      of the in-progress `Value v` across the second `gc_alloc`, or allocate
      both cells then write.
- [ ] Block SIGALRM during GC (`sigprocmask`); `volatile` after `longjmp`.

### Phase 2 — Generational split: nursery + 2-site write barrier
Gate: generational stress (old-gen vector written with nursery values survives
a scavenge) + global-table stress + full existing suite.

**Design decision (approved):** Keep the existing full-copy `collect()` as the
(rare) old-gen/full-heap collector, and add a **nursery fast lane** in front of
it. Do NOT refactor old gen to mark-sweep for v1 — the existing pinning +
Cheney-queue machinery is already ~90% of a nursery scavenge, and full collect
compacts old gen for free. Mark-sweep needs a new mark bitmap (the header low
bit is already `FORWARDED`) and accepts fragmentation; defer to Phase 3.

**Nursery region:** fixed 2MB region at the start of the heap, pages marked
`space==NURSERY` (3). `grow_heap` only grows old gen. `gc_in_nursery(p)` is a
two-comparison range check on `GCP_to_PAGE(p)`. Large objects (> ~nursery/8)
bypass the nursery via `gc_alloc_oldgen()` — `frame_stack`
(65536 × `sizeof(CallFrame)`) MUST bypass.

**Promotion:** first-survivor. The ACTUAL implementation uses ONLY pin-in-place
for first-survivor promotion: `gc_move` pins nursery pages in place (page
flipped to old-gen) and never copies them. The evacuate-to-old-gen option (copy
via existing `move_internal`) was considered and rejected — see "Step 4
decision" below. All 5 `GC_TYPE_*` tags work because the drain dispatches on
`HEADER_TYPE` identically. No survivor space / aging for v1.

**Step 4 decision — pin-in-place, no copying scavenge:** Keep the no-flip
pin-in-place nursery scavenge; do NOT implement a copying/flip nursery
scavenge. Rationale: the root model is CONSERVATIVE and handle-free — the
mutator holds Values as raw C pointers on the stack and in `extra_roots`, with
no indirection the collector can rewrite. Any nursery object reachable from a
live conservative root CANNOT be moved (the mutator's pointer must stay valid).
This is why even the full `collect()` pins root-reachable pages rather than
copying them. A copying/flip scavenge would need precise roots (handles / typed
root stack) — a Phase 3+ mutator/GC interface change, out of scope for Step 4.

A "pin root-reachable, copy the rest" hybrid is near-dead code: the nursery is
a bump allocator and the just-allocated top page is always pinned by
conservative false positives, so almost every live page ends up pinned;
compaction of old gen is already provided by the full `collect()` semi-space
flip.

Accepted trade-off: pin-in-place can only reclaim nursery pages that are
ENTIRELY dead, so under a retention-heavy workload the nursery can fully promote
and degrade to a one-shot fast lane (small allocs then fall through to old-gen).
This is inherent to conservative + pin-in-place and is ACCEPTED for v1; it is
NOT fixed by the Step 5 write barrier (the barrier only removes the O(heap) full
old-gen scan cost per scavenge, not pinning behavior). Levers if it matters
later: larger nursery, Step 5 barrier, precise roots.

**Barrier site 1 — `address->` vector write (zincvm.c:912): REQUIRED for
correctness.**
```c
vec.vector.data[i] = val;
if (gc_in_oldgen(&vec_heap_obj) && gc_in_nursery(&val)) gc_remember_vector(&vec_heap_obj);
```
Old gen is NOT scanned during a nursery scavenge, so an old-gen vector holding a
nursery pointer must be recorded or the nursery object dangles. `vec` at the
primitive is a by-value pop — the barrier must record the *heap* vector's
address (capture the stack slot before popping). Remembered set `dirty_vectors`:
growable malloc'd `Value**`; the scavenge runs `gc_evacuate(&v->vector.data)` on
each (the drain then scans the array's elements). Cleared at end of each nursery
scavenge and at start of each full collect.

**Barrier site 2 — `global_set` (zincvm.c:324): OPTIONAL / DEFERRED for v1.**
Correctness is preserved because `global_table` is already conservatively pinned
as an extra_root in every collect (including the nursery scavenge), so nursery
pointers in globals survive without a barrier. The only cost is page-granular
over-retention (one nursery cons pins a 512-byte page). Add a dirty-globals
bitset only if profiling shows over-retention is a real problem.

**Ordered steps (each gated by `make test && make test-debug && make run-bundle`):
** 1) [DONE] Add `NURSERY`+region+predicates (no behavior change); 2) [DONE] route
`gc_alloc` to the nursery bump + `gc_alloc_oldgen` for large; 3) [DONE] teach
`collect()` to evacuate nursery pages; 4) [DONE via Step 3] implement
`collect_nursery()` (real scavenge, no barrier) — delivered by Step 3's
pin-in-place approach, so Step 4's remaining content is docs + generational
stress/retention tests (item 6); 5) [PENDING] add the `address->` barrier +
`dirty_vectors`; 6) add generational stress/retention tests; 7) [PENDING]
separate pre-emptive triggers (nursery-full → scavenge; old-gen → full collect);
8) docs update.

**Top risks:** (1) `scavenge_to_space` — handled by the no-flip pin-in-place
design: promoted pages go to the live old-gen `current_space`, never
`next_space` (asserted by the two invariants at the end of `collect_nursery`);
(2) `in_scavenge` re-entry
guard so `allocatepage` doesn't recursively full-collect during a scavenge;
(3) re-audit the 11+ allocate-then-read sites for the nursery-evacuation
(non-pinned) case; (4) persistent pinned bitmap — clear only nursery bits across
scavenges, all bits at full collect.

### Phase 3 — BiBOP old-gen + compaction (optional, v2)
- [ ] Size classes: `Value` (40B), `Value[]`, `Instr[]`, `char[]`.
- [ ] Page-per-size-class, free-list per page, sliding compaction.
- [ ] Full-heap pointer update via `scan_value`.
Defer until Phases 1-2 are stable.

## Key hazards (from validation)

1. **Allocate-then-read-stale-pointer** — 11+ sites (see Phase 0 audit).
2. **Nested `val_cons`** during allocation — compiler keeps intermediate
   Values in registers at `-O2`, invisible to the GC.
3. **`setjmp`/`longjmp` + moving GC** — callee-saved registers restore stale
   pointers after `longjmp`; fix with `volatile` + block signals.
4. **Per-type scanning** — `Value` has non-GC pointers (`sym.name` strdup,
   `prim.name` literal, `stream.file` FILE*) that a uniform word-scan would
   follow into C heap. Must dispatch on `tag`.
5. **Vector backing array is a separate `GC_MALLOC`** (`val_vector`, line 206)
   — must be evacuated with the vector and the `vector.data` pointer updated.
   v1: evacuate both (option b); v2: inline the array (option a).

## Sequencing advice

Do NOT jump straight to Appel + BiBOP. The 2-site barrier is correct and
minimal but it is the EASY part. Prove the moving machinery first with
single-space Cheney (Phase 1), then add generations (Phase 2). BiBOP is
over-engineered for v1 because the old gen (~1200 immortal closures) is
populated at startup and then stable — plain mark-sweep is enough until
fragmentation is actually observed.
