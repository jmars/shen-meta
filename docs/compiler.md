# shen->kl compiler — limitations & roadmap

This documents the `shen->kl` compiler front-end: what it supports, its
constraints and limitations, and the improvement roadmap. It is the companion
to `architecture.md` / `AGENTS.md` for the **Shen-source → KLambda** stage of
the pipeline.

## Role in the pipeline

```
Shen source (.shen) → shen->kl → KLambda → (kl->zinc) → ZINC → nat->csexp → C VM
```

`shen->kl` (implemented in `shen/shen->kl.shen` + `shen/shen-kl-helpers.shen`)
is a **minimal Shen→KLambda compiler written in the type-safe Shen subset
itself**. Its purpose is to let the reduced self-contained bundle
(`globals.csexp`, built by `make bundle` → `shen/serialize-reduced.shen`) be
compiled by **our own toolchain** instead of host `shen-scheme`.

Why it exists: the host compiler emits curried partial-application calls
(e.g. `((debruijn []) N)`) that the C VM cannot run — the C VM has **no
partial application**. Our compiler emits **full-arity** bytecode only.

## What it supports (the safe subset)

Everything the meta-interpreter + its compiler pipeline need, all compiled
full-arity with no partial application:

- `define` with multi-clause, positional, nested-cons and literal patterns;
  `{ A --> B }` type signatures are **stripped** (arity comes from arg count).
- `lambda` and `/.` shorthand.
- Non-destructuring `let` (no `[A B]` destructuring — confirmed 0 uses).
- `if`, `and`/`or`, `do`, `set`.
- `where` guards and `<-` guarded clauses with `fail`-backtracking (folded
  into `if`/`cond` dispatch).
- `%%` escapes (`(%% F A..)` → `[prim F]`, bypassing the global table).
- `newvar`, `function`, `protect`.
- Constructors `[cons P1 P2]`, `@p`, `fst`, `snd`, `gensym`, `variable?`.

Compilation currently succeeds with **0 FAILs** for all 9 bundle `.shen`
files (util, types, zinc, compile, normalize, primitives, interp, toplevel,
load) plus the serialization helpers.

## Constraints & limitations

### Pattern compiler

- **No non-linear patterns** — you cannot repeat a variable in the same
  clause's patterns to express equality. `X [X | _] -> true` does **not**
  work: the second `X` silently overwrites the first binding. Rewrite using a
  `where` guard:

  ```shen
  (define element?
    _ [] -> false
    X [H | T] -> true where (= X H)
    X [_ | T] -> (element? X T))
  ```

  (This bit `assoc` too: `K [[K V] | T]` → use `K [[H V] | T] -> [H V]
  where (= K H)`.)
- `_` (wildcard) is supported; `variable?` slots bind.
- Only `cons` is a recognized constructor in patterns; tuple `@p` patterns
  are not supported.

### Body rewriting (`shen->kl-body`)

- `[X | Y]`-style application is rewritten via `shen-kl-expr`; a **list
  literal** `[a b c]` is consified into `(cons a (cons b (cons c [])))`,
  distinct from a call `(f a b c)`.
- `%%` is preserved as an application head (handled at `zinc-c`/`zinc-t`
  time).

### Guards / dispatch

- `<-` guarded clauses use a right-nested `if` tree with a `fail`-backtrack
  sentinel; `cond` for unguarded-only functions. A guarded clause whose body
  is `(if Cond RHS (fail))` folds the condition into the test (eliminating
  the `fail`, which throws on the C VM).

### Explicitly out of scope

- **No macro system** (`defmacro` not supported; `macroexpand` handled by
  host or hardcoded cases only).
- **No type checking / sequent calculus** — `{ A --> B }` signatures are
  parsed and discarded, not checked.
- **No `datatype`** (skipped).
- **No `freeze`/`thaw`/`cond` source sugar** — `cond` is supported at the
  clause level; explicit `freeze`/`thaw` source forms are not.
- **No partial application** — by design, the compiler produces full-arity
  calls only.

### Readers (two, deliberately separate)

- `.kl` reader (`read-file-raw` in `load.shen`) treats `{ } [ ] |` as ordinary
  **atoms** (needed for e.g. `(= { (hd ...))` in `shen.typetable`).
- `.shen` reader (`shen-read-file` in `shen-kl-helpers.shen`) treats `{ }`
  as a grouped type signature, `[ ]` as lists, `|` as dotted cdr.
- They MUST stay separate — merging them corrupts one or the other.

### Bundle composition

- The reduced bundle contains **no Shen OS `.kl` code** — every closure is
  compiled by our own `shen->kl`. The handful of OS list helpers the
  meta-interpreter needs (`append`, `reverse`, `empty?`, `assoc`,
  `element?`, `variable?`) are re-implemented in type-safe subset
  (`shen/os-helpers.shen` + `safe.variable?` in `primitives.shen`).
- Safe-wrapper aliases (`number? → safe.number?`, …) are installed by a
  data-driven loop in `serialize-reduced.shen`, pointing short names at
  our-compiled `safe.X` closures.

## Improvement roadmap

1. **Non-linear patterns** — teach the pattern compiler to emit an equality
   test (`= Slot X`) when a variable repeats in a clause, instead of silently
   overwriting. Would let `element?`/`assoc` use the natural form and remove
   the `where`-guard workaround.

2. **A proper type checker for the subset (Hindley–Milner / ML)** — the stated
   goal after the compiler is minimal and working. Types are currently
   stripped, not checked. An HM checker would gate the subset and let the
   meta-interpreter be *proven* type-safe.

3. **`@p` tuple patterns** — extend `compile-pattern` to destructure `@p`
   constructor patterns (currently only `cons`).

4. **Source `cond` / `case` sugar** — desugar explicit `cond`/`case` in
   bodies rather than relying on clause-level dispatch.

5. **`fail` as a primitive** — add `fail` to the `primitive?` list in both
   `util.shen` and `types.shen` so `zinc-c`/`zinc-t` emit `[prim fail]`
   directly instead of `[global fail]` (which currently falls back to
   `val_prim` via the global table).

6. **Macro system (deferred)** — only if the self-contained compiler needs
   to expand macros itself; currently not required.

7. **Fixed-point / curried-call tooling** — the `--curried` scan in `zincdec`
   (flags consecutive `apply`/`appterm` opcodes, i.e. curried calls) is the
   regression gate: our output must stay at **0 curried**. A true
   closure-bytecode-identity fixed-point test remains open (driving
   `kl->zinc`'s lambda path from C requires proper variable scoping — a
   harness concern, not a `marshal→extract-kl` defect).

8. **`add-prefix-aliases`** — likely unnecessary once no OS `.kl` remains
   (our closures emit bare `[global X]`, not `shen.X`). Candidate for a
   cleanup pass.

## Verification

```sh
make bundle             # 0 FAILs expected
make test               # 34/34 built-in bytecode tests
timeout 120 ./zinctest globals.csexp   # Self-hosting proven + Test F + GC
./zincdec globals.csexp --curried      # expect 0 curried calls
```
