# Bugs & known issues

## 1. Test 7e / typed-define: `read-from-string` hangs on `{ }` type annotations

**Status: FIXED.**

**Symptom:** `read-from-string "(define id {A --> A} X -> X)"` hung indefinitely. `read-from-string "(+ 1 2)"` worked fine. The `read-from-string-unprocessed` path (parse only) was instant.

**Root cause (stale `vm_error_jmp` in the trap-error handler path):**

The typed-define path runs `find-arities → store-arity → arity(id) → get → shen.<-dict → assoc`. Because `id` is not yet in the dict, `shen.<-dict` raises `simple-error "value id not found in dict"`. `arity` wraps this in `trap-error` whose handler returns `-1`.

The bug was in how that handler was executed. In `vm/zincvm.c`, the `trap-error` primitive's error path:

1. `te_pop()` restores the enclosing `vm_error_jmp` (sp 2→1).
2. Runs the handler via `vm_exec_env(...)`.
3. But `vm_error_pending` was left set by the `simple-error` (it is only cleared on the *first* entry to `trap-error`, at the top, not on the error path).
4. The handler's `vm_exec_env` therefore hit the rescue branch at the top of `vm_exec_env`:
   `if (vm_error_pending) { vm_error_pending = 0; setjmp(vm_error_jmp); }`
   — which **overwrites `vm_error_jmp`** with the handler's `setjmp` location, without `te_push()` (violating the documented invariant that every `setjmp(vm_error_jmp)` site pairs with `te_push`/`te_pop`).
5. When `vm_exec_env` returned, `vm_error_jmp` was left pointing into that returned (dangling) C frame.
6. The next error raised without an active `trap-error` (e.g. `simple-error "id has no attributes: arity"` during arity storage) `longjmp`'d to the dangling target, which looped forever (each `longjmp` returned to the stale `setjmp`, re-ran, and re-raised the same error).

**Fix (commit `3ed45b1` — final):** The whole error-handling design was refactored to
remove the root cause structurally. Instead of the global `vm_error_jmp` +
`error_jmp_stack` + `te_push()/te_pop()` + the rescue `setjmp(vm_error_jmp)` at the
top of `vm_exec_env`, the VM now uses a linked list of stack-allocated `CatchFrame`
structs. The rescue `setjmp` — which clobbered the global jmp_buf without pairing
with `te_push`, leaving a dangling target — is deleted; a `simple-error` raised in a
handler now propagates to the enclosing catch frame, and `vm_catch_chain` is
restored by plain frame unlink on every exit. An earlier intermediate fix (commit
`b2b1988`: clear `vm_error_pending` before running the handler) resolved the
immediate hang but was superseded by this refactor.

**Earlier (partial) fixes kept:** `overrides.kl` → `overrides-pure.kl` (removes `scm.*` dependencies), and switching the KLambda source to the standard Shen OS Kernel 41.2 distribution. These removed the broken `scm.*`-based `shen.<-dict`/`hash` that could not run in the C VM, but the hang persisted until the stale-jmp fix above.

**Regression test:** Added `read-from-string-typed-define` to the self-hosting suite: `(read-from-string "(define id { A --> A } X -> X)")` now returns `[[define id { A --> A } X -> X]]`. Note: it prints a benign `runtime: apply non-callable tag=5` warning during define macroexpansion (a NIL value is applied), which does not affect the result.

## 2. `=` cons-vs-symbol HACK — REMOVED

**Status:** Fixed.  
**Was:** `zinc-c` generated flat `(= [number 42] "number")` instead of `(= hd(hd(Code)) "number")`.  
**Resolution:** The zinc-c compiler now generates correct `hd`-wrapped comparisons.

## 3. `eval_kl` error swallowing

**File:** `vm/zincvm.c`  
**Symptom:** On error, `eval_kl` returns identity instead of re-raising.  
**Reason:** Shen's `load` doesn't wrap forms in `trap-error`.  
**Status:** Intentional but fragile. Now uses a `CatchFrame` (see AGENTS.md trap-error section).

## 4. `str` primitive — FIXED

**Status:** Fixed.  
**Fix:** `str_value()` handles all types with full `put-datum` representation.

## 5. Trap-error handler ping-pong — FIXED (da55d9b), then SUPERSEDED (3ed45b1)

**Symptom:** `get`'s handler's `simple-error` longjmp'd back to itself because `vm_error_jmp` wasn't restored.

**Root cause:** Single `vm_error_jmp` + manual save/restore couldn't handle nested trap-errors where the inner handler calls `simple-error`.

**Fix:** `error_jmp_stack[64]` + `te_push()`/`te_pop()`. `trap-error` pushes before `setjmp`. Handler pops FIRST so `simple-error` propagates outward. Also updated `eval-kl` and REPL code.

**Superseded:** The whole jmp_buf-stack design was replaced by the per-catch-site `CatchFrame` chain (commit `3ed45b1`); `te_push`/`te_pop` and `error_jmp_stack` no longer exist.

## 6. Error handling — remaining known limitations (post-CatchFrame refactor)

These are deliberate, preserved behaviors that the CatchFrame refactor (`3ed45b1`) did not change. Not regressions.

- **`eval-kl` swallows all pipeline errors** and returns the input unchanged (identity). `Shen`'s `load` path doesn't wrap forms in `trap-error`, so re-raising would expose pre-existing pipeline errors. This hides real user-code errors and compiler-pipeline bugs. A future fix would propagate a `VAL_ERROR` or rethrow via the catch chain and let callers wrap in `trap-error`.
- **C primitive type-errors are DEFENSE-IN-DEPTH, enabled only in debug builds (`ZINCVM_DEBUG`).** Primary ownership of catchable runtime errors is the Shen safe-wrapper layer (`shen/primitives.shen`): each `safe.X` validates its args and raises a catchable `simple-error` before the raw primitive is ever called. In release builds a primitive that still receives bad input prints `runtime: ...` and returns `-1` (a hard, non-catchable VM error) — correct, because the wrapper never forwards bad input. Only `make debug` (adds `-DZINCVM_DEBUG`) routes primitive type-errors through `vm_throw` (catchable inside `trap-error`) as a safety net for `%%`/`raw.` direct calls. Always-on (not safe-wrapper-protected): `simple-error`, `fail`, `apply`/`appterm` non-callable + too-many-args, `env_pop`, and eval-kl's catch.
- **The C type-checks are NOT compiled out in release for the FULL bundle.** Compiling out the pure type-tag checks (`PRIM_TYPE_CHECK` debug-only, `PRIM_VALUE_GUARD` always-on) makes release self-hosting of the FULL bundle segfault during `shen.initialise`. **Root cause (confirmed):** the full bundle contains the whole Shen OS `.kl` layer, which is type-unsafe (the type-unsafe code lives in the heavy OS — `prolog`/`yacc`/`sequent`/`t-star`/`stlib`/extensions — pulled in by `shen.initialise`). A **reduced bundle** (meta-interpreter `.shen` + the type-safe `.kl` base `core/declarations/types/macros/load/toplevel/sys/dict/track/reader/writer`, excluding the heavy OS) runs self-hosting cleanly with the C type-tag checks removed (exit 0, 798 closures) — confirming the meta-interpreter itself is type-safe and self-contained, and the crash is purely from the type-unsafe Shen OS. So: the C checks can be dropped only if the runtime is limited to the meta-interpreter subset (or the heavy OS is routed through safe wrappers).
- **Routing the eval-kl path through the safe wrappers (via the metacircular interp rules) was attempted and REVERTED.** Two mechanisms were tried: `((function X) ...)` compiled back to `[prim X]` (not `[global X]`) and introduced a nil-arg regression; `(apply X [args])` failed because `apply` is not a callable global in this context (`apply non-callable sym='apply'`). Both broke self-hosting. The interp rules are back to the original `(X ...)` form, so eval-kl'd dynamic code does NOT yet route through the safe wrappers — the design intent is documented in AGENTS.md but not implemented.
- **Safe wrappers now cover:** all the arithmetic (`+ - * /`, incl. division-by-zero), list (`hd`/`tl`/`fst`/`snd`/`cons`/`emptylist`), string (`n->string`/`string->n`/`tlstr`/`hdstr`/`str`), symbol (`intern`/`value`/`set`), vector (`absvector` incl. negative, `<-address`/`address->`), I/O (`open` incl. genuine open-failure, `close`, `read-byte`, `write-byte`), `get-time`, `pos`, `cn`, comparisons, `trap-error`, `simple-error`, `error-to-string`, and the type predicates. (`hdstr` and `read-file-as-string` required adding metacircular-interp `[prim ...]` rules so the eval-kl path can wrap them.)
- `val_error` messages are GC-allocated (no `strdup` leak). All error state is file-scope C statics (not thread-safe); the VM is single-threaded by design.
