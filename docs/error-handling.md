# Error handling (trap-error / primitive errors)

Sources: `AGENTS.md` trap-error section + `bugs.md`.

## Design: per-catch-site CatchFrame chain

Error handling uses a **per-catch-site linked list of stack-allocated `CatchFrame`
structs** (commit `3ed45b1`). This replaced the earlier global `vm_error_jmp` +
`error_jmp_stack[64]` + `te_push()/te_pop()` memcpy save/restore design, and the
rescue `setjmp(vm_error_jmp)` at the top of `vm_exec_env`.

- `CatchFrame { jmp_buf buf; Value error_val; int in_trap_error; struct CatchFrame *parent; }`
  plus a `vm_catch_chain` head + `vm_throw(msg)` (writes `error_val` into the chain
  head, then `longjmp`s to its `buf`; if the chain is empty it prints and `abort()`s).
- Each catch site (trap-error, eval-kl, run_test_timeout, main initialise + REPL,
  self-hosting Tests A/B/C) declares a local `CatchFrame`, links it onto the chain,
  and `setjmp(cf.buf)`. On the error path the frame is **unlinked first** so a
  `simple-error` raised inside a handler propagates to the enclosing frame.
- `simple-error` always `vm_throw`s to the current chain head. Inside a trap-error
  BODY the frame's `in_trap_error=1`, so type-error primitives throw too.

### C-level type-error routing is DEFENSE-IN-DEPTH

Compiled only in `ZINCVM_DEBUG` builds (the `PRIM_TYPE_ERROR` macro + inline
guards). Primary ownership of catchable runtime errors is the Shen safe-wrapper
layer (`shen/primitives.shen`): each `safe.X` validates args and raises a
catchable `simple-error` before the raw primitive is called. In RELEASE
`PRIM_TYPE_ERROR` expands to `((void)0)` so the guards compile out entirely. The
always-on throw sites are those NOT protected by a safe wrapper and not type
guards: `simple-error`, `fail`, `apply`/`appterm` non-callable + too-many-args,
`env_pop`, `pos` OOB inside `trap-error`, and eval-kl's catch. Debug-only tests
29-32b verify the C-primitive-level defense path.

### Other mechanisms

- `val_error` GC-allocates its message (no `strdup` leak).
- The `alarm_jmp` (test TIMEOUT) and `repl_exit_jmp` (REPL EOF) mechanisms are
  separate from the catch chain and unaffected.
- This routes OOB access sentinels (tag=0,n=0 from empty-env vm_exec calls)
  through error handlers, letting `bound?` correctly return false for unbound
  symbols.

## Historical bug (fixed) — stale jmp target caused an infinite hang

**Bug #1 / #5 in `bugs.md`:** `read-from-string "(define id {A --> A} X -> X)"`
hung indefinitely.

**Root cause:** The typed-define path runs `find-arities → store-arity →
arity(id) → get → shen.<-dict → assoc`. Because `id` is not yet in the dict,
`shen.<-dict` raises `simple-error "value id not found in dict"`, which `arity`
wraps in `trap-error` whose handler returns `-1`. In the old global-jmp_buf
design:

1. `te_pop()` restored the enclosing `vm_error_jmp` (sp 2→1).
2. The handler ran via `vm_exec_env(...)`.
3. But `vm_error_pending` was left set by the `simple-error` (only cleared on
   first entry to `trap-error`, not on the error path).
4. The handler's `vm_exec_env` hit the rescue branch:
   `if (vm_error_pending) { vm_error_pending = 0; setjmp(vm_error_jmp); }`
   — overwriting `vm_error_jmp` with the handler's `setjmp` location without
   `te_push()` (violating the documented invariant).
5. When `vm_exec_env` returned, `vm_error_jmp` pointed into a dangling C frame.
6. The next error without an active `trap-error` longjmp'd to the dangling
   target, looping forever.

**Fix (`3ed45b1`):** the structural CatchFrame refactor above. The rescue setjmp
is deleted; a `simple-error` in a handler propagates to the enclosing frame, and
`vm_catch_chain` is restored by plain frame unlink. An earlier intermediate fix
(commit `b2b1988`: clear `vm_error_pending` before running the handler) resolved
the immediate hang but was superseded.

**Regression test:** `read-from-string-typed-define` — `(read-from-string
"(define id { A --> A } X -> X)")` returns `[[define id { A --> A } X -> X]]`.
Prints a benign `runtime: apply non-callable tag=5` warning during define
macroexpansion (a NIL value is applied), which does not affect the result.

## Remaining known limitations (post-CatchFrame; deliberate)

- **`eval-kl` swallows all pipeline errors** and returns the input unchanged
  (identity). Shen's `load` path doesn't wrap forms in `trap-error`, so re-raising
  would expose pre-existing pipeline errors. This hides real user-code errors and
  compiler-pipeline bugs. A future fix would propagate a `VAL_ERROR` or rethrow
  via the catch chain.
- **C primitive type-errors are debug-only defense** (see above).
- **The guard-free release VM only runs the REDUCED, type-safe bundle.**
- **Close-the-loop (runtime `.kl` loading) is PARTIAL** — defun registration does
  not yet work. See `docs/loading.md`.
- **Safe wrappers now cover:** all arithmetic (`+ - * /`, incl. division-by-zero),
  list (`hd`/`tl`/`fst`/`snd`/`cons`/`emptylist`), string (`n->string`/`string->n`/
  `tlstr`/`hdstr`/`str`), symbol (`intern`/`value`/`set`), vector (`absvector`
  incl. negative, `<-address`/`address->`), I/O (`open` incl. genuine
  open-failure, `close`, `read-byte`, `write-byte`), `get-time`, `pos`, `cn`,
  comparisons, `trap-error`, `simple-error`, `error-to-string`, and the type
  predicates. (`hdstr` and `read-file-as-string` required adding
  metacircular-interp `[prim ...]` rules so the eval-kl path can wrap them.)
- `val_error` messages are GC-allocated. All error state is file-scope C statics
  (not thread-safe); the VM is single-threaded by design.
