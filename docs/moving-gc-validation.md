# Moving Generational GC for shen-meta — Validation & Implementation Plan

## VERDICT: Conditionally sound. The design is correct in its core insight (immutable heap → barrier-free allocation, 2-site write barrier) but has **5 critical correctness hazards** that must be solved before implementation. The Appel-style nursery + BiBOP old-gen choice is sound but arguably over-engineered; a simpler alternative should be considered first.

---

## 1. VALIDATION

### 1.1 "Only vectors mutable" claim — VERIFIED CORRECT (with one nuance)

I traced every write to GC-managed memory in `vm/zincvm.c`:

**Confirmed mutation sites (GC-managed heap):**
1. `address->` primitive, line 927: `vec.vector.data[i] = val;` — the ONLY write to any GC-managed pointer array.
2. `global_set()` at line 334, reached at runtime only via `set` primitive (line 1161). Other `global_set` calls (lines 1843, 1859, 1986) are init/startup only.

**Non-mutation sites that LOOK like mutation but aren't:**
- Line 1002: `handler.lambda.env = henv; handler.lambda.env_len++;` — `handler` is a local `Value` (passed by value to `exec_primitive`). Mutating the local copy's pointer does NOT mutate the heap closure. The original closure in the global table or stack is unchanged. Safe.
- Line 1086: `string_streams[idx].pos++` — mutates a static C array (`string_streams[]`), not GC heap. Irrelevant to write barrier.
- Line 1064-1065: `free(string_streams[idx].data); string_streams[idx].data = NULL;` — frees a `malloc`'d buffer (not GC-managed). Irrelevant.
- `val_cons` (line 163): writes to freshly-allocated `GC_VALUE()` cells. This is initialization, not mutation of existing objects.

**No hidden mutation found in:** cons car/cdr, closure code/env, symbol names, string data, stream objects, env arrays (env arrays are copied, never mutated in place — `env_push` allocates a new array on growth).

**Nuance:** The `env_push` function (line 1472-1480) grows the env array by allocating a new one and copying. The OLD array becomes garbage. This is not mutation of a reachable object — it's replacement. No barrier needed. But it does mean env arrays can be large and short-lived (nursery candidates).

### 1.2 Appel-style nursery + BiBOP old-gen — SOUND, but consider simpler

**Sound for the workload:** Shen is purely functional. Cons cells dominate allocation, are short-lived (nursery), and die young. The ~1200 bundled closures + global table are immortal (old gen). A generational split is justified: the old set is large (~1.4MB of closures) and stable, the nursery churns rapidly.

**BiBOP old-gen is over-engineered for v1.** BiBOP (Big Bag of Pages) size-class pages are excellent for workloads with many size classes and frequent old-gen allocation. Shen's old gen is populated almost entirely at startup (bundle load) and then stable. A simpler mark-sweep with lazy sweeping, or even a stop-the-world copying collector for the whole heap (no generations), would be easier to get correct first.

**Recommendation:** Phase the work. Start with a **single-space stop-and-copy (Cheney) collector** — simplest moving collector, no remembered set, no barrier. Get it correct. Then add the nursery/old-gen split as an optimization. The 2-site write barrier is trivial to add later because the mutation surface is so small.

### 1.3 Correctness pitfalls with a moving collector — 5 CRITICAL hazards

#### Hazard 1: Allocate-then-read-stale-pointer pattern (11+ sites)

The pattern `Value *ne = GC_VALUE_ARRAY(n); memcpy(ne, X.lambda.env, ...)` appears at lines 983, 998, 1197, 1210, 1223, 1616, 1714, 2047, 2070, 2284, 2315. The `GC_VALUE_ARRAY` call can trigger a collection that moves `X.lambda.env`. Then `X.lambda.env` (read from a local `Value` copy) is a stale pointer.

**With Boehm:** Safe (non-moving).
**With moving GC:** The local `Value` (e.g., `acc` in OP_APPLY, `body`/`handler` in trap-error, `extkl`/`klzinc`/`tli` in eval-kl) must be a **precise root** that the GC finds on the C stack and updates in-place. This requires:
- The GC to scan the C stack in word-sized increments.
- For each word that looks like a `Value*` or is inside a `Value` struct, know whether it's a GC pointer and update it.
- Since `Value` is 40 bytes with pointer fields at known offsets, the GC needs a **per-type scan function** (not a uniform pointer scan).

**Fix:** Read `X.lambda.env` into a local `Value *saved_env = X.lambda.env;` BEFORE the allocation, register it as a root, then use `saved_env` after. Or: ensure the GC updates the local `Value` struct in-place on the stack.

#### Hazard 2: Nested val_cons / val_* calls during allocation

`val_cons` (line 161-163) does:
```c
v.cons.car = GC_VALUE(); *v.cons.car = car;
v.cons.cdr = GC_VALUE(); *v.cons.cdr = cdr;
```
The second `GC_VALUE()` can trigger a GC that moves the first allocation. `v.cons.car` (in the local `Value v`) must be updated. If `v` is on the stack and the GC scans it precisely, this works. But with `-O2`, the compiler may keep `v` or `v.cons.car` in registers.

Worse: nested calls like `val_cons(val_symbol("number"), val_cons(v, val_nil()))` (line 446). The inner `val_cons` result is a temporary. If the outer `val_cons`'s `GC_VALUE()` triggers a GC, the inner result's `cons.car`/`cons.cdr` must be updated. If the inner result is in a register (not on the stack), the GC can't find it.

**Fix:** Either (a) use `volatile` locals for intermediate Values, (b) add compiler barriers (`asm volatile("" : : "r"(v) : "memory")`) to force stack spills, or (c) use a GC that supports "register roots" via `getcontext()`/`setjmp` to capture registers. Boehm does (c) automatically. A custom moving collector must do the same.

#### Hazard 3: setjmp/longjmp + moving GC

`trap-error` (line 966-1006) and `eval-kl` (line 1163-1242) use `setjmp`/`longjmp`. After `longjmp`, the handler reads `Value` locals set before `setjmp`:
- `handler` (line 969, read at 997-1003)
- `result` (line 1185, read at 1240)
- `cf.error_val` (set by `vm_throw` at line 385, read at 996)

`setjmp` saves callee-saved registers. `longjmp` restores them. If the compiler kept `handler` in a callee-saved register, `longjmp` restores the pre-`setjmp` value — which has stale GC pointers if the GC moved anything between `setjmp` and `longjmp`.

**Fix:** Mark all `Value` locals read after `longjmp` as `volatile`. This forces them onto the stack (not registers), where the GC can find and update them. Alternatively, re-read from a GC-root-registered location after `longjmp`.

**Note on `cf.error_val`:** `vm_throw` writes `val_error(msg)` which allocates via `GC_MALLOC_ATOMIC` (line 198). The message is non-pointer data. The `error_val` Value itself is on the stack-allocated `CatchFrame`. If the GC scans the catch chain, it finds `cf.error_val` on the stack and updates it. But `cf.error_val.tag` is `VAL_ERROR` and `error.message` is a non-GC pointer — so there's nothing to update. Safe.

#### Hazard 4: Per-type scanning requirement (non-uniform pointer layout)

The `Value` union has different pointer fields depending on `tag`:
- `VAL_CONS`: `cons.car` (GC ptr), `cons.cdr` (GC ptr)
- `VAL_STRING`: `str.data` (GC_ATOMIC, non-pointer, no scan)
- `VAL_SYMBOL`: `sym.name` (strdup, C heap, NOT GC, no scan)
- `VAL_LAMBDA`: `lambda.code` (GC ptr to Instr array), `lambda.env` (GC ptr to Value array)
- `VAL_PRIM`: `prim.name` (string literal, no scan)
- `VAL_ERROR`: `error.message` (GC_ATOMIC, no scan)
- `VAL_VECTOR`: `vector.data` (GC ptr to Value array)
- `VAL_STREAM`: `stream.file` (FILE* or int index, no scan)
- `VAL_NUMBER/BOOLEAN/NIL/MARK`: no pointers

A precise collector MUST dispatch on `tag` to know which fields to scan. A uniform word-scan would either miss pointers (e.g., `lambda.env`) or follow non-GC pointers (e.g., `sym.name`) into C heap, causing corruption.

**Fix:** Write a `scan_value(Value *v)` function that switches on `tag` and scans the appropriate fields. The GC calls this for every root and every reachable object.

#### Hazard 5: Vector backing array is a separate allocation

`val_vector` (line 203-208) allocates `vector.data` as a separate `GC_MALLOC`. The `Value` struct holds a pointer to it. For compaction:
- When evacuating a vector, you must also evacuate the data array and update `vector.data`.
- The data array contains `Value` elements (GC pointers), so it must be scanned.
- The write barrier at `address->` (line 927) must record the vector in the remembered set when: (a) the vector is in old gen, and (b) the written value points to a nursery object.

**Fix options:**
- **(a) Inline the data array** into the vector object: allocate `sizeof(Value) + size * sizeof(Value)` as one object. `vector.data` points to the inline area. Pro: single evacuation, no separate scan. Con: changes `val_vector` and all access patterns; variable-size allocation in the nursery.
- **(b) Keep separate, evacuate both**: the vector's `scan_value` function evacuates `vector.data` and updates the pointer. Pro: minimal code change. Con: two allocations per vector, fragmentation.

**Recommendation:** Option (b) for v1 (minimal change), option (a) as optimization later.

### 1.4 Interaction: setjmp/longjmp + moving GC — CRITICAL

See Hazard 3 above. The key risk is `Value` locals in callee-saved registers being restored to stale values by `longjmp`. The fix is `volatile` qualifiers on all `Value` locals read after `longjmp` in:
- `trap-error` handler path (line 992-1005): `handler` must be `volatile`.
- `eval-kl` error path (line 1238-1241): `result` must be `volatile`.
- `run_test_timeout` (line 1799-1818): `code`, `len` (not Values, but `Instr*` is a GC pointer — must be `volatile`).
- `alarm_handler` longjmp (line 1779): jumps out of vm_exec recursion. All `Value` locals in the vm_exec_env frame are abandoned — the GC must not try to scan them (they're dead). This is fine as long as the GC runs BEFORE the longjmp (it won't — `alarm_handler` is a signal handler that longjmps directly).

**Signal handler + GC:** `alarm_handler` (line 1773-1780) does `longjmp` from a signal handler. If a GC is in progress when SIGALRM fires, the longjmp corrupts the GC state. **Mitigation:** block SIGALRM during GC, or use `sigprocmask` to defer the signal. This is a new concern that Boehm doesn't have (Boehm blocks signals internally).

---

## 2. IMPLEMENTATION PLAN

### Phase 0: Preparation (no GC change)

**Goal:** Make the codebase moving-GC-ready without changing the collector.

**Step 0.1: Add `volatile` to all `Value` locals read after `longjmp`.**
- File: `vm/zincvm.c`
- `trap-error` handler (line 997): `volatile Value handler` — but `handler` is set at line 969 before `setjmp`, read at 997-1003 after `longjmp`. Make it `volatile`.
- `eval-kl` (line 1185): `volatile Value result` — set before `setjmp`, read after `longjmp`.
- `run_test_timeout` (line 1787-1788): `Instr *code` is GC-managed. Make `volatile Instr *code`.
- Test: `make test && make test-debug` — should pass (volatile doesn't change behavior with Boehm).

**Step 0.2: Add a `scan_value(Value *v)` function.**
- File: `vm/zincvm.c`, new function before `vm_exec_env`.
- Switches on `v->tag`, scans the appropriate pointer fields.
- For `VAL_CONS`: scan `*v.cons.car` and `*v.cons.cdr` (recursively, or enqueue for the scavenger).
- For `VAL_LAMBDA`: scan `v.lambda.code` (Instr array — scan `operand` and `closure_code` fields) and `v.lambda.env` (Value array).
- For `VAL_VECTOR`: scan `v.vector.data` (Value array of `v.vector.len` elements).
- For `VAL_STRING/VAL_SYMBOL/VAL_PRIM/VAL_ERROR/VAL_STREAM/VAL_NUMBER/VAL_BOOLEAN/VAL_NIL/VAL_MARK`: no pointer scanning.
- This function is the precise root scanner. It will be called by the GC for every root and every evacuated object.

**Step 0.3: Add a `gc_root_register()` / `gc_root_unregister()` API.**
- For `global_table`, `frame_stack`, `traced_code`, `vm_catch_chain`.
- These are the fixed roots. The C stack is scanned separately.

**Step 0.4: Audit all `GC_VALUE_ARRAY` → `memcpy(X.lambda.env)` patterns.**
- Add a comment at each site: "MOVING GC HAZARD: allocation may move X.lambda.env. Ensure X is a precise root."
- These will be fixed in Phase 2 when the moving collector is introduced.

### Phase 1: Single-space stop-and-copy (Cheney) collector

**Goal:** Replace Boehm with the simplest possible moving collector. No generations, no barrier.

**Step 1.1: Implement the collector core.**
- New file: `vm/gc.c` (or inline in `zincvm.c` for now).
- Two semi-spaces: `from_space` and `to_space`, each a contiguous mmap'd region.
- Bump-pointer allocation in `from_space`.
- `gc_collect()`: Cheney-style copy from `from_space` to `to_space`, then swap.
- Forwarding pointer: use a header word before each object (or steal the `tag` field — but `tag` is only 4 bytes of a 40-byte struct, so use a separate forwarding table: hash map from old address to new address).

**Step 1.2: Replace all `GC_MALLOC` / `GC_VALUE` / `GC_STR` / `GC_VALUE_ARRAY` calls.**
- `GC_VALUE()` → `gc_alloc(sizeof(Value))` — bump-pointer alloc in nursery.
- `GC_STR(len)` → `gc_alloc_atomic(len + 1)` — non-scanned allocation.
- `GC_VALUE_ARRAY(n)` → `gc_alloc(n * sizeof(Value))` — scanned allocation.
- `GC_MALLOC(size)` (for Instr arrays, frame_stack) → `gc_alloc(size)` or `gc_alloc_atomic(size)` depending on whether the content has GC pointers.
  - `Instr` array (line 1304): contains `Value operand` (GC pointers) and `Instr *closure_code` (GC pointer) → `gc_alloc`.
  - `frame_stack` (line 1503): `CallFrame` contains `Value *env` and `ValueArray stack` (GC pointers) → `gc_alloc`.
  - `GlobalEntry.name` is `strdup` (C heap) — but `GlobalEntry.closure` is a `Value` with GC pointers → `gc_alloc` for the table, but the table is static (not GC-managed). Register as root instead.

**Step 1.3: Implement root scanning.**
- C stack: scan from `__builtin_frame_address(0)` to the stack base. For each word, check if it's a `Value*` pointing into `from_space`. If so, evacuate it and update the pointer.
  - **Problem:** This is conservative, not precise. For a precise collector, we need to know which stack words are `Value` structs. This requires either (a) compiler support (DWARF, frame layout), or (b) explicit root registration.
  - **Pragmatic approach:** Use `__builtin_setjmp` / `__builtin_unwind_init()` to capture registers, then scan the register save area + stack conservatively. This is what Boehm does. For a MOVING collector, conservative scanning is dangerous (a non-pointer that looks like a GC pointer gets "evacuated" to a bogus location, corrupting the non-pointer).
  - **Better approach:** Register all `Value` locals explicitly as roots. This is invasive but correct. Use a `GC_ROOT(value)` macro that pushes the address of a `Value` local onto a thread-local root stack. The GC scans the root stack precisely (knows each entry is a `Value*`).
  - **Even better:** Use `setjmp` at the top of `vm_exec_env` to capture registers into a known buffer, then scan the buffer + stack conservatively but ONLY evacuate objects that are definitely GC-managed (address in `from_space` range). Non-pointers that happen to look like GC pointers will be "evacuated" but the original will be left in place — this is the "ambiguous root" problem. For a copying collector, ambiguous roots must PIN the object (not evacuate). So: conservative scan → pin found objects → precise scan of known roots → evacuate. This is the Bartlett "mostly copying" approach.
  - **Recommendation:** Use the Bartlett approach (conservative stack scan with pinning) for v1. The existing Bartlett GC at `vendor/bartlett-gc` already does this. Re-introduce it as the base, then add precise scanning for `global_table`, `frame_stack`, and `vm_catch_chain` (which have known layouts).

**Step 1.4: Handle the `val_cons` nested allocation hazard.**
- After `v.cons.car = gc_alloc(...); *v.cons.car = car;`, the next `gc_alloc()` for `v.cons.cdr` may trigger a collection that moves `v.cons.car`.
- If `v` is on the stack and the GC scans the stack (conservatively or precisely), `v.cons.car` will be updated. But `v` might be in a register.
- **Fix:** Add `asm volatile("" : : "r"(&v) : "memory");` after the first allocation to force `v` onto the stack. Or: allocate both cells first, then write:
  ```c
  Value *car_cell = gc_alloc(sizeof(Value));
  Value *cdr_cell = gc_alloc(sizeof(Value));  // may trigger GC, but car_cell is in a local var
  // Re-read car_cell if GC moved it (if car_cell is a precise root)
  *car_cell = car; *cdr_cell = cdr;
  v.cons.car = car_cell; v.cons.cdr = cdr_cell;
  ```
  This requires `car_cell` to be a precise root across the second allocation.

**Step 1.5: Handle `setjmp`/`longjmp`.**
- Block signals during GC (`sigprocmask`).
- `volatile` on all `Value` locals read after `longjmp` (done in Phase 0).
- After `longjmp`, re-validate any GC pointers by checking they're in the current semi-space (post-swap). If not, they're stale and must be re-read from a root.

**Step 1.6: Test.**
- `make test` (34 release tests) — all must pass.
- `make test-debug` (39 debug tests) — all must pass.
- `make run-bundle` (self-hosting tests) — all must pass.
- Add a GC stress test: allocate 100K cons cells in a tight loop, verify no corruption.
- Add a forwarding-pointer test: allocate, trigger GC, verify moved objects are accessible via old pointers (forwarding).

### Phase 2: Generational split (nursery + old gen)

**Goal:** Add a nursery semi-space and an old generation. Only collect the nursery on minor GCs.

**Step 2.1: Nursery.**
- Small contiguous semi-space (e.g., 2MB). Bump allocation.
- When full, scavenge: copy live nursery objects to the old gen (or to the survivor space, then promote to old gen).
- Roots for nursery scavenge: C stack, `global_table`, `frame_stack`, `vm_catch_chain`, `traced_code`, AND the remembered set (old→young pointers).

**Step 2.2: Old generation.**
- For v1: mark-sweep with lazy sweeping (no compaction). Simpler than BiBOP.
- For v2: BiBOP size-class pages with compaction (the proposed design).
- Old-gen collection triggered when old gen is full or after N minor GCs.

**Step 2.3: Write barrier — 2 sites only.**

**Site 1: `address->` primitive (line 927).**
```c
vec.vector.data[i] = val;
// WRITE BARRIER:
if (gc_in_old_gen(&vec) && gc_in_nursery(&val)) {
    gc_remember_vector(&vec);  // add vec to dirty-vector remembered set
}
```
- Remembered set: a set of vector objects that have been dirtied (old→young pointer written).
- During nursery scavenge, scan the dirty vectors' data arrays for nursery pointers.
- Clear the dirty set after each nursery scavenge.

**Site 2: `set` primitive → `global_set` (line 334).**
```c
global_table[i].closure = v;  // or new entry
// WRITE BARRIER:
if (gc_in_nursery(&v)) {
    gc_remember_global(i);  // add global index to dirty-global remembered set
}
```
- Remembered set: a set of global table indices that have been dirtied with nursery pointers.
- During nursery scavenge, scan the dirty globals' closure fields for nursery pointers.
- Clear the dirty set after each nursery scavenge.
- **Note:** Most `global_set` calls are at startup (before any nursery objects exist). The barrier is a no-op then. Only runtime `set` calls (line 1161) need the barrier.

**Step 2.4: Remembered set data structures.**
- `dirty_vectors`: a growable array of `Value*` (pointers to vector objects in old gen). Deduplicated (a vector dirtied twice is scanned once).
- `dirty_globals`: a bitset or growable array of `int` (global table indices). Deduplicated.
- Both live in C heap (not GC-managed). Cleared after each nursery scavenge.

**Step 2.5: Test.**
- Same test suite as Phase 1.
- Add a generational stress test: allocate a vector in old gen (force promotion), write nursery values into it via `address->`, trigger nursery GC, verify the values survive.
- Add a global-table stress test: `set` a global to a nursery cons cell, trigger nursery GC, verify the cons survives.

### Phase 3: BiBOP old-gen with compaction (optional, v2)

**Goal:** Replace mark-sweep old gen with BiBOP size-class pages + compaction.

**Step 3.1:** Size classes for `Value` (40 bytes), `Value[]` arrays (variable), `Instr[]` arrays (variable), `char[]` strings (variable).
**Step 3.2:** Page-per-size-class, free-list per page.
**Step 3.3:** Compaction: slide objects within pages to remove fragmentation.
**Step 3.4:** Update all pointers (requires a full-heap scan with `scan_value`).

**This is the most complex phase. Defer until Phases 1-2 are stable.**

---

## 3. RISK SUMMARY

| Risk | Severity | Mitigation |
|---|---|---|
| Allocate-then-read-stale-pointer (11+ sites) | CRITICAL | Precise roots or re-read after alloc |
| Nested val_cons during GC | CRITICAL | Force stack spills or explicit root registration |
| setjmp/longjmp + moving GC | CRITICAL | `volatile` qualifiers, block signals during GC |
| Conservative stack scan with moving collector | HIGH | Use Bartlett mostly-copying (pin ambiguous roots) |
| Per-type scanning requirement | HIGH | `scan_value()` dispatch on tag |
| Vector backing array compaction | MEDIUM | Evacuate data array separately (option b) |
| Signal handler + GC interaction | MEDIUM | Block SIGALRM during GC |
| BiBOP complexity | LOW (defer) | Phase 3, optional |

## 4. RECOMMENDATION

**Do NOT jump straight to Appel-style generational + BiBOP.** The 5 critical hazards above make a moving collector significantly harder than the current Boehm setup. The right path is:

1. **Phase 0:** Make the code moving-GC-ready (volatile, scan_value, root registration). No collector change. All tests pass.
2. **Phase 1:** Single-space Cheney stop-and-copy with Bartlett-style conservative stack scan (pin ambiguous roots). All tests pass.
3. **Phase 2:** Add nursery/old-gen split with the 2-site write barrier. All tests pass.
4. **Phase 3:** BiBOP old-gen compaction (optional, only if old-gen fragmentation is a real problem).

The 2-site write barrier is the design's strongest insight — it's correct and minimal. But it's the EASY part. The hard part is making a moving collector safe with the existing `setjmp`/`longjmp` error handling, nested allocations, and conservative stack scanning.
