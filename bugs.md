# Bugs & known issues

## 1. Test 7e: runtime `.shen` load hangs in `shen.eval-and-print`

**File:** `vm/zincvm.c:2294-2303` (commented out)  
**Symptom:** Loading a `.shen` file at runtime via bundled `load` hangs when processing `define`/`defun` forms.  
**Chain:** `load` → `shen.load-help` → `shen.for-each` → `shen.eval-and-print` → `shen.shen->kl`  
**Status:** Skipped. The `=` prefix hack (removed in `ca18773`) was a premature attempt to fix this. Root cause is elsewhere.  
**Debug strategy:** Use `--trace shen.eval-and-print --trace shen.shen->kl-h` to find the actual loop.

## 2. `=` cons-vs-symbol HACK — REMOVED

**Status:** Fixed. Removed in commit after `019a969`.  
**Was:** `zinc-c` generated flat `(= [number 42] "number")` instead of `(= hd(hd(Code)) "number")`.  
**Resolution:** The zinc-c compiler now generates correct `hd`-wrapped comparisons. All 48 tests pass without any cons-vs-symbol special-casing in `=`. The hack was dead code.

## 3. `eval_kl` error swallowing

**File:** `vm/zincvm.c:1148-1151`  
**Symptom:** On error, `eval_kl` restores the caller's `jmp_buf` and returns the error value without re-raising.  
**Reason:** Shen's `load` doesn't wrap forms in `trap-error`, so a single failing form would abort the entire load.  
**Status:** Swallowing is intentional but fragile — hides genuine errors. Should re-raise once the load path wraps forms in `trap-error`.

## 4. `str` primitive in metacircular interp: symbol-only

**File:** `shen/interp.shen:129`  
**Symptom:** `[prim str | C] [symbol A] E S R` — only handles symbols.  
**TODO:** Extend to other datatypes (numbers, strings, booleans) like the C VM's `exec_primitive("str")` does.
