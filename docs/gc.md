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
- [ ] Root scanning: re-introduce Bartlett mostly-copying (conservative C-stack
      scan with pinning of ambiguous roots) + precise scan of the registered
      fixed roots. The archived `vendor/bartlett-gc` is the base.
- [ ] Fix nested `val_cons` hazard (`vm/zincvm.c:161-163`): force stack spill
      of the in-progress `Value v` across the second `gc_alloc`, or allocate
      both cells then write.
- [ ] Block SIGALRM during GC (`sigprocmask`); `volatile` after `longjmp`.

### Phase 2 — Generational split + 2-site write barrier
Gate: generational stress (old-gen vector written with nursery values survives
a scavenge) + global-table stress.

- [ ] Nursery: ~2MB contiguous semi-space; scavenge on full.
- [ ] Old gen: mark-sweep with lazy sweeping (no compaction yet).
- [ ] Barrier site 1 — `address->` (line 927):
      `if (gc_in_old_gen(&vec) && gc_in_nursery(&val)) gc_remember_vector(&vec);`
      Remembered set: growable dedup'd `Value*` array of dirtied vectors.
- [ ] Barrier site 2 — `global_set` (line 334):
      `if (gc_in_nursery(&v)) gc_remember_global(i);`
      Remembered set: dedup'd bitset/array of dirtied global indices.
- [ ] Remembered sets live in C heap (not GC-managed); cleared after each
      nursery scavenge. Scavenge scans them for nursery pointers.
- [ ] Startup `global_set` calls are barrier no-ops (no nursery objects exist
      yet).

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
