# shen-meta · self-hosted Shen

[![CI](https://github.com/jmars/shen-meta/actions/workflows/ci.yml/badge.svg)](https://github.com/jmars/shen-meta/actions/workflows/ci.yml)

**Shen evaluates Shen, compiles itself to native bytecode, runs on a native C VM with Boehm GC.** The full eval-kl chain is working: `(+ 1 2)` marshalled to tagged form, compiled through the metacircular pipeline, executed by the metacircular interpreter, demarshalled back to native — returns `3`. Pure self-hosting, no C bypass.

The meta-circular evaluator is the core — the ZINC abstract machine implemented in ~100 lines of Shen pattern-matching rules, serialized to ~0.5MB of bytecode, and loaded by a ~2600-line C VM. The VM is self-contained: a working Shen runtime that doesn't depend on anything beyond a C compiler and Boehm GC.

This project is part of a larger architecture: sequent calculus provides the inference kernel (cut elimination as computation), LLMs handle pattern completion over the proof space, and Shen ties them together. But the VM itself is self-contained.

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
| **Safe wrappers** | `shen/primitives.shen` | Type-checked wrappers for all primitives |
| **Eval/load** | `shen/toplevel.shen` | `interp-eval` — compiles `defun` forms through the pipeline |
| **KLambda loader** | `shen/load.shen` | Raw s-expression parser + `interp-load` for `.kl` files |
| **Utilities** | `shen/util.shen` | `defun->lambda`, `primitive?`, `dedupe-globals` |
| **Native compiler** | `shen/compile.shen` | ZINC → canonical s-expression bytecode |
| **C VM** | `vm/zincvm.c` | Native parser + VM (~2100 lines, GC, all primitives, closures, tail calls) |
| **Decompiler** | `vm/zincdec.c` | Standalone bytecode decompiler with 4 output formats |
| **Serializer** | `shen/serialize.shen` | Compiles safe wrappers to csexp bundle for native VM |

## Build & Run

```sh
git clone --recurse-submodules https://github.com/jmars/shen-meta.git
cd shen-meta
make setup    # clone shen-scheme if not already present
make          # build C VM + decompiler (requires gcc/clang)
make test     # run 32 built-in tests
make pipeline # compile (+ 1 2) through full pipeline
make bundle   # serialize all closures → globals.csexp
make run-bundle  # run C VM with self-hosting bundle
```

Requires [shen-scheme](https://github.com/tizoc/shen-scheme) (Shen 41.2 on Chez Scheme) at `../shen-scheme/`.

## Decompiler

Standalone binary for inspecting bundled bytecode:

```sh
./zincdec globals.csexp <function> [--raw|--asm|--shen|--csexp]
```

| Flag | Format | Example |
|---|---|---|
| `--raw` (default) | Human-readable opcodes | `access 0`, `global +`, `apply` |
| `--asm` | Disassembly w/ addresses | `0003: jmpf 7  ; -> 0007` |
| `--shen` | Shen list for `interp.shen` | `[access 0]`, `[global +]`, `apply` |
| `--csexp` | Raw wire format (round-trippable) | `(ra[1:n]1P[1:s]+p)` |

The old `./zincvm globals.csexp -d <name>` flag still works.

## Tracing

Trace execution of specific closures as they run:

```sh
./zincvm globals.csexp --trace + --trace reverse
```

Output shows each instruction in raw format with PC numbers:

```
[reverse]   0000  pushmark
[reverse]   0001  number 0
[reverse]   0002  prim emptylist
[reverse]   0003  access 0
[reverse]   0004  global shen.reverse-help
[reverse]   0005  appterm
```

Traces only the named function — not functions it calls (unless you `--trace` them too).

## Bytecode format

Canonical s-expressions with `[len:type]value` atoms:

- `s` — symbol, `n` — number, `S` — string, `b` — boolean
- Single-char opcodes: `m` pushmark, `p` apply, `r` grab, `v` return, `c` cur, `g` global, `a` access, `f` jmpf, `j` jmp, `e` let, `d` endlet, `t` appterm, `n` number, `S` string, `s` symbol, `b` boolean, `P` prim

Example: `(+ 1 2)` → `(mn[1:n]2n[1:n]1g[1:s]+p)`

**ZINC evaluates arguments right-to-left**: the rightmost arg is pushed first, the leftmost last (on top of stack). When writing bytecode by hand, push args in right-to-left order:

```
(open "Makefile" in) → (s[2:s]inS[8:S]MakefileP[4:s]open)
```

There is no `push` (`u`) opcode — all value-producing instructions auto-push their result, and the compiler relies on this (see AGENTS.md). To call an unchecked C primitive that a safe wrapper shadows in the global table, use the inline `P[...]` opcode (`P[4:s]open`, `P[7:s]eval-kl`), which dispatches via `exec_primitive` and bypasses the global table.

## Self-hosting

The C VM loads `globals.csexp` — a ~0.5MB reduced self-contained bundle of **~786 closures** compiled by the metacircular interpreter from the type-safe `.kl` base (core, declarations, types, macros, load, toplevel, sys, dict, track, reader, writer, plus `overrides-pure.kl` and `shen/util.shen`). The full eval-kl chain is proven: native values marshalled to tagged forms, compiled through `extract-kl → kl→zinc → toplevel-interp`, and demarshalled back. Shen compiles Shen, which runs on Shen, on the C VM.

**Self-hosting tests all pass:**

| Test | What it proves | Status |
|---|---|---|
| 1 | `(+ 1 2)` via bundled + | ✅ 3 |
| 2 | `(reverse [1 2 3])` via bundled reverse | ✅ [3 2 1] |
| 3 | `(factorial 5)` via bundled factorial | ✅ 120 |
| 4 | `(open/close)` — I/O via inline OP_PRIM | ✅ |
| A | `toplevel-interp([])` → `[cons]` | ✅ |
| B | `toplevel-interp([number 42])` → `[number 42]` | ✅ |
| C | `interp [] [cons] [] [] []` → `[cons]` | ✅ |
| 5 | **`eval-kl [+ 1 2]` via marshal chain** | ✅ **3** |
| 6 | `read-file-as-string` via bundled apply | ✅ |
| 7 | `load` via bundled chain | ✅ |
| 7b | `read-from-string "(+ 1 2)"` | ✅ [[+ 1 2]] |
| 7c | `read` via string stream | ✅ |
| 8 | `load shen/util.shen` | ✅ (functions load) |
| 9 | `id` closure (identity) | ✅ 42 |
| 10 | `newvar` (gensym) | ✅ shen.V0 |

## Key design decisions

### Boehm GC

Non-moving conservative collector (libgc). Objects never move — stack-local Value pointers are always safe across allocations. `GC_MALLOC`/`GC_MALLOC_ATOMIC` via macros `GC_VALUE()`, `GC_STR()`, `GC_VALUE_ARRAY()`. No extra roots needed. The old Bartlett copying GC is archived at `vendor/bartlett-gc` branch `bartlett-mostly-copying`.

### Calling C primitives past safe wrappers

`parse_bundle` overwrites C primitives with compiled safe wrapper closures. `%%` escapes and the inline `P[...]` opcode both dispatch via `OP_PRIM` → `exec_primitive`, bypassing the global table. There is no `raw.X` namespace — bytecode reaches an unchecked C primitive directly through `P[...]`.

### Recursive eval-kl

`eval-kl` delegates to the bundled closure via `vm_exec_env` with a recursion guard. The bundled `safe.eval-kl` (`A → %% eval-kl A`) re-enters `exec_primitive`; the guard catches re-entry and returns identity. The full chain is: `marshal → extract-kl → kl→zinc → toplevel-interp → demarshal`. The metacircular `interp` function (97 pattern-match rules in `shen/interp.shen`) was updated to auto-push old accumulator values, matching the standard ZINC semantics of `zinc.shen`.

## Status

- [x] 34 built-in VM tests (arithmetic, types, closures, error handling, I/O, trap-error routing)
- [x] ~786 bundled closures loaded and executing (reduced self-contained bundle)
- [x] **Self-hosting proven**: eval-kl chain returns `3` for `(+ 1 2)` — no C bypass
- [x] Boehm GC (non-moving, no pointer staleness)
- [x] `deep_equal` for cons==cons structural comparison
- [x] `P[...]` inline OP_PRIM dispatch — bytecode calls C primitives directly
- [x] Recursive `eval-kl` delegating to bundled closure
- [x] Raw I/O from C VM
- [x] KLambda primitives: `gensym`, `@p`, `fst`, `snd`, `variable?`
- [x] Full read-compile-eval round-trip (bundled `load` works)
- [x] `read-from-string` via YACC parser
- [x] marshal_to_tagged / demarshal_from_tagged layer
- [x] Metacircular interp auto-push (matches standard ZINC semantics)
- [x] Standalone bytecode decompiler (`zincdec`) with 4 output formats
- [x] Per-closure instruction tracing (`--trace`)
- [x] String stream support in `open` primitive
- [ ] REPL with interactive terminal I/O
- [ ] `shen.initialise` non-idempotency fix

## Credits

ZINC abstract machine: Xavier Leroy (INRIA).
Bartlett GC: James Marshall.
