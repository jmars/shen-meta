# Bugs & known issues

## 1. Test 7e / typed-define: `read-from-string` hangs on `{ }` type annotations

**Symptom:** `read-from-string "(define id {A --> A} X -> X)"` hangs >10s. `read-from-string "(+ 1 2)"` works fine. The `read-from-string-unprocessed` path (parse only) is instant.

**Debugging results (2026-08-03):**
- `*property-vector*` is **correctly initialized** as a Shen dict (VAL_VECTOR, tag=10 in this codebase's enum) after `shen.initialise`. The dict is functional — `dict.kl` is loaded and `shen.dict` is in the bundle.
- The jmp_buf stack (`error_jmp_stack`) is **balanced** — no corruption. Every `te_push` is matched by `te_pop` via either the body or handler path.
- The hang manifests as: `simple-error("id has no attributes: arity")` fires at `error_jmp_sp=1` (the eval-kl level), the longjmp is caught, and the whole computation re-enters in an infinite loop. Trap-error bodies with `env_len=1` complete via BODY-DONE (no handler invoked) thousands of times, then the simple-error fires and the cycle repeats.
- This is consistent with the **"arity loop in kl->zinc"** root cause from commit `825ff9b` — the problem is in the Shen-level arity lookup/error-recovery logic, not in the C VM's trap-error machinery.

**Root cause chain:** `find-arities → store-arity → arity(id) → get(property-vector, arity, id) → <-dict → "value id not found in dict" → get's handler → simple-error("id has no attributes: arity") → arity's handler returns -1 → store-arity calls execute-store-arity → put → ... → loop re-enters.

**Contributing factors fixed:**
1. Stale `push` instructions in bundle — fixed by `make bundle`
2. `eval-kl` recursion guard — removed in `14b8d2d`
3. Trap-error handler ping-pong — fixed in `da55d9b` (jmp_buf stack)

**Older trace evidence:**
- `shen.store-arity`/`shen.sysfunc?`/`element?`/`get`: 24,295 calls
- `shen.app`: 63,586 calls

## 2. `=` cons-vs-symbol HACK — REMOVED

**Status:** Fixed.  
**Was:** `zinc-c` generated flat `(= [number 42] "number")` instead of `(= hd(hd(Code)) "number")`.  
**Resolution:** The zinc-c compiler now generates correct `hd`-wrapped comparisons.

## 3. `eval_kl` error swallowing

**File:** `vm/zincvm.c`  
**Symptom:** On error, `eval_kl` returns identity instead of re-raising.  
**Reason:** Shen's `load` doesn't wrap forms in `trap-error`.  
**Status:** Intentional but fragile. Now uses `te_push/te_pop` (da55d9b).

## 4. `str` primitive — FIXED

**Status:** Fixed.  
**Fix:** `str_value()` handles all types with full `put-datum` representation.

## 5. Trap-error handler ping-pong — FIXED (da55d9b)

**Symptom:** `get`'s handler's `simple-error` longjmp'd back to itself because `vm_error_jmp` wasn't restored.

**Root cause:** Single `vm_error_jmp` + manual save/restore couldn't handle nested trap-errors where the inner handler calls `simple-error`.

**Fix:** `error_jmp_stack[64]` + `te_push()`/`te_pop()`. `trap-error` pushes before `setjmp`. Handler pops FIRST so `simple-error` propagates outward. Also updated `eval-kl` and REPL code.
