# AGENTS.md — Project conventions for AI agents

## Build & test

```sh
make          # build C VM
make test     # run 28 built-in tests
make pipeline # compile (+ 1 2) through full pipeline
make bundle   # serialize all safe wrappers → globals.csexp
```

## Architecture

```
Shen source → kmacros → normalize-term → debruijn → zinc-c → compile-zinc → nat->csexp → C VM
```

Key files (under `shen/` unless noted):
- `shen/interp.shen` — meta-circular ZINC VM (loads everything)
- `shen/normalize.shen` — KLambda normalization + debruijn indices
- `shen/zinc.shen` — KLambda → ZINC bytecode compiler
- `shen/compile.shen` — ZINC → canonical s-expression (csexp)
- `shen/primitives.shen` — 37 type-checked safe wrappers
- `vm/zincvm.c` — native C parser + VM (~970 lines)
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

- csexp atoms: `[len:type]value` — type is `s`/`n`/`S`/`b`
- Opcodes are single chars: `m` pushmark, `p` apply, `u` push, `r` grab, `v` return, etc.
- `global` loads from table then falls back to `val_prim(name)`
- Primitives dispatch via `exec_primitive()` — apply-mode pops mark + args from stack
- Inline `OP_PRIM` (`P`) executes primitive with args from stack + accumulator (ZINC semantics)
- `trap-error`/`simple-error` use `setjmp`/`longjmp`

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
- ~1220 closures in bundle (1.3MB), 1189 loaded by C VM (parse_bundle returns on first error)
- All 24 KLambda files loaded: core through shen-scheme-extensions + stlib + init
- `read-file-raw` in `load.shen` parses `.kl` files without macro expansion
  using `read-file-as-string` + recursive descent with cached `strlen`
- `interp-load-raw` wraps `read-file-raw` for files with reader macro issues
- `read-file-as-string` C primitive added to zincvm.c for native file I/O from VM
- `vm.read-file` registered as alias in case bundled KLambda version overwrites it

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
- **`raw.X` namespace**: All 37 primitives that get overwritten by safe wrappers
  are also registered under `raw.X` names (e.g., `raw.open`, `raw.+`, `raw.eval-kl`).
  `exec_primitive` strips the `raw.` prefix before dispatching. Bytecode that
  needs the unchecked C primitive uses `raw.X` — this is how the read-compile-eval
  round-trip can call I/O primitives directly.
- `shen.repl`, `shen.read-evaluate-print`, `read`, `compile`, `eval-kl` are
  all in the bundle — the full Shen OS is available
- `raw.open` / `raw.close` / `raw.read-byte` / `raw.write-byte` enable I/O
  from bytecode without hitting safe wrapper currying
- `gensym`, `@p`, `fst`, `snd`, `variable?` — KLambda primitives added to
  both `primitive?` (Shen side) and `exec_primitive` (C side)

## ZINC argument convention

- **ZINC evaluates args RIGHT-TO-LEFT**: rightmost Shen arg pushed first,
  leftmost pushed last (on top of stack)
- All two-arg C primitives pop `a1` (top = rightmost) then `a2` (below =
  leftmost). E.g., `cons` does `val_cons(a1, a2)`, `-` does `a1 - a2`
- `open` was the exception — had `dir`/`path` swapped, causing "open bad
  types" in bundled `load`. Fixed: pop `path` first, then `dir`
- **When writing bytecode by hand**, push args in right-to-left order:
  `(s[2:s]in u S[8:S]Makefile u m g[8:s]raw.open p)` for `(open "Makefile" in)`
- Built-in tests use `m` (pushmark) before args; mark ends up at stack bottom,
  not popped by `OP_APPLY` with `VAL_PRIM` (mark must be on top to be popped)

## Commit style

- Conventional commits: `feat:`, `fix:`, `chore:`
- Don't commit compiled binaries
