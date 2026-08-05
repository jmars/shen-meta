# AGENTS.md — Project conventions for AI agents

## Build & test

```sh
make          # build C VM (release; links Boehm GC via -lgc)
make test     # run 34 built-in tests (release)
make debug    # build C VM with -DZINCVM_DEBUG (C-level type-error defense active)
make test-debug # run 39 built-in tests (adds debug-only trap-error tests 29-32b)
make pipeline # compile (+ 1 2) through full pipeline
make bundle   # serialize all safe wrappers → globals.csexp
make run-bundle  # run C VM with globals.csexp (self-hosting tests)

# Trace execution of specific closures:
./zincvm globals.csexp --trace + --trace reverse
```

## Architecture

```
Shen source → kmacros → normalize-term → debruijn → zinc-c → compile-zinc → nat->csexp → C VM
```

## Partial application (metacircular interp only)

The metacircular interpreter (`shen/interp.shen`) supports partial application:
when a closure is called with fewer arguments than its arity, instead of entering
the closure body, it creates a new closure capturing the provided arguments. This
is implemented via the `arity`, `count-args`, and `drop-grabs` helpers in `interp.shen`,
with arity checks in the `apply` and `appterm` rules.

**The C VM does NOT support partial application** — it is intentionally simpler.
The C VM runs only the subset of ZINC required for the meta-interpreter (the
reduced self-contained bundle), where all call sites are proven full-arity.  If
a bundled closure calls another with a short argument list, the metacircular
interp (which runs ON the C VM) handles it — the C VM never sees partial
application directly.  This split is intentional: the metacircular interp runs
full KLambda; the C VM runs only the statically-proven subset.

## Design intent (why static sites skip safe wrappers)

**The end goal:** the meta-circular interpreter (`interp` in `shen/interp.shen`) is
written in Shen and is meant to be PROVEN type-safe using the Shen sequent-calculus
type rules, and the C interpreter is meant to be GENERATED from that proven
interpreter (a static compiler that only compiles that subset, or by specialising
the interpreter). The C VM in `vm/zincvm.c` is a hand-written stand-in for that
generated interpreter.

Consequence — call sites split into two kinds:

- **Static call sites** — code produced by the compiler is type-safe by
  construction (it comes from the proven interpreter). These need NO runtime type
  check and NO safe wrapper. This is why `zinc-c`/`zinc-t` in `shen/zinc.shen`
  (lines 17-20 / 42-46) special-case `primitive?` heads to emit `[prim F]` — a
  direct primitive dispatch that BYPASSES the global table (and thus any `safe.X`
  wrapper registered via `set-toplevel`). This is intentional, NOT a bug.
- **Dynamic call sites** — boundaries, higher-order use, and untyped user input
  (e.g. `eval-kl` of `tc -` user code, `%%` escapes). These are NOT proven
  type-safe, so they must route through the Shen safe wrappers (`safe.X` in
  `shen/primitives.shen`) so a type error becomes a catchable `simple-error`.

`[global X]` → `safe.X` only fires on the dynamic path (a primitive used *as a
value*, higher-order, or explicit `(function X)`). Normal direct calls use
`[prim X]`. The C primitives' own type checks are therefore defense-in-depth and
are `ZINCVM_DEBUG`-only. In RELEASE they are **compiled out entirely** — the
`PRIM_TYPE_ERROR(msg)` macro expands to `((void)0)`, so GCC -O2 eliminates the
enclosing `if (cond) PRIM_TYPE_ERROR(...)` (no comparison, no type check, no
runtime cost). This is safe ONLY for a type-safe bundle: the canonical bundle
(`make bundle` → `globals.csexp`) is the **reduced self-contained interpreter**
(meta-interpreter + type-safe `.kl` base), which never passes bad types. The full
Shen OS bundle (`make bundle-full` → `globals-full.csexp`) is type-unsafe
(`shen.initialise` does `+ - * /` on non-numbers) and CANNOT run on the guard-free
release VM — it segfaults; run it with `./zincvm-debug globals-full.csexp`.

Always-on throw sites that are NOT type guards and stay in release: `simple-error`,
`fail`, `apply`/`appterm` non-callable + too-many-args, `env_pop`, `eval-kl` catch,
and `pos` out-of-bounds inside `trap-error` (semantic, needed for `strlen`/end-of-string).

## Self-hosting tests

| Test | Description | Status |
|---|---|---|
| 1-4 | +, reverse, factorial, open/close via inline OP_PRIM | Pass |
| A | toplevel-interp on `[]` → `[cons]` | Pass |
| B | toplevel-interp on `[number 42]` → `[number 42]` | Pass |
| C | interp `[] [cons] [] [] []` → `[cons]` | Pass |
| 5 | eval-kl `[+ 1 2]` via marshal chain | Pass |
| 6-7 | read-file-as-string, load via apply | Pass |
| 7b | read-from-string | Pass (returns [[+ 1 2]]) |
| 7b' | read-from-string typed define `{ A --> A }` | Pass (returns [[define id ...]]) — regression test for Bug #1 |
| 7c | read via string stream | Pass |
| 8-10 | id, newvar, defun->lambda (bundled via interp-load-raw) | Pass |

## Key files (under `shen/` unless noted):
- `shen/interp.shen` — meta-circular ZINC VM (loads everything)
- `shen/normalize.shen` — KLambda normalization + debruijn indices
- `shen/zinc.shen` — KLambda → ZINC bytecode compiler
- `shen/compile.shen` — ZINC → canonical s-expression (csexp)
- `shen/primitives.shen` — 37 type-checked safe wrappers
- `vm/zincvm.c` — native C parser + VM (~1800 lines, includes GC)
- `shen/serialize.shen` — compile all closures from global-table to csexp bundle
- `shen/toplevel.shen` — `interp-eval` — compiles defun forms through interpreter
- `shen/load.shen` — `interp-load` / `interp-load-raw` — file loading
- `shen/util.shen` — `defun->lambda`, `primitive?` (single source of truth), `dedupe-globals`
- `shen/types.shen` — type definitions + DUPLICATE `primitive?` list (must stay synced!)

## Shen quirks

- `tc -` to disable type checker; Shen 41.2 uses `___` not `===` for datatypes
- `defun` is Shen 22.2 syntax; use `define` or `defun->lambda` for 41.2
- `[X . Y]` is a dirty pair; `[X | Y]` is list cons
- `cn` takes exactly 2 args; string concat needs nested `cn` calls
- `print` outputs to stdout; script mode prints `fn`/`run time` noise from loads
- `eval` mode `-e` results are mixed with `-l` load output on stdout
- `-q` sets `*hush*` which gates `print` but not `write-byte`
- `%%` escapes to host Shen primitives; compiles to `[prim X]` in ZINC

## C VM conventions

- **GC**: custom generational collector (`vm/gc.c`, `vm/gc.h`, shared types in
  `vm/zinctypes.h`). A 2MB nursery (pages marked `space==3`) is the allocation
  fast lane; the existing full-copy `collect()` is the (rare) old-gen collector
  and compacts old gen. Typed headers drive a tag-dispatch scavenger;
  conservative C-stack scan pins ambiguous roots; typed `gc_scan_value`/
  `gc_evacuate` in `zincvm.c` handle interior pointers. Write barrier at
  `address->` vector writes (site 1, required); `global_set` barrier deferred
  (global_table is conservatively pinned as an extra_root). See `docs/gc.md`.

- csexp atoms: `[len:type]value` — type is `s`/`n`/`S`/`b`
- Opcodes are single chars: `m` pushmark, `p` apply, `r` grab, `v` return, etc.
- `global` loads from table then falls back to `val_prim(name)`
- Primitives dispatch via `exec_primitive()` — apply-mode pops mark + args from stack
- Inline `OP_PRIM` (`P`) executes primitive with args from stack + accumulator (ZINC semantics)
- `trap-error`/`simple-error` use `setjmp`/`longjmp`
- `eval_kl_depth` recursion guard: setjmp guard ensures depth always decremented even on
  longjmp from simple-error. Without this, a failed eval-kl blocks all subsequent calls.
- `--trace <name>`: trace every instruction of a specific closure as it executes.
  Repeatable. Output in raw format with PC numbers. E.g. `./zincvm globals.csexp --trace +`
  shows `[+]   0000  grab`, `[+]   0003  jmpf 7 (tgt=7)`, etc.
  Traces only the named function — not functions it calls (unless also --traced).

## Primitive semantics (critical — must match Shen)

- **`=`** now supports deep structural equality for cons cells via `deep_equal()`.
  Without this, `(= [+ 1 2] [+ 1 2])` returns false, breaking `macroexpand-h`'s
  fixed-point check.  Depth-limited to 1000 for cycle safety.
- **`=` symbol comparison** is strict `strcmp` — no prefix awareness.  Reference
  shen-scheme's `kl:=` uses plain `eq?` (pointer identity on symbols); `foo` and
  `shen.foo` are different symbols and must compare unequal.  The C VM MUST NOT
  add `shen.` prefix handling to `=` — prefix consistency is a pipeline concern.
  The pipeline enforces it via: `%% set` in normalize.shen → `shen.initialise`
  completes → `shen.external-symbols` populated → `sysfunc?` returns true for
  `define`/`defun`/`type`/etc. → `package-symbols` leaves them bare.  If `(= define
  shen.define)` ever returns false at runtime, the bug is in `shen.initialise`
  not completing (likely a missing `%%` on a primitive in normalize.shen), NOT in `=`.
- **`=` cons-vs-symbol** and symbol-vs-cons comparisons always return false.
  zinc-c currently generates correct `hd`-wrapped comparisons (e.g.,
  `(= hd(Code) define)`), so flat `(= [define ...] define)` no longer occurs.
- **`%%` in normalize**: ALL primitives must use `%%` prefix in normalize.shen.
  `[set S E]` was missing `%%`, causing `set` to go through `global set` + `apply`
  (safe wrapper) instead of `prim set` (direct C primitive).  This broke
  `shen.initialise`'s deeply nested `do` chain.  Always use `[%% set S T]`.
- `n->string N`: number → single-character string via ASCII code. `(n->string 40)` → `"("`
- `string->n S`: first character → ASCII code. `(string->n "(")` → `40`
- `pos S N`: single character at index N (0-based). OOB → `""`. `(pos "hello" 1)` → `"e"`
- `str V`: value→printed string. Numbers use decimal. Symbols use name. Strings pass through.
- `open Path Dir`: file I/O + string streams. ENOENT on `in` → creates string stream from Path.
  String stream data stored externally (not in Value union) to keep sizeof(Value) small.

## Pipeline gotchas

- `%%` compiles to `[prim X]` in ZINC; normalize must flatten curried `%%` calls
  via `flatten-%%app` or you get spurious `apply` after `prim` instructions
- `instr-count` and `label-positions` must handle opcodes with operands explicitly
  (`access _`, `global _`, `jmpf _`, `jmp _`, `number _`, `string _`, `symbol _`,
  `boolean _`, `prim _`) — catch-all `[_ | C]` counts operand atoms as separate
  instructions, inflating jump targets
- `cur` is 1 instruction in csexp stream, not `1 + body_size`
- `parse_bundle` must unwrap `OP_CUR` to get closure body — the `c(...)` wrapper
  is a single instruction whose operand is the closure's code array
- `ps` returns KLambda; unary primitives like `number?` lack `%%` wrapping in
  Shen 41.2 — normalize/debruijn need to handle bare primitives for inline `prim`
- `marshal_to_tagged` must NOT recursively tag VAL_CONS car/cdr. extract-kl handles its
  own recursion on `[cons X Y]`. Recursive marshalling creates impossibly deep nesting.
- ZINC bytecode for the interp family is FLAT: opcodes and operands are separate list elements.
  `[number 42]` = cons('number, cons(42, nil)). NOT cons(cons('number, cons(42, nil)), nil).
- `global` keyword registration: ZINC pattern keywords (number, symbol, cons, lambda, etc.)
  must be forced into the global table as symbols after parse_bundle, or self-compiled
  pattern-matching code resolves them as closures instead of tag symbols.

## Eval/load & serialization

- `interp-eval` in `toplevel.shen` compiles `defun` forms and stores closures in
  `global-table` via `defun->lambda → kl->zinc → toplevel-interp`
- `interp-load` in `load.shen` reads a file with `read-file`, feeds each `defun`
  through `interp-eval`; errors in individual forms are caught and skipped
- `serialize.shen` loads `interp.shen`, calls `interp-load` on `.kl` files to
  populate `global-table`, then walks the table and serializes all closures
- Output goes to file via `open`/`pr`/`close` — `print` wraps long lines
  and `grep '^"'` truncates multi-line bundles
- `pr` writes raw string to a stream; `(stoutput)` is stdout
- ~1216 closures in the FULL OS bundle (~1.4MB; ~1.6MB with prefix aliases) —
  `make bundle-full` → `globals-full.csexp`. The canonical **reduced**
  self-contained bundle (`make bundle` → `globals.csexp`) is ~786 closures / ~0.5MB.
- All 24 KLambda files loaded (full bundle): core through shen-scheme-extensions + stlib + init

### Shen module system & package prefixing

- The Shen package system prefixes ALL symbols (definitions AND references) with
  the module name (e.g. `shen.`) during Shen→KLambda compilation.  This happens
  in the Shen reader's `packageh` / `shen.package-symbols` functions.
- `.kl` files from shen-scheme are pre-compiled KLambda.  Most have the `shen.`
  prefix already baked in.  But `yacc.kl` defines `<e>`, `<!>`, `<end>` WITHOUT
  the prefix (generated by the YACC compiler, not the Shen compiler).
- Our bundle mixes two compilation paths:
  1. Our `.shen` tools (interp.shen, zinc.shen, etc.) — loaded via Shen's `load`,
     which adds the `shen.` prefix → bytecode references `shen.<e>`
  2. `.kl` files — loaded via `interp-load`, which stores closures under raw
     names → definitions at `<e>` without prefix
- **Fix**: serialize.shen runs `shen.add-prefix-aliases` after all interp-loads,
  creating `shen.<name>` entries for unprefixed closure names.  Both `<e>` and
  `shen.<e>` resolve to the same closure.
- **Do NOT add module prefix aliasing in the C VM** — it belongs at the Shen
  pipeline level, during bundle creation.  The C VM should only consume
  correctly-named bundles.  This applies especially to `=` and `deep_equal`:
  never make them `shen.`-prefix-aware; prefix consistency is enforced by the
  pipeline (see Primitive semantics above).
- `interp-load` does NOT apply package prefixing itself (unlike Shen's
  `eval-and-print` which goes through the reader).  The post-load aliasing step
  in serialize.shen is the correct place to reconcile the mismatch.

## Bytecode decompiler (`zincdec`)

Standalone binary for decompiling bundled closures. Four output formats:

```sh
./zincdec globals.csexp <function-name> [--raw|--asm|--shen|--csexp]
```

| Flag | Format | Example |
|---|---|---|
| `--raw` (default) | Human-readable opcodes | `access 0`, `global +`, `apply` |
| `--asm` | Disassembly w/ addresses | `0000: access 0`, `0003: jmpf 7  ; -> 0007` |
| `--shen` | Shen list for `interp.shen` | `[access 0]`, `[global +]`, `apply` |
| `--csexp` | Raw wire format (round-trippable) | `(ra[1:n]1P[7:s]number?f[1:n]7...)` |

Examples: `./zincdec globals.csexp +`, `./zincdec globals.csexp reverse --csexp`, `./zincdec globals.csexp shen.repl --shen`

The old `./zincvm globals.csexp -d <name>` flag still works for quick inspection.
- `read-file-raw` in `load.shen` parses `.kl` files without macro expansion
  using `read-file-as-string` + recursive descent with cached `strlen`
- `interp-load-raw` wraps `read-file-raw` for files with reader macro issues
- `read-file-as-string` C primitive added to zincvm.c for native file I/O from VM

## Raw s-expression parser (`load.shen`)

- `read-file-raw Path` — reads file, parses all forms without macro expansion
- Uses `(n->string N)` for all special chars (avoids Shen's `\` escape issues)
- Shen 41.2 does NOT interpret `\n`/`\t`/`\r` in string literals — use `(n->string 10)` etc.
- `\` in KLambda strings is literal (not escape) — `parse-string-chars` reads until `"`
- `strlen` cached once per parse; all parsers thread `Len` parameter
- `let` destructuring `[A B]` does NOT work with `tc -`; use `hd`/`tl` on returned pairs

## KLambda primitives (added to `primitive?` + `interp` handlers)

- `@p` — tuple constructor, stored as cons cell `[cons A B]`
- `fst`/`snd` — tuple accessors, aliases for `hd`/`tl`
- `gensym` — fresh symbol generation
- `variable?` — predicate for KLambda variable symbols

## Shen pitfalls

- `let` DOES work with `tc -` (verified), just types in `define` aren't checked
- `let` destructuring `[A B]` does NOT work with `tc -`; use `hd`/`tl` on returned pairs
- `type` signatures in `define` ARE accepted with `tc -` (just not checked)
- `read-file` returns a list of parsed s-expressions from a file — works for
  both `.shen` and `.kl` files
- `.kl` files use raw KLambda constructs: `defun`, `lambda`, `let`, `cond`,
  `@p`, `where`, `freeze`, `thaw`, `cons?`, `=`, `if`, etc.

## Self-hosting & C VM gotchas

- `GLOBAL_TABLE_MAX` was 256 — bumped to 2048 to hold all ~1200 closures
- `global_get` falls back to `val_prim(name)` for missing names — can cause
  "unknown primitive" errors if a bundled closure overwrites a C primitive
  and then something expects the raw primitive
- Bundled safe wrappers (safe.+, safe.open, safe.string? etc.) overwrite
  C primitives in global table since `parse_bundle` runs after `init_globals`
- `%%` escapes compile to `[prim X]` which calls `exec_primitive` directly,
  bypassing the global table — so safe wrapper internals still work
- Bytecode that needs an unchecked C primitive (bypassing safe-wrapper shadowing)
  uses the inline `OP_PRIM` dispatch (`P[4:s]open`, `P[7:s]eval-kl`, etc.) — the
  same path `%%` escapes use. There is NO `raw.X` namespace; the C primitives are
  reached only via `OP_PRIM` (direct) or through a safe wrapper (global table).
- `shen.repl`, `shen.read-evaluate-print`, `read`, `compile`, `eval-kl` are
  all in the bundle — the full Shen OS is available
- `gensym`, `@p`, `fst`, `snd`, `variable?` — KLambda primitives added to
  both `primitive?` (Shen side) and `exec_primitive` (C side)

## ZINC argument convention

- **ZINC evaluates args RIGHT-TO-LEFT**: rightmost Shen arg pushed first
  (ends at stack bottom), leftmost pushed last (on top of stack)
- All two-arg C primitives pop `a1` (top = leftmost arg) then `a2` (below =
  rightmost arg). E.g., for `(- 5 3)`: stack `[3, 5]`, pop a1=5, a2=3,
  compute `a1 - a2` = 5-3 = 2. `cons` does `val_cons(a1, a2)` = cons(left, right).
- `open` was the exception — had `dir`/`path` swapped, causing "open bad
  types" in bundled `load`. Fixed: pop `path` first, then `dir`
- **When writing bytecode by hand**, push args in right-to-left order:
  `(s[2:s]in S[8:S]Makefile P[4:s]open)` for `(open "Makefile" in)`
  (inline `OP_PRIM` `P[...]` dispatches directly, bypassing the global table)
- **CRITICAL**: Hand-written bytecode MUST use RTL order. The VM pops
  top-first (leftmost arg). Writing LTR (natural reading order) works
  for commutative ops (+, =, cons-as-pair) but silently produces wrong
  results for non-commutative ops (-, /, trap-error, write-byte).
  This is the #1 recurring bug pattern. See tests 27-32 for examples.
- Built-in tests use `m` (pushmark) before args; mark ends up at stack bottom,
  not popped by `OP_APPLY` with `VAL_PRIM` (mark must be on top to be popped)
- **Built-in tests**: 34 hand-written bytecode tests in `vm/zincvm.c` (release build; 39 in `ZINCVM_DEBUG`, which adds debug-only C-primitive trap-error tests 29-32b).
  Tests 1-32 exercise apply ('p'), tests 33-38 exercise appterm ('t').
  Built-in tests run without a bundle (`./zincvm`). Self-hosting tests
  (10) + GC stress run with bundle (`./zincvm globals.csexp`).
- `appterm` ('t') and `apply` ('p') share identical stack layout:
  `[mark, argN..arg1, function]`. Difference: appterm reuses current frame
  (tail-call, pc=0), apply pushes new CallFrame. Both reject >64 args.
  Appterm additionally rejects zero args and requires pushmark.

## Commit style

- Conventional commits: `feat:`, `fix:`, `chore:`
- Don't commit compiled binaries

## trap-error / primitive error handling

Error handling uses a **per-catch-site linked list of stack-allocated `CatchFrame`
structs** (commit `3ed45b1`). This replaced the earlier global `vm_error_jmp` +
`error_jmp_stack[64]` + `te_push()/te_pop()` memcpy save/restore design, and the
rescue `setjmp(vm_error_jmp)` at the top of `vm_exec_env`.

- `CatchFrame { jmp_buf buf; Value error_val; int in_trap_error; struct CatchFrame *parent; }`
  + a `vm_catch_chain` head + `vm_throw(msg)` (writes `error_val` into the chain
  head, then `longjmp`s to its `buf`; if the chain is empty it prints and `abort()`s).
- Each catch site (trap-error, eval-kl, run_test_timeout, main initialise + REPL,
  self-hosting Tests A/B/C) declares a local `CatchFrame`, links it onto the chain,
  and `setjmp(cf.buf)`. On the error path the frame is **unlinked first** so a
  `simple-error` raised inside a handler propagates to the enclosing frame.
- `simple-error` always `vm_throw`s to the current chain head. Inside a trap-error
  BODY the frame's `in_trap_error=1`, so type-error primitives throw too. **However,
  C-level type-error routing is DEFENSE-IN-DEPTH, compiled only in `ZINCVM_DEBUG`
  builds** (the `PRIM_TYPE_ERROR` macro + inline guards). Primary ownership is the
  Shen safe-wrapper layer (`shen/primitives.shen`): each `safe.X` validates args and
  raises a catchable `simple-error` before the raw primitive is called. In RELEASE
  `PRIM_TYPE_ERROR` expands to `((void)0)` so the guards compile out entirely (see
  "Design intent" above). The always-on throw sites are those NOT protected by a
  safe wrapper and not type guards: `simple-error`, `fail`, `apply`/`appterm`
  non-callable + too-many-args, `env_pop`, `pos` OOB inside `trap-error`, and
  eval-kl's catch. Debug-only tests 29-32b verify the C-primitive-level defense path.
- `val_error` GC-allocates its message (no `strdup` leak).
- The `alarm_jmp` (test TIMEOUT) and `repl_exit_jmp` (REPL EOF) mechanisms are
  separate from the catch chain and unaffected.
- This routes OOB access sentinels (tag=0,n=0 from empty-env vm_exec calls) through
  error handlers, letting `bound?` correctly return false for unbound symbols.

## ZINC calling convention (STANDARD — fully aligned)

The VM now uses **standard ZINC** semantics: all value-producing opcodes push
results to the stack AND set `acc`. There is no `push` opcode — the compiler
relies on auto-push (see "Compiler changes" below).

**Opcodes that push to stack:**
- `OP_NUMBER`, `OP_STRING`, `OP_SYMBOL`, `OP_BOOLEAN` — push operand
- `OP_ACCESS` — push env lookup result
- `OP_GLOBAL` — push global table lookup result
- `OP_CUR` — push newly created closure
- `OP_PRIM` — push primitive result (after execution, no pre-push)
- `OP_APPLY` (VAL_PRIM) — push primitive result
- `OP_APPTERM` (VAL_PRIM) — push primitive result
- `OP_RETURN` — push return value to caller's stack

**Opcodes that pop from stack:**
- `OP_JMPF` — pops condition from stack
- `OP_LET` — pops value from stack (binds to env)
- `OP_APPLY` / `OP_APPTERM` — pop function from stack top, then args up to mark

**Compiler changes:**
- `shen/zinc.shen` (`zinc-c` and `zinc-t`): relies on auto-push. Multi-arg
  primitives and function calls emit bare operand sequences — no `push` opcode.
- The `push` (`u`) opcode has been REMOVED from the C VM, the compiler pipeline
  (compile.shen/util.shen/types.shen), and the metacircular interp. It is dead:
  the compiler never emits it, and the bundle/test bytecode contain no `u`.
  `pushmark` (`m`) remains and is still emitted by `zinc-c`/`zinc-t`.

**Bundle recompiled:** `globals.csexp` rebuilt with modified zinc.shen.
All bundled closures now use push semantics natively.

### Metacircular interp — auto-push refactored (DONE)

The metacircular `interp` in `interp.shen` now implements standard ZINC auto-push
semantics. Only 7 value-instruction rules were changed — each pushes the **old**
accumulator to the stack before setting the new value:

```
[access N | C] A E S R    → (interp C (lookup N E) E [A | S] R)
[global G | C] A E S R    → (interp C (lookup-global G) E [A | S] R)
[cur C1 | C] A E S R      → (interp C [lambda C1 E] E [A | S] R)
[number N | C] A E S R    → (interp C [number N] E [A | S] R)
[string Ss | C] A E S R   → (interp C [string Ss] E [A | S] R)
[symbol Ss | C] A E S R   → (interp C [symbol Ss] E [A | S] R)
[boolean B | C] A E S R   → (interp C [boolean B] E [A | S] R)
```

All 85 other rules (binary prims, unary prims, jmpf, let, grab, return, etc.)
remain unchanged. Binary prim rules like `[prim + | C] [number A] E [[number A1] | S] R`
work correctly because auto-push leaves the previous value on the stack top, which
is exactly the rightmost argument.

**The C bridge (push insertion in eval_kl) has been REMOVED.** No bytecode
transformation is needed — the interp natively handles standard ZINC output.

**Key design choice:** Pushing OLD accumulator (not new value) means the accumulator
remains the "current value" at every step, keeping all existing prim rules compatible.

### Metacircular interp — apply/appterm (DONE)

- **Multi-arg `apply`**: Uses `collect-apply-args` helper to collect all args
  up to the mark (skipping A0+mark), then gives callee a fresh stack with
  caller context saved as `[C E Rest]` return frame.
- **Multi-arg `appterm`**: Same arg collection via `collect-apply-args`.
  Tail-call semantics: replaces saved stack in return frame (or starts fresh
  at top level). Zero-arg check added (matches C VM).
- **`collect-apply-args`**: depth-limited (max 64 args, matches C VM),
  errors if mark is missing, signature `(list zinc-value) -> number -> (list zinc-value)`.
- **Env ordering fix**: `(append (reverse Args) E1)` — newest bindings at
  head of env list, matching forward `lookup` in metacircular interp.
  Equivalent to C VM's reverse-index `lookup_env`.

## REPL

- `stinput`/`stoutput` C primitives added — return stdin/stdout VAL_STREAM.
  Registered in init_globals so bundled closures find them via `global_get`.
- `fflush(stdout)` in `write-byte` for piped output.
- `shen.initialise` (15-char name) must be called before `shen.repl`. Wraps
  `shen.initialise-environment` → `shen.initialise-lambda-forms` →
  `shen.initialise-signedfuncs`.
- REPL is functional. `shen.initialise` + `shen.repl` both execute and return.
  shen.initialise is non-idempotent: first call errors "set: first arg must be
  a symbol" (caught by trap-error), second call returns false. In test mode
  (stdin at EOF), shen.repl returns false immediately.
- **Key fixes enabling REPL:**
  - `*stinput*`/`*stoutput*`/`*sterror*` initialized as VAL_STREAM globals after parse_bundle
  - `write-byte` arg order fixed (ZINC RTL: byte first, stream last)
  - CALL_STACK_DEPTH bumped from 8192 to 65536 (shen.initialise needs ~65K frames)
  - GC heap at 256MB (64MB exhausted on non-ASan builds)
  - Stack isolation per CallFrame (commit 00299cf)
  - read-byte/write-byte bypass stack for stream args (commit 6247571)
  - trap-error jmp_buf save/restore to prevent use-after-return (commit 6247571)
  - Tail-call mark cleanup in OP_RETURN/OP_APPTERM (commit 27bdcbe)
- `shen.initialise` REPL bytecode: `(mn[1:n]0ug[15:s]shen.initialisep)`
- `shen.repl` with input: `(mn[1:n]0P[9:s]emptylistus[7:s]successP[4:s]consug[9:s]shen.replp)`
- Name confusion: `shen.initialise_environment` (underscore, 27 chars) is a
  DIFFERENT function — only resets shen.*call*/shen.*infs* counters. Called by
  shen.loop each iteration. Not the setup function.
