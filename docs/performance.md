# Performance & Refactor Analysis — Shen ZINC VM

Status: advisor review (GLM-5.2), 2026-08-06. Grounded in `vm/zincvm.c`, `vm/gc.c`,
`vm/zincvm.h`, `vm/zinctypes.h`, `shen/zinc.shen`. No code changed by this document.

## Summary

The hottest cost centers, in roughly decreasing impact:

1. **`global_get` is a linear `strcmp` scan over ~800–1200 entries**
   (`vm/zincvm.c:375`). Every `OP_GLOBAL` and most `OP_PRIM` lookups (via the
   `exec_primitive_valid` fallback) hit this. Almost certainly the single dominant
   per-instruction cost for the metacircular interpreter.
2. **`vm_exec_env` allocates a 3 MB `frame_stack` per recursive call**
   (`vm/zincvm.c:1663`). Each `eval-kl`, `trap-error`, `call_closure1/3` recursion
   grabs 65 536 × 48 B of old-gen, never freed, scanned on every full collect.
3. **`exec_primitive` is a 50-arm `if (strcmp(...))` chain**
   (`vm/zincvm.c:668-1342`). Hit on every `OP_PRIM` and every
   `OP_APPLY`/`OP_APPTERM` with a `VAL_PRIM`.
4. **No symbol interning** — `val_symbol` `strdup`s every time (`vm/zincvm.c:166`),
   forcing `strcmp` everywhere and leaking `sym.name` on GC.
5. **`OP_APPTERM` (tail call) allocates a fresh env array every iteration**
   (`vm/zincvm.c:1877`). Tail-recursive loops — exactly what the metacircular
   interp does — pump the nursery.
6. **Switch dispatch** in the hot loop (`vm/zincvm.c:1711`) with per-instruction
   `instr_count++` and `pc`-bounds checks.

---

## Category A — cheap, high-impact, localized

### A1. Hash the global table (biggest single win)
- **Problem.** `global_get` (`vm/zincvm.c:375-387`) does a linear `strcmp` scan
  over `global_table_len` (~800 reduced / ~1216 full). `OP_GLOBAL`
  (`vm/zincvm.c:1829-1834`) calls it on every non-local reference. `OP_PRIM`'s
  miss path falls back to `exec_primitive_valid` (another ~50-entry linear
  `strcmp` chain at `vm/zincvm.c:636-656`). `global_set` is also linear
  (`vm/zincvm.c:358-373`), hit by every `set` primitive during `shen.initialise`.
- **Fix.** Open-addressing hash table keyed by string hash. Keep the linear array
  for `gc_scan_roots` (which already scans every slot, `gc.c:442-456`) — the hash
  is a pure lookup accelerator layered on top. Build after `vm_load_bundle`;
  maintain incrementally on `global_set`. Interned names (A2) collapse the key to
  a pointer.
- **Effort.** ~150 LOC, 1–2 days. **Risk.** Low. **Benefit.** O(1) vs O(n);
  expect 2–5× on interp-heavy runs.

### A2. Intern symbols
- **Problem.** `val_symbol` (`vm/zincvm.c:166-169`) `strdup`s every call. Same
  string at different sites gets different `sym.name` pointers, forcing `strcmp`
  in `=`, `global_get`, `exec_primitive`, trap-error matching, primitive dispatch.
  Additionally `sym.name` is C-heap (the `gc_scan_value` default branch at
  `vm/zincvm.c:91-96` skips it), so when a symbol Value is collected the name
  **leaks** — relevant for `gensym`/`intern` in long REPL sessions.
- **Fix.** String table at parse time: linear-probing hash on bytes → canonical
  `const char *`. `val_symbol(buf)` returns the canonical pointer. Enables
  pointer-equality fast path and unlocks A1/A3/A5.
- **Effort.** ~80 LOC, half a day. **Risk.** Low — interning is name-equality
  preserving; the "never make `=` shen.-prefix-aware" pipeline invariant is
  unaffected. **Benefit.** Faster compares, faster hashing for A1, fixes a real
  leak, unlocks A5.

### A3. `exec_primitive` dispatch
- **Problem.** 50+ `if (strcmp(name, "..."))` arms (`vm/zincvm.c:668-1342`),
  hit by every `OP_PRIM` and every `VAL_PRIM` apply/appterm.
- **Fix options, increasing payoff:** (1) switch on `name[0]` first (~30 LOC);
  (2) with A2, pointer-indexed table / perfect-hash (gperf) (~60 LOC);
  (3) compile-time primitive ID in the bundle (`OP_PRIM` operand carries a small
  int) — best long-term but needs `shen/zinc.shen` + `compile.shen` + bundle
  format bump.
- **Effort.** 30–100 LOC. **Risk.** Low. **Benefit.** Large for prim-heavy code.

### A4. Computed-goto dispatch (direct threading)
- **Problem.** `switch (in->op)` (`vm/zincvm.c:1711`) with ~17 cases; one shared
  branch-prediction slot across opcodes. Direct threading gives each handler its
  own prediction site — typically 10–25% on hot interpreter loops (Ertl–Gregg).
- **Fix.** `static const void *const dispatch[256]` indexed by opcode; replace
  the switch with `goto *dispatch[in->op]` at the loop head and
  `goto *dispatch[next_op]` at the end of each handler.
- **Effort.** ~150 LOC mechanical, half a day. **Risk.** Requires labels-as-values
  (cosmocc supports it); keyed to the existing `Opcode` enum
  (`vm/zinctypes.h:65-72`). **Benefit.** 10–25%.

### A5. Amortize `instr_count`/bounds checks
- **Problem.** Loop head (`vm/zincvm.c:1676-1699`) does `++instr_count; if (...>= INSTR_HARD_LIMIT)` and `if (pc<0 || pc>=cur_len)` every instruction.
- **Fix.** (a) decrement counter, refresh every 4096 iterations; (b) eliminate
  `pc` bounds check if every code array ends in `OP_RETURN` (compiler guarantee).
- **Effort.** 10 LOC, an hour. **Risk.** Very low. **Benefit.** ~5%.

### A6. Larger initial value stack
- **Problem.** `STACK_INIT_CAP 64` (`vm/zincvm.c:304`) — early doublings each
  trigger `GC_VALUE_ARRAY` + `memcpy` (`va_push` at `zincvm.c:310-318`).
- **Fix.** `STACK_INIT_CAP 1024`. **Effort.** 1 line. **Risk.** None.

### A7. `OP_APPLY`/`OP_APPTERM` arg-buf indirection
- **Problem.** Both opcodes pop args into a local `Value argbuf[64]`
  (`zincvm.c:1756`, `1862`), root-pin, then copy into the new env
  (`zincvm.c:1786-1787`, `1883-1884`) — two copies per call.
- **Fix.** Write directly from the stack into the new env in reverse index order
  (args already on stack RTL: top = arg1). Keep the >64-args error path and
  rooting semantics.
- **Effort.** ~30 LOC. **Risk.** Low. **Benefit.** Removes one memcpy per call.

---

## Category B — meaningful, more involved

### B1. Per-`vm_exec_env` 3 MB `frame_stack` allocation
- **Problem.** `vm_exec_env` does
  `gc_alloc_oldgen(CALL_STACK_DEPTH * sizeof(CallFrame), ...)`
  (`vm/zincvm.c:1663`) — 65 536 × 48 B = 3 145 728 B **per recursive call**.
  `eval-kl` recurses 3× per invocation, `trap-error` twice,
  `call_closure1/3` once each. Nested chains stack up several 3 MB old-gen blocks,
  scanned page-by-page on every full `collect()` (`gc.c:349-358`). The 65 536
  depth is only needed for `shen.initialise`'s top-level call chain, which runs in
  a **single** `vm_exec_env` invocation.
- **Fix.** Allocate one global `frame_stack` at startup; each `vm_exec_env` call
  threads `frames_sp_base` (caller's `frames_sp`) and uses indices above it.
- **Effort.** ~80 LOC (signature change + callers). **Risk.** Medium — every
  `goto done`/`longjmp` path must restore `frames_sp` (the `done:` block at
  `zincvm.c:1904-1911`). **Benefit.** Large for memory; closes a latent OOM cliff.
- **Alternative.** Drop `CALL_STACK_DEPTH` to 8192 — saves 256 KB per call, 12×
  reduction, less invasive.

### B2. `OP_APPTERM` env reuse for tail calls
- **Problem.** `OP_APPTERM` (`vm/zincvm.c:1854-1900`) reuses the `CallFrame` but
  always allocates a fresh env (`GC_VALUE_ARRAY(new_env_len)`, `zincvm.c:1877`).
  Tail-recursive interp loops pump the nursery every iteration.
- **Fix.** If `env_cap >= new_env_len` and no aliasing with the closure's captured
  env, reuse current `env` in place. The current env array is uniquely owned by
  this frame — `OP_CUR` copies it via `val_lambda` (`zincvm.c:208-209`), and
  `OP_LET`'s `env_push` realloc replaces `env`.
- **Effort.** ~30 LOC. **Risk.** Medium — must audit the unique-ownership claim.
  **Benefit.** Cuts nursery allocation substantially for tail-recursive workloads.

### B3. `val_cons` root-pin dance
- **Problem.** Every `cons` (`vm/zincvm.c:174-194`) does 2 root-pushes + 2
  `gc_alloc` (a real `noinline` call) + car-root pin + 3 pops. `cons` is one of
  the most-called primitives.
- **Fix.** Skip the root-pin dance when both car/cdr are immediate types (number /
  boolean / nil / symbol with no nursery pointer). The `value_references_nursery`
  check already exists (`vm/zincvm.c:104`).
- **Effort.** ~50 LOC. **Risk.** Medium. **Benefit.** 20–40% cons-overhead
  reduction plausible.

### B4. `OP_CUR` env-copy overhead
- **Problem.** `val_lambda` (`vm/zincvm.c:199-215`) always `memcpy`s env into a
  fresh `GC_VALUE_ARRAY`. Options: cur+apply fusion in the compiler (needs
  `shen/zinc.shen`), or persistent refcounted env arrays (larger refactor).
- **Effort.** High. **Risk.** Medium-high. **Recommendation.** Defer until A1/A2/A3
  land and profiling shows `OP_CUR` is hot.

### B5. Inline cache for `OP_GLOBAL`
- **Problem.** Even with A1's hash, every `OP_GLOBAL` does a hash lookup.
- **Fix.** Repurpose the unused `jmp_target` field of `Instr` as a cache slot;
  invalidate on `global_set`.
- **Effort.** ~50 LOC. **Risk.** Low for monomorphic sites. **Recommendation.**
  Only after A1.

---

## Category C — architectural

- **C1. Direct-threaded dispatch** — see A4 (promoted to Category A).
- **C2. QBE AOT lowering** — documented in `docs/qbe-lowering.md` (design only).
  Hot closures gain 10–100×, but pursue for the soundness/type-preservation story
  (its actual purpose), not as a perf hack; the C VM is a stand-in for a generated
  interpreter. Weeks, high risk.
- **C3. Specialized/fused opcodes** — profile-driven (`access2`, `cons-access`,
  etc.). Defer until a benchmark harness exists.
- **C4. NaN-boxing** — rejected: changes the entire Value model and breaks the
  documented 40 B BiBOP size-class static asserts (`vm/zinctypes.h:99-106`).

---

## Correctness risks that are also perf issues

- **CR1.** Per-`vm_exec_env` 3 MB allocation can OOM (`B1`). Deep `eval-kl` chains
  during `shen.initialise` are the trigger. Both a correctness cliff and a perf
  issue (frequent old-gen collects scanning 3 MB × N pages).
- **CR2.** `deep_equal` uses a `static int depth` counter (`vm/zincvm.c:544`) —
  non-reentrant; the 1000 limit may be too low for compiler-generated deep
  structures; a longjmp out of a future extension would leak depth. Pass depth as
  a parameter.
- **CR3.** `MAX_STRING_STREAMS = 8` fixed cap (`vm/zincvm.c:254`); the `open`
  ENOENT fallback creates a string stream (`vm/zincvm.c:1107-1109`) and workloads
  opening many nonexistent paths exhaust the pool. Latent correctness cliff.
- **CR4.** `VAL_SYMBOL` `sym.name` leak (`sym.name` is `strdup`'d C-heap; the
  `gc_scan_value` default branch at `vm/zincvm.c:91-96` skips it). Real for
  `gensym`/`intern` in long REPL sessions. A2 fixes it.
- **CR5.** `instr_count` resets per `vm_exec_env` (`vm/zincvm.c:1671`) — the
  500 M hard limit is per-invocation, not global; deep `eval-kl` chains can run
  far more total instructions before tripping. Weaker safety net than it appears.

---

## Recommended execution order

1. **F1 — add a benchmark harness first.** There is **no timing instrumentation**
   in `vm/` (no `gettimeofday`/`clock_gettime`/`benchmark`). Run
   `./zinctest globals.csexp` plus a fixed self-hosting workload under
   `clock_gettime(CLOCK_MONOTONIC)` and report wall time + GC counters
   (`gc_nursery_scavenge_count`, `gc_full_collect_count`, `gc_alloc_class_count[]`
   are all exposed in `gc.h`). ~1 hour. Without this, nothing can be measured.
2. **A1 + A2 together** (synergistic, biggest win) — 2–3 days.
3. **A3** (after A2) — half a day.
4. **B1** (fixes OOM cliff) — 1–2 days.
5. **A4** (computed goto) — half a day.
6. **B2** (appterm env reuse) — half a day.
7. **A5, A6, A7** — cheap polish.
8. Profile, then consider B3/B4/B5/C3.
