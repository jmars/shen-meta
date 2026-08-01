# Bugs & known issues

## 1. Test 7e: runtime `.shen` load hangs in `shen.eval-and-print`

**File:** `vm/zincvm.c:2294-2303` (commented out)  
**Symptom:** Loading a `.shen` file containing `(defun ...)` at runtime via bundled `load` hangs. `(+ 1 2)` loads fine.

**Root cause found:** The `shen.eval-and-print` closure has nested `pushmark` instructions that leave stray marks on the stack after the inline `prim eval-kl` (P opcode) pops its argument. When `shen.app` is applied next, it receives wrong arguments. For `defun` forms, this triggers type-checker/error-reporter code that calls `shen.app` in a tight loop (~62K calls before timeout):
```
shen.app → shen.arg->str → shen.atom->str → returns → shen.app again
```

For `(+ 1 2)`, `eval-kl` completes without type checking, so the buggy stack layout isn't triggered.

**Debug evidence:** `--trace` shows zero activity from `shen.shen->kl-h`, `shen.record-and-evaluate`, `interp`, `toplevel-interp`, `kl->zinc`, `extract-kl` — the hang is in the `eval-kl` C primitive's interaction with the `shen.eval-and-print` closure's stack layout.

**Fix needed:** The KLambda source in `load.kl:9` is:
```
(pr (shen.app (eval-kl (shen.shen->kl Z1253)) "\n" (shen.s (shen.shen->kl Z1253)) ""))
```
`shen.s` is a function call, but `zinc-c` emits `[symbol shen.s]` (literal) instead of `[global shen.s]` (function lookup), leaving the `apply` for `shen.s` missing from the bytecode. The orphaned args (`shen.s`, `"\n"`) corrupt `shen.app`'s stack, triggering infinite `shen.app`→`shen.arg->str`→`shen.atom->str` recursion for `defun` forms. Fix in `zinc-c` or KLambda IR — not in the bytecode.

## 2. `=` cons-vs-symbol HACK — REMOVED

**Status:** Fixed. Removed in commit after `019a969`.  
**Was:** `zinc-c` generated flat `(= [number 42] "number")` instead of `(= hd(hd(Code)) "number")`.  
**Resolution:** The zinc-c compiler now generates correct `hd`-wrapped comparisons. All 48 tests pass without any cons-vs-symbol special-casing in `=`. The hack was dead code.

## 3. `eval_kl` error swallowing

**File:** `vm/zincvm.c:1148-1151`  
**Symptom:** On error, `eval_kl` restores the caller's `jmp_buf` and returns the error value without re-raising.  
**Reason:** Shen's `load` doesn't wrap forms in `trap-error`, so a single failing form would abort the entire load.  
**Status:** Swallowing is intentional but fragile — hides genuine errors. Should re-raise once the load path wraps forms in `trap-error`.

## 4. `str` primitive — FIXED

**Status:** Fixed.  
**Was:** Metacircular interp only handled `[symbol A]` and C VM returned `""` for booleans/cons/nil/etc.  
**Fix:** 
- Interp: specific rules for symbol, number, string, boolean; catch-all delegates to host `(str A)`
- C VM: `str_value()` helper handles all types — cons (bracket notation with proper ` . ` dotted pairs), nil (`[]`), error (`<error msg>`), lambda (`<lambda>`), prim (`<prim name>`), vector/stream (`<vector N>`/`<stream>`)
- Matches shen-scheme's `put-datum` behavior: full printed representation for every type, never `""`
