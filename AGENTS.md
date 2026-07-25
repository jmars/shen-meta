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

Key files:
- `interp.shen` — meta-circular ZINC VM (loads everything)
- `normalize.shen` — KLambda normalization + debruijn indices
- `zinc.shen` — KLambda → ZINC bytecode compiler
- `compile.shen` — ZINC → canonical s-expression (csexp)
- `primitives.shen` — 37 type-checked safe wrappers
- `zincvm.c` — native C parser + VM (~970 lines)
- `serialize.shen` — compile safe wrappers to csexp bundle

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
- Inline `OP_PRIM` pushes acc before calling exec_primitive, skips trailing `p`
- `trap-error`/`simple-error` use `setjmp`/`longjmp`

## Commit style

- Conventional commits: `feat:`, `fix:`, `chore:`
- Don't commit compiled binaries
