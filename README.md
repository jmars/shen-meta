# shen-meta

[![CI](https://github.com/jmars/shen-meta/actions/workflows/ci.yml/badge.svg)](https://github.com/jmars/shen-meta/actions/workflows/ci.yml)

A meta-circular Shen ZINC abstract machine — Shen evaluating Shen, compiling itself to native bytecode, running on a native C VM.

This is a self-hosted Shen runtime: Shen compiles itself to native bytecode, running on a compact C VM. The meta-circular evaluator is the core — the ZINC abstract machine implemented in Shen, then serialized and loaded by the native VM.

This project is part of a larger architecture: sequent calculus provides the inference kernel (cut elimination as computation), LLMs handle pattern completion over the proof space, and Shen ties them together. But the VM itself is self-contained — a working Shen runtime that doesn't depend on anything beyond a C compiler.

Contributions welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Architecture

```
Shen source → kmacros → normalize → debruijn → zinc-c → csexp → C VM
                                                    ↑
                         interp.shen (meta-circular ZINC VM on Shen/Chez)
                                    ↓
                      serialize → globals.csexp → C VM (self-hosting)
```

| Layer | File | Description |
|---|---|---|
| **Normalizer** | `shen/normalize.shen` | KLambda expansion, A-normal form, debruijn indices |
| **ZINC compiler** | `shen/zinc.shen` | KLambda → ZINC bytecode |
| **Meta-circular VM** | `shen/interp.shen` | ZINC interpreter in Shen — loads, compiles, and runs Shen OS |
| **Safe wrappers** | `shen/primitives.shen` | Type-checked wrappers for all 37 primitives |
| **Eval/load** | `shen/toplevel.shen` | `interp-eval` — compiles `defun` forms through the pipeline |
| **KLambda loader** | `shen/load.shen` | Raw s-expression parser + `interp-load` for `.kl` files |
| **Utilities** | `shen/util.shen` | `defun->lambda`, `primitive?`, `dedupe-globals` |
| **Native compiler** | `shen/compile.shen` | ZINC → canonical s-expression bytecode |
| **C VM** | `vm/zincvm.c` | Native parser + VM (~1590 lines, all primitives, closures, tail calls) |
| **Serializer** | `shen/serialize.shen` | Compiles safe wrappers to csexp bundle for native VM |

## Build & Run

```sh
git clone --recurse-submodules https://github.com/jmars/shen-meta.git
cd shen-meta
make setup    # clone shen-scheme if not already present
make          # build C VM (requires gcc/clang)
make test     # run 28 built-in tests
make pipeline # compile (+ 1 2) through full pipeline
make bundle   # serialize all safe wrappers → globals.csexp
make run-bundle  # run C VM with self-hosting bundle
```

Requires [shen-scheme](https://github.com/tizoc/shen-scheme) (Shen 41.2 on Chez Scheme) at `../shen-scheme/`.

## Bytecode format

Canonical s-expressions with `[len:type]value` atoms:

- `s` — symbol, `n` — number, `S` — string, `b` — boolean
- Single-char opcodes: `m` pushmark, `p` apply, `u` push, `r` grab, `v` return, `c` cur, `g` global, `a` access, `f` jmpf, `j` jmp, `e` let, `d` endlet, `t` appterm, `n` number, `S` string, `s` symbol, `b` boolean, `P` prim

Example: `(+ 1 2)` → `(mn[1:n]2un[1:n]1ug[1:s]+p)`

**ZINC evaluates arguments right-to-left**: the rightmost arg is pushed first, the leftmost last (on top of stack). All two-arg C primitives pop `a1` (top = rightmost) then `a2` (below = leftmost). When writing bytecode by hand, push args in right-to-left order:

```
(open "Makefile" in) → (s[2:s]inuS[8:S]Makefileumg[8:s]raw.openp)
```

## Self-hosting proof

The C VM (`zincvm`) loads `globals.csexp` — a ~1.4MB bundle of **~1200 closures** compiled by the metacircular interpreter from all 24 Shen OS KLambda files. Four self-hosting tests run automatically:

| Test | What it proves | Status |
|---|---|---|
| 1 | `(+ 1 2)` via bundled safe.+ | Pass |
| 2 | `(reverse [1 2 3])` via bundled reverse | Pass |
| 3 | `(factorial 5)` via bundled factorial | Pass |
| 4 | `(raw.open/close)` — raw I/O primitives | Pass |
| A | `toplevel-interp([])` → `[cons]` | Pass |
| B | `toplevel-interp([number 42])` → `[number 42]` | Pass |
| C | `interp [] [cons] [] [] []` → `[cons]` | Pass |
| 5 | `eval-kl [+ 1 2]` via marshal chain | Pass |
| 6 | `read-file-as-string` via bundled safe wrapper (`P[4:s]open`) | Pass |
| 7 | `load` via bundled chain (`read-file` → `read-file-as-bytelist` → `P[4:s]open`) | Pass |

## Key design decisions

### raw.X primitive namespace

`parse_bundle` overwrites 37 C primitives with compiled safe wrapper closures in the global table. `%%` escapes inside safe wrappers use `OP_PRIM` → `exec_primitive` (bypasses global table), so safe wrapper internals still work. But bytecode that needs the C primitive directly uses `raw.X` names (`raw.open`, `raw.+`, `raw.eval-kl`, etc.). `exec_primitive` strips the `raw.` prefix before dispatch.

### Recursive eval-kl

`eval-kl` in the C VM delegates to the bundled `eval-kl` closure via `vm_exec_env` with a static recursion guard. The bundled closure (`safe.eval-kl`: `A → %% eval-kl A`) calls `%% eval-kl` which re-enters `exec_primitive`; the guard catches re-entry and returns identity as the base case.

## Status

- [x] 28 built-in VM tests (arithmetic, types, closures, error handling, I/O)
- [x] ~1200 bundled closures loaded and executing (full Shen OS)
- [x] `raw.X` namespace — bytecode calls C primitives directly, bypassing safe wrappers
- [x] Recursive `eval-kl` delegating to bundled closure
- [x] Raw I/O from C VM (`raw.open` / `raw.close` / `raw.read-byte`)
- [x] Missing KLambda primitives: `gensym`, `@p`, `fst`, `snd`, `variable?`
- [x] Full read-compile-eval round-trip (bundled `load` works — P[4:s]open chain functional)
- [ ] Bartlet GC integration (`~/github/bartlet-gc`, ~350 lines, conservative mostly-copying)
- [x] marshal_to_tagged / demarshal_from_tagged layer
- [x] Flat ZINC bytecode convention for interp family

## Roadmap

| Priority | Area | What |
|---|---|---|
| P0 | **Bartlett GC** | Wire the vendored GC into the C VM (conservative mostly-copying, ~350 lines) |
| P0 | **Primitive parity** | Port remaining KLambda primitives (`gensym`, `@p`, `fst`, `snd`, `variable?`) |
| P1 | **Self-hosted boot** | Drop the shen-scheme dependency — boot from a pre-compiled csexp bundle |
| P1 | **Error handling** | Improve backtrace and error reporting from the C VM |
| P2 | **Serialization spec** | Document the csexp bundle format end-to-end |
| P2 | **Benchmarks** | Execution time and memory usage scaffolding |
| P3 | **Performance** | Simple VM optimizations (inline caching, opcode dispatch) |

Contributors welcome on any of these. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Credits

ZINC abstract machine: Xavier Leroy (INRIA).
