# Bugs & known issues

## 1. Test 7e / typed-define: `read-from-string` hangs on `{ }` type annotations

**Symptom:** `read-from-string "(define id {A --> A} X -> X)"` hangs >10s. `read-from-string "(+ 1 2)"` works fine. The `read-from-string-unprocessed` path (parse only) is instant.

**Root cause (partially fixed):** `overrides.kl` from shen-scheme replaces pure-KLambda functions (`shen.<-dict`, `hash`, `@p`, `vector`, `read-file-as-string`, etc.) with Scheme-specific versions using `scm.*` primitives that don't exist in our C VM. When `arity` → `get` → `shen.<-dict` is called, the broken overrides version runs, `scm.hashtable-ref` fails, and the error propagates incorrectly.

**Fix applied:** Replaced `overrides.kl` with `shen/overrides-pure.kl` — a minimal file containing only the 6 pure-KLambda functions from overrides.kl that (a) don't use `scm.*` primitives and (b) are not defined in other `.kl` files:
- `shen.char-stinput?` → `false` (called by `shen.my-read-byte`)
- `shen.char-stoutput?` → `false` (called by `pr`)
- `shen.push-factorised-branch`, `shen.eval-factorised-branch`
- `shen.show-exceptions?`, `shen.interactive-error?`

All existing tests pass with the fix (38 built-in + 18 self-hosting + GC stress + REPL).

**Remaining hang:** The `read-from-string` typed-define test still hangs. Tracing shows `assoc` enters infinite recursion when searching for a key not in the dict. The `assoc` base case `(= () V3871)` should return true for an empty list, but `assoc` never terminates. This suggests a deeper issue in how `prim =` handles `VAL_NIL` comparison in the auto-push stack model, or how `tl` of `VAL_NIL` behaves in the dict bucket context. Further investigation needed.

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
