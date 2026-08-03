# Bugs & known issues

## 1. Test 7e / typed-define: `read-from-string` hangs on `{ }` type annotations

**Symptom:** `read-from-string "(define id {A --> A} X -> X)"` hangs >10s. `read-from-string "(+ 1 2)"` works fine. The `read-from-string-unprocessed` path (parse only) is instant.

**Current understanding (2026-08-02):** With the jmp_buf stack fix (da55d9b), the `shen.app` ping-pong is eliminated. The remaining hang is in `process-sexprs`. The typed define triggers a property-vector lookup for the freshly-read symbol, which fails (no arity stored), and the error recovery doesn't terminate. Backtrace after the jmp_buf fix shows `str->bytes` being called repeatedly from `read-from-string` at pc=4.

**Root cause chain:** `find-arities → store-arity → arity(id) → get(property-vector, arity, id) → <-dict → error → get's handler → simple-error → arity's handler returns -1`. After this, `store-arity` should continue and `put` the arity, but something re-enters the chain.

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
