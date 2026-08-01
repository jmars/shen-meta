# Bugs & known issues

## 1. Test 7e: runtime `.shen` load hangs during `kl->zinc` compilation of `defun`

**File:** `vm/zincvm.c:2294-2303` (commented out)  
**Symptom:** Loading a `.shen` file containing `(defun ...)` at runtime via bundled `load` hangs. `(+ 1 2)` loads fine.

**Root cause:** During `eval-kl` processing of the `defun` form, `kl->zinc` compiles the KLambda. This triggers `shen.store-arity` → `shen.sysfunc?` → `element?` → `get` in a tight loop (24K calls before timeout). `shen.app` is called 63K times as a symptom of this arity-checking loop.

**Trace evidence:**
- `shen.store-arity`/`shen.sysfunc?`/`element?`/`get`: 24,295 calls
- `shen.app`: 63,586 calls  
- `shen.for-each`: 47 calls (was 0 before stale-push fix)
- Zero activity from: `shen.shen->kl-h`, `shen.record-and-evaluate`, `interp`, `toplevel-interp`, `kl->zinc`, `extract-kl` — the hang is in the C `eval-kl` primitive, not in metacircular execution

**Contributing factors fixed:**
1. Stale `push` instructions in bundle (old `zinc.shen` pre-auto-push) — fixed by `make bundle`; restored `shen.for-each` execution
2. `eval-kl` recursion guard returning identity — removed in `14b8d2d`; with Boehm GC deep recursion is safe

**Remaining issue:** The arity-checking chain (`shen.store-arity` → `shen.sysfunc?` → `element?` → `get`) loops inside `kl->zinc` specifically when compiling `defun` KLambda forms. The trigger is somewhere in how the bundled Shen compiler processes `defun` forms through the `extract-kl` → `kl->zinc` → `toplevel-interp` pipeline inside the C `eval-kl` primitive.

## 2. `=` cons-vs-symbol HACK — REMOVED

**Status:** Fixed. Removed in commit after `019a969`.  
**Was:** `zinc-c` generated flat `(= [number 42] "number")` instead of `(= hd(hd(Code)) "number")`.  
**Resolution:** The zinc-c compiler now generates correct `hd`-wrapped comparisons. All 48 tests pass without any cons-vs-symbol special-casing in `=`. The hack was dead code.

## 3. `eval_kl` error swallowing

**File:** `vm/zincvm.c:1148-1151`  
**Symptom:** On error, `eval_kl` restores the caller's `jmp_buf` and returns the error value without re-raising.  
**Reason:** Shen's `load` doesn't wrap forms in `trap-error`, so a single failing form would abort the entire load.  
**Status:** Swallowing is intentional but fragile — hides genuine errors. Should re-raise once the load path wraps forms in `trap-error`. Note: the recursion guard (eval_kl_depth) was removed in `14b8d2d` — nested eval-kl now executes instead of returning identity. The depth counter is retained for potential future diagnostics.

## 4. `str` primitive — FIXED

**Status:** Fixed.  
**Was:** Metacircular interp only handled `[symbol A]` and C VM returned `""` for booleans/cons/nil/etc.  
**Fix:** 
- Interp: specific rules for symbol, number, string, boolean; catch-all delegates to host `(str A)`
- C VM: `str_value()` helper handles all types — cons (bracket notation with proper ` . ` dotted pairs), nil (`[]`), error (`<error msg>`), lambda (`<lambda>`), prim (`<prim name>`), vector/stream (`<vector N>`/`<stream>`)
- Matches shen-scheme's `put-datum` behavior: full printed representation for every type, never `""`
