# Docs

Documentation for the shen-meta project. Derived from `AGENTS.md`, `bugs.md`,
and the GC design work.

## Index

- **`architecture.md`** — pipeline, key files, design intent (static vs dynamic
  call sites, guard-free release VM), the reduced self-contained bundle, partial
  application, self-hosting tests.
- **`compiler.md`** — the `shen->kl` compiler front-end: supported subset,
  constraints & limitations (non-linear patterns, reader split, out-of-scope
  constructs), and improvement roadmap.
- **`bytecode-vm.md`** — ZINC calling/argument conventions, opcodes, csexp atoms,
  primitive semantics, C VM conventions, `apply`/`appterm` layout, pipeline
  gotchas.
- **`error-handling.md`** — the CatchFrame trap-error design, defense-in-depth
  type checks, historical stale-jmp bug, remaining known limitations.
- **`loading.md`** — eval/load, serialization, module system & package
  prefixing, the raw s-expression parser, KLambda primitives, the `zincdec`
  bytecode decompiler, and the REPL.
- **`shen-pitfalls.md`** — Shen language quirks, pitfalls, and self-hosting/C VM
  gotchas.
- **`bugs.md`** — bugs and known issues (the raw bugs file, moved here).
- **`gc.md`** — the approved plan for replacing Boehm with a moving generational
  collector.
- **`moving-gc-validation.md`** — the advisor validation of the GC plan
  (hazards, full line references).

## Source of truth

`AGENTS.md` at the repo root is the operative agent-instruction file and the
canonical source these docs are derived from. If content drifts, reconcile
against `AGENTS.md`.
