# Shen quirks & pitfalls

Source: `AGENTS.md`. Language quirks, pitfalls, and self-hosting gotchas for
working on the Shen source and the C VM.

## Shen quirks

- `tc -` to disable type checker; Shen 41.2 uses `___` not `===` for datatypes.
- `defun` is Shen 22.2 syntax; use `define` or `defun->lambda` for 41.2.
- `[X . Y]` is a dirty pair; `[X | Y]` is list cons.
- `cn` takes exactly 2 args; string concat needs nested `cn` calls.
- `print` outputs to stdout; script mode prints `fn`/`run time` noise from loads.
- `eval` mode `-e` results are mixed with `-l` load output on stdout.
- `-q` sets `*hush*` which gates `print` but not `write-byte`.
- `%%` escapes to host Shen primitives; compiles to `[prim X]` in ZINC.

## Shen pitfalls

- `let` DOES work with `tc -` (verified), just types in `define` aren't checked.
- `let` destructuring `[A B]` does NOT work with `tc -`; use `hd`/`tl` on
  returned pairs.
- `type` signatures in `define` ARE accepted with `tc -` (just not checked).
- `read-file` returns a list of parsed s-expressions from a file — works for both
  `.shen` and `.kl` files.
- `.kl` files use raw KLambda constructs: `defun`, `lambda`, `let`, `cond`, `@p`,
  `where`, `freeze`, `thaw`, `cons?`, `=`, `if`, etc.

## Self-hosting & C VM gotchas

- `GLOBAL_TABLE_MAX` was 256 — bumped to 2048 to hold all ~1200 closures.
- `global_get` falls back to `val_prim(name)` for missing names — can cause
  "unknown primitive" errors if a bundled closure overwrites a C primitive and
  then something expects the raw primitive.
- Bundled safe wrappers (safe.+, safe.open, safe.string? etc.) overwrite C
  primitives in global table since `parse_bundle` runs after `init_globals`.
- `%%` escapes compile to `[prim X]` which calls `exec_primitive` directly,
  bypassing the global table — so safe wrapper internals still work.
- Bytecode that needs an unchecked C primitive (bypassing safe-wrapper shadowing)
  uses the inline `OP_PRIM` dispatch (`P[4:s]open`, `P[7:s]eval-kl`, etc.) — the
  same path `%%` escapes use. There is NO `raw.X` namespace; the C primitives are
  reached only via `OP_PRIM` (direct) or through a safe wrapper (global table).
- `shen.repl`, `shen.read-evaluate-print`, `read`, `compile`, `eval-kl` are all
  in the bundle — the full Shen OS is available.
- `gensym`, `@p`, `fst`, `snd`, `variable?` — KLambda primitives added to both
  `primitive?` (Shen side) and `exec_primitive` (C side).

## Commit style

- Conventional commits: `feat:`, `fix:`, `chore:`.
- Don't commit compiled binaries.
