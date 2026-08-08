# shen-meta · self-hosted Shen

[![CI](https://github.com/jmars/shen-meta/actions/workflows/ci.yml/badge.svg)](https://github.com/jmars/shen-meta/actions/workflows/ci.yml)

**Shen evaluates Shen, compiles itself to native bytecode, runs on a native C VM.** The full eval-kl chain is working: `(+ 1 2)` marshalled to tagged form, compiled through the metacircular pipeline, executed by the metacircular interpreter, demarshalled back to native — returns `3`. Pure self-hosting, no C bypass.

The meta-circular evaluator is the core — the ZINC abstract machine implemented in ~100 lines of Shen pattern-matching rules, serialized to ~0.33MB of bytecode, and loaded by a ~2600-line C VM with a custom moving generational garbage collector. The VM is self-contained: a working Shen runtime that depends only on a C compiler and our own GC (no Boehm, no host runtime).

The reduced self-contained bundle is compiled entirely by **our own `shen->kl` compiler** — a minimal Shen→KLambda front-end written in the type-safe Shen subset itself. It contains **no Shen OS `.kl` code**; the few list helpers the meta-interpreter needs (`append`, `reverse`, `empty?`, `assoc`, `element?`, `variable?`) are re-implemented in the safe subset.

This project is part of a larger architecture: sequent calculus provides the inference kernel (cut elimination as computation), LLMs handle pattern completion over the proof space, and Shen ties them together. But the VM itself is self-contained.

Contributions welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Architecture

```
Shen source → shen->kl (own compiler) → KLambda → kmacros → normalize → debruijn
            → zinc-c → csexp → C VM
                                            ↑
                 interp.shen (meta-circular ZINC VM on Shen/Chez)
                        ↓
      serialize-reduced → globals.csexp → C VM (self-hosting)
```

| Layer | File | Description |
|---|---|---|
| **Shen→KLambda front-end** | `shen/shen->kl.shen` | Own full-arity Shen→KLambda compiler (safe subset, no partial application) |
| **Front-end helpers** | `shen/shen-kl-helpers.shen` | Pattern compiler, clause parser, body rewriter, `.shen` reader |
| **Normalizer** | `shen/normalize.shen` | KLambda expansion, A-normal form, debruijn indices |
| **ZINC compiler** | `shen/zinc.shen` | KLambda → ZINC bytecode (incl. `%%` primitive dispatch) |
| **Meta-circular VM** | `shen/interp.shen` | ZINC interpreter in Shen — loads, compiles, and runs |
| **Safe wrappers** | `shen/primitives.shen` | Type-checked wrappers for all primitives (incl. `safe.variable?`) |
| **OS helpers** | `shen/os-helpers.shen` | Safe-subset replacements for the OS list utilities the interp needs |
| **Eval/load** | `shen/toplevel.shen`, `shen/load.shen` | `interp-eval` / `interp-load` + raw s-expression parser |
| **Utilities** | `shen/util.shen` | `defun->lambda`, `primitive?`, `dedupe-globals` |
| **Native compiler** | `shen/compile.shen` | ZINC → canonical s-expression bytecode |
| **C VM** | `vm/zincvm.c` | Native parser + VM (~2600 lines, custom GC, all primitives, closures, tail calls) |
| **GC** | `vm/gc.c`, `vm/gc.h`, `vm/zinctypes.h` | Custom moving generational collector (2MB nursery + old-gen) |
| **Decompiler** | `vm/zincdec.c` | Standalone bytecode decompiler (4 formats + `--curried` scan) |
| **Serializer** | `shen/serialize-reduced.shen` | Compiles bundle `.shen` via `shen-load` into `globals.csexp` |

## Build & Run

```sh
git clone --recurse-submodules https://github.com/jmars/shen-meta.git
cd shen-meta
make setup    # clone shen-scheme if not already present
make          # build C VM + decompiler (uses cosmocc, Cosmopolitan)
make test     # run 34 built-in bytecode tests
make pipeline # compile (+ 1 2) through full pipeline
make bundle   # compile bundle .shen via shen->kl → globals.csexp
make run-bundle  # run C VM with self-hosting bundle
make gate     # test + test-debug + test-asan
```

Requires [shen-scheme](https://github.com/tizoc/shen-scheme) (Shen 41.2 on Chez Scheme) at `vendor/shen-scheme/` (used to bootstrap the serializer; the shipped bundle is compiled by our own `shen->kl`).

## Decompiler

Standalone binary for inspecting bundled bytecode:

```sh
./zincdec globals.csexp <function> [--raw|--asm|--shen|--csexp]
./zincdec globals.csexp --curried   # scan all closures for curried calls
```

| Flag | Format | Example |
|---|---|---|
| `--raw` (default) | Human-readable opcodes | `access 0`, `global +`, `apply` |
| `--asm` | Disassembly w/ addresses | `0003: jmpf 7  ; -> 0007` |
| `--shen` | Shen list for `interp.shen` | `[access 0]`, `[global +]`, `apply` |
| `--csexp` | Raw wire format (round-trippable) | `(ra[1:n]1P[1:s]+p)` |
| `--curried` | Flag curried partial-application calls (C VM can't run them); exit 1 if any | — |

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

The C VM loads `globals.csexp` — a ~0.33MB reduced self-contained bundle of **~295 closures** (340 compiled by the meta-interpreter at load) compiled by **our own `shen->kl`** from the type-safe `.shen` sources. There is **no Shen OS `.kl` code**: the six list/utility helpers the interpreter needs are re-implemented in `shen/os-helpers.shen` (`append`, `reverse`, `empty?`, `assoc`, `element?`) plus `safe.variable?` in `primitives.shen`. The full eval-kl chain is proven: native values marshalled to tagged forms, compiled through `extract-kl → kl→zinc → toplevel-interp`, and demarshalled back. Shen compiles Shen, which runs on Shen, on the C VM.

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

### Custom moving generational GC

A custom **moving generational collector** (`vm/gc.c`, `vm/gc.h`, shared types in `vm/zinctypes.h`). A 2MB nursery (pages marked `space==3`) is the allocation fast lane; the full-copy `collect()` is the (rare) old-gen collector and compacts old gen. Typed headers drive a tag-dispatch scavenger; roots are precise-only via the shadow stack + typed walkers (no conservative C-stack scan). A write barrier at `address->` vector writes keeps the old-gen→nursery references correct. No Boehm, no libgc.

See `docs/gc.md` and `docs/moving-gc-validation.md` for the design and validation.

### Calling C primitives past safe wrappers

`parse_bundle` overwrites C primitives with compiled safe wrapper closures. `%%` escapes and the inline `P[...]` opcode both dispatch via `OP_PRIM` → `exec_primitive`, bypassing the global table. There is no `raw.X` namespace — bytecode reaches an unchecked C primitive directly through `P[...]`.

### No Shen OS code in the bundle

The reduced bundle contains zero Shen OS `.kl` closures. Every bytecode closure is compiled by our own `shen->kl` (full-arity, no partial application). This also removes OS closures that shadowed C primitives in the global table (e.g. `variable?` was shadowed by a buggy `sys.kl` closure that returned `false`; it's now `safe.variable?` → the correct C primitive).

### Recursive eval-kl

`eval-kl` delegates to the bundled closure via `vm_exec_env` with a recursion guard. The bundled `safe.eval-kl` (`A → %% eval-kl A`) re-enters `exec_primitive`; the guard catches re-entry and returns identity. The full chain is: `marshal → extract-kl → kl→zinc → toplevel-interp → demarshal`. The metacircular `interp` function (97 pattern-match rules in `shen/interp.shen`) was updated to auto-push old accumulator values, matching the standard ZINC semantics of `zinc.shen`.

## Status

- [x] 34 built-in VM tests (arithmetic, types, closures, error handling, I/O, trap-error routing; 39 in `ZINCVM_DEBUG`)
- [x] ~295 bundled closures loaded and executing (reduced self-contained bundle; 340 compiled by the meta-interpreter)
- [x] **Self-hosting proven**: eval-kl chain returns `3` for `(+ 1 2)` — no C bypass
- [x] Custom moving generational GC (nursery + old-gen, precise roots, write barrier) — 18 nursery scavenge/retention tests pass
- [x] `deep_equal` for cons==cons structural comparison
- [x] `P[...]` inline OP_PRIM dispatch — bytecode calls C primitives directly
- [x] Recursive `eval-kl` delegating to bundled closure
- [x] Raw I/O from C VM
- [x] KLambda primitives: `gensym`, `@p`, `fst`, `snd`, `variable?`
- [x] Full read-compile-eval round-trip (bundled `load` works)
- [x] `read-from-string` via YACC parser
- [x] marshal_to_tagged / demarshal_from_tagged layer
- [x] Metacircular interp auto-push (matches standard ZINC semantics)
- [x] Standalone bytecode decompiler (`zincdec`) with 4 output formats + `--curried` scan
- [x] Per-closure instruction tracing (`--trace`)
- [x] String stream support in `open` primitive
- [x] **Own `shen->kl` compiler** — reduced bundle has no Shen OS `.kl` code
- [ ] REPL with interactive terminal I/O
- [ ] `shen.initialise` non-idempotency fix

## Credits

ZINC abstract machine: Xavier Leroy (INRIA).
Bartlett GC: James Marshall.
