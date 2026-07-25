# shen-meta

A meta-circular Shen ZINC abstract machine — Shen evaluating Shen, compiling itself to native bytecode.

## Architecture

```
Shen source → KLambda → normalize → debruijn → ZINC bytecode → csexp → C VM
```

| Layer | File | Description |
|---|---|---|
| **Normalizer** | `normalize.shen` | KLambda expansion, A-normal form, debruijn indices |
| **ZINC compiler** | `zinc.shen` | KLambda → ZINC bytecode (37 primitives) |
| **Shen VM** | `interp.shen` | Meta-circular ZINC interpreter (Shen 41.2 on Chez Scheme) |
| **Safe wrappers** | `primitives.shen` | Type-checked wrappers for all 37 primitives |
| **Native compiler** | `compile.shen` | ZINC → canonical s-expression bytecode |
| **C VM** | `zincvm.c` | Native parser + VM (all 37 primitives, closures, tail calls) |
| **Serializer** | `serialize.shen` | Compiles safe wrappers to csexp bundle for native VM |

## Build & Run

```sh
make vm          # build C VM
make test        # run built-in tests (28 tests)
make bundle      # compile all safe wrappers → globals.csexp
make run-bundle  # load serialized globals + run (+ 1 2)
make pipeline    # compile (+ 1 2) through full Shen→csexp pipeline
make interp      # run meta-circular interpreter on Chez Scheme
```

Requires [shen-scheme](https://github.com/tizoc/shen-scheme) (Shen 41.2 on Chez Scheme).

## Bytecode format

Canonical s-expressions with `[len:type]value` atoms:

- `s` — symbol, `n` — number, `S` — string, `b` — boolean
- Single-char opcodes: `m` pushmark, `p` apply, `u` push, `r` grab, `v` return, `c` cur, `g` global, `a` access, `f` jmpf, `j` jmp, `e` let, `d` endlet, `t` appterm, `n` number, `S` string, `s` symbol, `b` boolean, `P` prim

Example: `(+ 1 2)` → `(mn[1:n]2un[1:n]1ug[1:s]+p)`

## Status

- [x] 37 primitives in C VM (arithmetic, comparison, types, strings, I/O, error handling)
- [x] Closures, tail calls, `trap-error`/`simple-error`
- [x] Full Shen→csexp→C VM pipeline
- [x] Serialized global table (38 closures) loaded by native VM
- [ ] Calling serialized safe wrappers from native VM (known `P+p` issue in zinc-c)
- [ ] GC integration (bartlett-gc ready)
- [ ] Full Shen OS serialization

## Credits

ZINC abstract machine: Xavier Leroy (INRIA).  
Original Shen 22.2 interpreter and native compiler by the repo author.  
Shen 41.2 port, C VM, and pipeline by the repo author.
