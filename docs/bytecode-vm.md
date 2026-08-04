# ZINC bytecode & C VM conventions

Source: `AGENTS.md`. Opcodes, calling/argument conventions, primitive semantics,
and C VM gotchas.

## ZINC calling convention (STANDARD — fully aligned)

The VM uses **standard ZINC** semantics: all value-producing opcodes push results
to the stack AND set `acc`. The `zinc.shen` compiler no longer emits explicit
`u` (PUSH) instructions — it was modified to rely on auto-push.

**Opcodes that push to stack:**
- `OP_NUMBER`, `OP_STRING`, `OP_SYMBOL`, `OP_BOOLEAN` — push operand
- `OP_ACCESS` — push env lookup result
- `OP_GLOBAL` — push global table lookup result
- `OP_CUR` — push newly created closure
- `OP_PRIM` — push primitive result (after execution, no pre-push)
- `OP_APPLY` (VAL_PRIM) — push primitive result
- `OP_APPTERM` (VAL_PRIM) — push primitive result
- `OP_RETURN` — push return value to caller's stack
- `OP_PUSH` — kept for compatibility, duplicates acc to stack

**Opcodes that pop from stack:**
- `OP_JMPF` — pops condition from stack
- `OP_LET` — pops value from stack (binds to env)
- `OP_APPLY` / `OP_APPTERM` — pop function from stack top, then args up to mark

**Compiler changes:** `shen/zinc.shen` (`zinc-c` and `zinc-t`) removed all
`intersperse [push]` and explicit `[push]` emissions. Multi-arg primitives and
function calls now emit bare operand sequences, relying on auto-push.

**Metacircular interp — auto-push:** The `interp` in `interp.shen` implements
standard auto-push semantics. Only 7 value-instruction rules were changed — each
pushes the **old** accumulator to the stack before setting the new value:

```
[access N | C] A E S R    → (interp C (lookup N E) E [A | S] R)
[global G | C] A E S R    → (interp C (lookup-global G) E [A | S] R)
[cur C1 | C] A E S R      → (interp C [lambda C1 E] E [A | S] R)
[number N | C] A E S R    → (interp C [number N] E [A | S] R)
[string Ss | C] A E S R   → (interp C [string Ss] E [A | S] R)
[symbol Ss | C] A E S R   → (interp C [symbol Ss] E [A | S] R)
[boolean B | C] A E S R   → (interp C [boolean B] E [A | S] R)
```

All 85 other rules remain unchanged. Binary prim rules like
`[prim + | C] [number A] E [[number A1] | S] R` work because auto-push leaves the
previous value on the stack top, which is exactly the rightmost argument.

**Key design choice:** Pushing OLD accumulator (not new value) means the
accumulator remains the "current value" at every step, keeping all existing prim
rules compatible.

**The C bridge (push insertion in eval_kl) has been REMOVED.** No bytecode
transformation is needed — the interp natively handles standard ZINC output.

## ZINC argument convention

- **ZINC evaluates args RIGHT-TO-LEFT**: rightmost Shen arg pushed first (ends at
  stack bottom), leftmost pushed last (on top of stack).
- All two-arg C primitives pop `a1` (top = leftmost arg) then `a2` (below =
  rightmost arg). E.g. for `(- 5 3)`: stack `[3, 5]`, pop a1=5, a2=3, compute
  `a1 - a2` = 5-3 = 2. `cons` does `val_cons(a1, a2)` = cons(left, right).
- `open` was the exception — had `dir`/`path` swapped, causing "open bad types"
  in bundled `load`. Fixed: pop `path` first, then `dir`.
- **When writing bytecode by hand**, push args in right-to-left order:
  `(s[2:s]in u S[8:S]Makefile u m g[8:s]raw.open p)` for `(open "Makefile" in)`.
- **CRITICAL**: Hand-written bytecode MUST use RTL order. The VM pops top-first
  (leftmost arg). Writing LTR works for commutative ops (+, =, cons-as-pair) but
  silently produces wrong results for non-commutative ops (-, /, trap-error,
  write-byte). This is the #1 recurring bug pattern. See tests 27-32.

## csexp atoms & flat bytecode

- csexp atoms: `[len:type]value` — type is `s`/`n`/`S`/`b`.
- Opcodes are single chars: `m` pushmark, `p` apply, `u` push, `r` grab, `v`
  return, etc.
- ZINC bytecode for the interp family is FLAT: opcodes and operands are separate
  list elements. `[number 42]` = `cons('number, cons(42, nil))`, NOT
  `cons(cons('number, cons(42, nil)), nil)`.

## Primitive semantics (critical — must match Shen)

- **`=`** supports deep structural equality for cons cells via `deep_equal()`
  (depth-limited to 1000 for cycle safety). Without this, `(= [+ 1 2] [+ 1 2])`
  returns false, breaking `macroexpand-h`'s fixed-point check.
- **`=` symbol comparison** is strict `strcmp` — no prefix awareness. Reference
  shen-scheme's `kl:=` uses plain `eq?` (pointer identity on symbols); `foo` and
  `shen.foo` are different symbols. The C VM MUST NOT add `shen.` prefix handling
  to `=` — prefix consistency is a pipeline concern (see loading doc). If
  `(= define shen.define)` ever returns false at runtime, the bug is in
  `shen.initialise` not completing, NOT in `=`.
- **`=` cons-vs-symbol** and symbol-vs-cons comparisons always return false.
  `zinc-c` generates correct `hd`-wrapped comparisons, so flat
  `(= [define ...] define)` no longer occurs.
- **`%%` in normalize**: ALL primitives must use `%%` prefix in normalize.shen.
  `[set S E]` was missing `%%`, causing `set` to go through `global set` + `apply`
  (safe wrapper) instead of `prim set`. Always use `[%% set S T]`.
- `n->string N`: number → single-character string via ASCII code.
  `(n->string 40)` → `"("`.
- `string->n S`: first character → ASCII code. `(string->n "(")` → `40`.
- `pos S N`: single character at index N (0-based). OOB → `""`. `(pos "hello" 1)`
  → `"e"`.
- `str V`: value→printed string. Numbers use decimal, symbols use name, strings
  pass through.
- `open Path Dir`: file I/O + string streams. ENOENT on `in` → creates string
  stream from Path. String stream data stored externally (not in Value union) to
  keep sizeof(Value) small.

## C VM conventions

- **GC**: Boehm GC (libgc) — non-moving conservative collector. `GC_MALLOC`/
  `GC_MALLOC_ATOMIC` via macros `GC_VALUE()`, `GC_STR()`, `GC_VALUE_ARRAY()`.
  No gcinit, no extra roots, no pointer counts. Objects never move, so
  stack-local Value pointers are always safe across allocations. (See `docs/gc.md`
  for the plan to replace this with a moving generational collector.)
- The old Bartlett copying GC is archived at `vendor/bartlett-gc` branch
  `bartlett-mostly-copying` (pinned-page implementation) — kept for
  reference/experimentation.
- `global` loads from table then falls back to `val_prim(name)`.
- Primitives dispatch via `exec_primitive()` — apply-mode pops mark + args from
  stack. Inline `OP_PRIM` (`P`) executes primitive with args from stack +
  accumulator (ZINC semantics).
- `trap-error`/`simple-error` use `setjmp`/`longjmp` (see error-handling doc).
- `eval_kl_depth` recursion guard: setjmp guard ensures depth always decremented
  even on longjmp from simple-error.
- `--trace <name>`: trace every instruction of a closure as it executes.
  Repeatable. Output in raw format with PC numbers (e.g. `[+] 0000 grab`).
  Traces only the named function, not functions it calls (unless also --traced).

## `apply` / `appterm` stack layout

`apply` ('p') and `appterm` ('t') share identical stack layout:
`[mark, argN..arg1, function]`. Difference: appterm reuses current frame
(tail-call, pc=0), apply pushes new CallFrame. Both reject >64 args. Appterm
additionally rejects zero args and requires pushmark.

## Pipeline gotchas (bytecode-related)

- `%%` compiles to `[prim X]` in ZINC; normalize must flatten curried `%%` calls
  via `flatten-%%app` or you get spurious `apply` after `prim` instructions.
- `instr-count` and `label-positions` must handle opcodes with operands
  explicitly (`access _`, `global _`, `jmpf _`, `jmp _`, `number _`, `string _`,
  `symbol _`, `boolean _`, `prim _`) — catch-all `[_ | C]` counts operand atoms
  as separate instructions, inflating jump targets.
- `cur` is 1 instruction in csexp stream, not `1 + body_size`.
- `parse_bundle` must unwrap `OP_CUR` to get closure body — the `c(...)` wrapper
  is a single instruction whose operand is the closure's code array.
- `ps` returns KLambda; unary primitives like `number?` lack `%%` wrapping in
  Shen 41.2 — normalize/debruijn need to handle bare primitives for inline `prim`.
- `marshal_to_tagged` must NOT recursively tag VAL_CONS car/cdr. extract-kl
  handles its own recursion on `[cons X Y]`. Recursive marshalling creates
  impossibly deep nesting.
- `global` keyword registration: ZINC pattern keywords (number, symbol, cons,
  lambda, etc.) must be forced into the global table as symbols after
  parse_bundle, or self-compiled pattern-matching code resolves them as closures
  instead of tag symbols.
