# Bugs & known issues

## 1. Test 7e: runtime `.shen` load hangs in `shen.eval-and-print`

**File:** `vm/zincvm.c:2294-2303` (commented out)  
**Symptom:** Loading a `.shen` file at runtime via bundled `load` hangs when processing `define`/`defun` forms.  
**Chain:** `load` → `shen.load-help` → `shen.for-each` → `shen.eval-and-print` → `shen.shen->kl`  
**Status:** Skipped. The `=` prefix hack (removed in `ca18773`) was a premature attempt to fix this. Root cause is elsewhere.  
**Debug strategy:** Use `--trace shen.eval-and-print --trace shen.shen->kl-h` to find the actual loop.

## 2. `=` cons-vs-symbol HACK: flat-access pattern matching workaround

**File:** `vm/zincvm.c:621-640`  
**Symptom:** `zinc-c` generates `(= [number 42] "number")` instead of `(= hd(hd(Code)) "number")`.  
**Workaround:** `=` treats `(cons X) = symbol` as comparing the cons's car to the symbol. Form-head keywords (`define`, `defun`, `lambda`, `let`, `cond`, `if`) are excluded to avoid false matches on multi-element form lists.  
**Status:** Workaround in place. Root cause is in `zinc-c` generating one fewer `hd` than needed for nested pattern matching. Fix in the Shen compiler, not the VM.

## 3. `eval_kl` error swallowing

**File:** `vm/zincvm.c:1148-1151`  
**Symptom:** On error, `eval_kl` restores the caller's `jmp_buf` and returns the error value without re-raising.  
**Reason:** Shen's `load` doesn't wrap forms in `trap-error`, so a single failing form would abort the entire load.  
**Status:** Swallowing is intentional but fragile — hides genuine errors. Should re-raise once the load path wraps forms in `trap-error`.

## 4. `str` primitive in metacircular interp: symbol-only

**File:** `shen/interp.shen:129`  
**Symptom:** `[prim str | C] [symbol A] E S R` — only handles symbols.  
**TODO:** Extend to other datatypes (numbers, strings, booleans) like the C VM's `exec_primitive("str")` does.
