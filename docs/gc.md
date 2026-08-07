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

**Barrier site 1 — `address->` vector write (zincvm.c:912): DONE (Step 5).**
```c
vec.vector.data[i] = val;
if (vec.vector.data &&
    gc_in_oldgen(vec.vector.data) &&
    value_references_nursery(&val)) gc_dirty_vectors_add(vec.vector.data);
```
Old gen is NOT scanned during a nursery scavenge, so an old-gen vector holding a
nursery pointer must be recorded or the nursery object dangles. The barrier
records the **heap element-array base** `vec.vector.data` (a by-value pop, but
`.vector.data` is the real heap array pointer — sufficient, since the scavenge
scans the array's element slots). Remembered set `dirty_vectors`: growable
dedup'd malloc'd `Value**` (`DIRTY_VECTORS_MAX=8192`, with an overflow capacity
valve that falls back to the full old-gen OBJECT-page scan). Cleared at end of
each nursery scavenge and at start of each full collect.

**KEY CORRECTNESS POINT — `gc_in_oldgen` tests the space tag, not the address
range:** under pin-in-place, a nursery page promoted by a scavenge keeps its
nursery-range ADDRESS but its `space` flips to `current_space`. An address-range
test (`page > nursery_last`) silently misses these promoted-in-place arrays —
exactly the case the barrier must cover. `gc_in_oldgen` therefore returns
`space[page] == current_space`. (Diagnosed via gc_nursery_tests Test 6.)

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
stress/retention tests (item 6); 5) [DONE] add the `address->` barrier +
`dirty_vectors`; 6) [DONE] add generational stress/retention tests (Step 4,
incl. write-barrier Test 6 in Step 5); 7) [DONE]
separate pre-emptive triggers (nursery-full → scavenge; old-gen → full collect);
8) [DONE] docs update.

**Step 6 — separate pre-emptive triggers (DONE).** The nursery scavenge and the
old-gen full `collect()` now fire on independent, pre-emptive triggers:

- **Nursery:** `gc_alloc`'s fast path fires `collect_nursery()` when free space
  drops to `NURSERY_SCAVENGE_FREE_LOWATER` (1/8 of the 2MB nursery = 256KB),
  *before* the bump cursor exhausts — not merely when a request can't fit. It is
  gated on `!in_scavenge && first_free_nursery_page() <= nursery_last` so the
  permanently-promoted (one-shot) degraded nursery is skipped, and it sets
  `nursery_tried=1` so the reactive fallback doesn't scavenge twice in one call.
  The reactive not-enough-room path is retained as the safety net.
- **Old-gen:** the three literal `heappages/4` / `heappages/2` triggers are
  unified into `oldgen_collect_threshold()` (pre-emptive, semi-space half-full)
  and `oldgen_collect_lastresort()` (in `allocatepage`, semi-space full), which
  read `heappages` live so `grow_heap` is tracked. No numeric threshold changed.
- **Independence:** the nursery trigger reads only nursery free-space; the
  old-gen trigger reads only `allocatedpages`/`heappages`. A scavenge never
  nests a full collect (`!in_scavenge` at every `collect()` site); the only
  coupling is sequential promotion pressure.
- **Instrumentation** (`gc.h` externs): `gc_preemptive_scavenge_count`,
  `gc_reactive_scavenge_count`, `gc_full_collect_count`. Test 7 in
  `gc_nursery_tests()` (zinctest) burst-allocates 30K dead objects and asserts
  the pre-emptive trigger fires while the reactive path never does. Probe
  location is informational: under `ZINCVM_DEBUG` the conservative stack scan
  pins more nursery pages, so the nursery may legitimately degrade to one-shot
  promotion (probe in old-gen) — accepted, not a failure.

**Top risks:** (1) `scavenge_to_space` — handled by the no-flip pin-in-place
design: promoted pages go to the live old-gen `current_space`, never
`next_space` (asserted by the two invariants at the end of `collect_nursery`);
(2) `in_scavenge` re-entry
guard so `allocatepage` doesn't recursively full-collect during a scavenge;
(3) re-audit the 11+ allocate-then-read sites for the nursery-evacuation
(non-pinned) case; (4) persistent pinned bitmap — clear only nursery bits across
scavenges, all bits at full collect.

### Phase 3/4 — Precise roots → compaction → BiBOP (multi-phase)

**Decision (2026-08-06):** Phase 3's full BiBOP + sliding compaction is NOT
sound under the conservative, handle-free root model. `pin_page` exists because
the mutator holds raw `Value*` C pointers with no indirection the collector can
rewrite — a sliding compactor that moves a page reachable from an ambiguous
stack word would "correct" a random word into garbage. So the correct path is to
**convert the root set from conservative to precise first** (Phase 4a), then
compaction becomes possible (4b), then BiBOP size-class pages pay off (4c).

**Phase 4a — precise roots (DONE, committed).** The whole mutator root surface
is converted to a typed shadow stack, so the conservative C-stack scan can be
deleted without losing any root:

- **Shadow stack** (`gc.c`): process-global growable `malloc`'d array of
  `GcRoot { RootKind kind; void *slot; int *np; }`. `ROOT_PTR` (a single
  pointer slot), `ROOT_VALUE` (a by-value `Value`, interior pointers rewritten),
  `ROOT_VALUE_ARRAY` (N by-value `Value`s). O(1) `gc_root_push_*`/`pop`, plus
  `gc_root_watermark`/`gc_root_pop_to` for longjmp unwind. Lives in C heap, never
  GC-scanned.
- **Rooted call sites**: `vm_exec_env` (5-entry frame block — `acc`,
  `env`, `stack.data`, `cur_code`, `frame_stack` — pushed once at entry, popped
  at `done:`; `argbuf` transient around each apply/appterm); `exec_primitive`
  (`val_cons` car+cdr, `trap-error` body+handler, `eval-kl` 7 intermediates +
  watermark, `error-to-string`, new `val_string_from` helper for tlstr/hdstr/pos);
  load/init (`call_closure1/3`, `--repl`, self-hosting tests).
- **Typed walkers** replace the 2 conservative `extra_roots`: `global_table`
  scans only `.closure` (skips the strdup'd `.name`), `traced_code` scans each
  `Instr*`. Registered via `gc_register_global_table`/`gc_register_traced_code`.
- **`parse_body` rewritten** to eliminate the `gc_realloc` hazard: it now parses
  into a C-heap scratch buffer (VAL_STRING operands `malloc`'d in scratch mode)
  and does ONE final `gc_alloc(GC_TYPE_INSTR_ARRAY)` + bulk copy + re-wrap
  strings to `GC_STR`. `gc_realloc` on a GC-allocated Instr array reads `old` BY
  VALUE after an internal collect — stale interior pointers once objects move.
- **Sanitizer**: ASan cannot link under cosmocc/Cosmopolitan (no `libasan`);
  the sanitizer gate is **UBSan** (`-fsanitize=undefined`), which caught a real
  `env_push` NULL→memcpy bug.
- **Gate** (all pass): 34 release + 39 debug + 34 UBSan + reduced bundle
  (821 closures, nursery 7/7, stress 50k, retention) + full OS bundle
  (1643 closures, debug VM). Conservative scan is STILL the authoritative root
  set during 4a (precise roots are additive-pinning, never evacuate); the flip
  to precise-authoritative is the deferred 4a.6.
- **Instrumentation**: `GC_ROOTS_DIFF` (debug build flag) reports conservative
  pinned pages + precise root count + typed-walker sizes. `verify_heap` no-op.

**Phase 4a.6 — flip (PENDING).** Delete the conservative C-stack scan in
`collect()`/`collect_nursery()`; make `gc_scan_roots` + typed walkers the sole
authoritative root set. Gate: `GC_ROOTS_DIFF` must show `P_cons ⊆ P_prec` (empty
diff) across the whole gate before this lands. This is the highest-risk step.

**Known 4a.6 blockers to fix BEFORE the flip** (from the 2026-08-06 review of
the 4a foundation — all currently masked by the additive-pinning invariant, all
become live use-after-move bugs once the conservative scan is removed):
1. **trap-error handler-throw leak** (zincvm.c:1015-1017): if the handler body
   raises a `simple-error`, `vm_throw` longjmps to `cf.parent` and the
   body/handler `gc_root_pop()`s at 1016-1017 are skipped. Mostly cleaned by the
   enclosing frame's watermark truncation, but fix so pops execute before the
   handler call.
2. **parse_body re-wrap `code` staleness** (zincvm.c:~1397-1415): the re-wrap
   loop calls `GC_STR` (can trigger a collect) while `code` (a C local, not yet
   returned/registered) is unrooted. Push `ROOT_PTR(&code)` around the loop.
3. **val_cons intermediate cells** (zincvm.c:170-178): `car_cell`/`cdr_cell`
   are held only in C locals (incl. the volatile `car_root`) between the two
   `gc_alloc`s; push `ROOT_PTR(&car_root)` before the second alloc.
4. **primitive intermediates** (exec_primitive, e.g. `cn`, `str`, `n->string`,
   `val_string_from` callers): popped Values' interior pointers held only in
   C-stack locals across their `gc_alloc`/`GC_STR`. Systematic audit of every
   `gc_alloc` site in `exec_primitive` required before 4a.6.
5. **volatile cast-away** (zincvm.c:977-979): `gc_root_push_value((Value*)&body)`
   reads a `volatile Value` through a non-volatile alias — technically UB,
   safe in practice; consider a `ROOT_VALUE_VOLATILE` kind.

None of these are live bugs under the additive-pinning 4a invariant (the
conservative scan still pins everything reachable on the C stack), which is why
the foundation is safe to ship/push at the end of 4a.

**Phase 4b — copy instead of pin = sliding compaction (DONE: 4b.1 + 4b.2).** Once
roots are precise, `collect()` evacuates root-reachable objects instead of pinning
them; compaction falls out of `move_internal`'s bump-into-`next_space` for free
(4b.1). `collect_nursery()` is now a COPYING scavenge (4b.2): it evacuates nursery
survivors to old-gen instead of pinning them in place, deleting `pin_nursery_page`,
the `other_space` scan, the pinned-bitmap subsystem, and the one-shot nursery
degradation. During a scavenge `next_space == current_space` (no flip), so
`gc_move`'s to-space short-circuit leaves old-gen objects in place while nursery
objects fall through to `move_internal`. The Cheney drain is fed by an
`allocatepage` gating fix (`current_space != next_space || in_scavenge`) that queues
freshly-copied destination pages. After the drain ALL nursery pages are reset to
`NURSERY` and the bump cursor rewound — the nursery is fully reusable every cycle.
`dirty_vectors` stay correct via the existing clear-on-scavenge-end. Verified by
zinctest Tests 9-13 (fully-reclaimed, copy-not-pin, deep-500-node-graph, no
other_space, cyclic-old-gen-no-infinite-loop).

**Phase 4c — BiBOP size-class pages (PENDING, optional).** Size classes
`Value`=40B, `Instr`=64B, `CallFrame`=48B (build-verified `_Static_assert`s in
`zinctypes.h`); page-per-size-class + free-lists. Only worth it after 4b is
stable and profiling shows old-gen churn (the bundle is ~821 immortal closures
loaded once, so BiBOP's churning-old-gen premise may not apply).

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
