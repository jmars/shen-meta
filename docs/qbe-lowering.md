# QBE-backed native codegen with a type-preservation proof

Status: **design / decision record** (no code landed).
Date: 2026-08-06.

## 0. TL;DR

Add a sibling of `nat->csexp` that lowers resolved ZINC bytecode to **QBE** IR
(Quentin Carbonneaux's small typed SSA backend, x86_64/aarch64) instead of raw C,
then prove in **Shen's own sequent calculus** that well-typed ZINC lowers to
well-typed QBE. QBE + the C runtime remain **trusted** (not proven). The C VM
(`vm_exec_env`) stays the fast production runtime; the QBE path is a **proven
reference** differential-tested against it.

```
Shen source ──(Stage 1: prove type-safe via Shen sequent-calculus)──> ZINC bytecode (typed IR #1, zinc-code)
ZINC ──(PROOF TARGET: stack-to-SSA lowering)──> QBE IR (typed IR #2, qbe-ir)
QBE ──(trusted small backend)──> native x86_64/aarch64
link with trusted C runtime (gc.c, exec_primitive, safe wrappers) ──> executable
```

---

## 1. Session decision trail (why this direction)

This design was chosen through a recorded chain of decisions:

1. **Soundness gap (explored 2026-08-06).** The AGENTS.md claim that the
   meta-circular interpreter is "type-safe by construction" is *partially real
   but largely aspirational*:
   - `klambda` datatype (`shen/types.shen:19-42`) is **degenerate** — the first
     rule has NO premise (`X : klambda` for all X), so `tc +` proves nothing
     about well-formedness of `zinc-c`/`zinc-t`.
   - 44 safe wrappers in `shen/primitives.shen` have **zero** `{ }` signatures
     (AGENTS.md says "37 type-checked" — stale count, and they are runtime-guarded
     via `where` + `simple-error`, not typed).
   - The `.kl` base is loaded **unchecked** via `interp-load-raw`
     (`shen/load.shen:186-189`).
   - No proof-assistant formalization exists anywhere in the repo.

2. **Codegen vs proof.** "Self-hosted alone" is table-stakes (decades-old Lisp
   bootstrap trick). The differentiator is the *soundness story*: proving the
   interpreter type-safe. Shen→C codegen was proposed; clarified that codegen is
   **compilation, not a proof** — it inherits whatever soundness the source has.

3. **Model the output in sequent calculus.** Yes — but only if the *output* is a
   **typed target IR you control**, and "C" is demoted to a trusted mechanical
   lowering. Two distinct guarantees:
   - **Type preservation** (well-typed input → well-typed output): a typing
     judgement, expressible and checkable in Shen's sequent calculus.
   - **Semantic/behavioral equivalence** (output behaves like input): needs a
     semantics of both sides + simulation proof — a general theorem-proving task,
     not type-checking. Much harder; out of near-term scope.

4. **QBE as the target IR (chosen).** QBE is a small, *typed*, SSA IR with a
   built-in x86_64/aarch64 code generator — designed exactly for "I have front-end
   work and don't want to write a register allocator." Compared to raw C:
   - Fixed, small typed instruction set → feasible to model with sequent-calculus
     typing rules (unlike C's huge/UB semantics).
   - No hand-written asm generation → trust a ~3kLOC auditable BSD backend.
   - Trusted surface shrinks from "a whole C toolchain" to "a small backend."
   - Honest caveat: QBE is itself unproven C — we prove **down to the QBE IR**,
     then trust the backend. The C runtime (GC, primitives) stays trusted glue.

---

## 2. Code grounding (verified against the tree)

- **Opcodes** (`vm/zinctypes.h:65-72`): `a` access, `g` global, `f` jmpf, `j` jmp,
  `t` appterm, `p` apply, `m` pushmark, `c` cur, `r` grab, `v` return, `e` let,
  `d` endlet, `n` number, `S` string, `s` symbol, `b` boolean, `P` prim.
  **No `label` opcode** — `compile-zinc` (`shen/compile.shen:44,64-66`) strips
  `[label …]` and rewrites `jmpf`/`jmp` to absolute numeric targets.
- **Auto-push invariant** (`vm/zincvm.c:1524-1527,1640-1648,1663-1667`): every
  value-producing op does `acc = operand; va_push(&stack, acc)`. So after any
  value op **`acc` is the stack top** (same value). This is the key fact that
  makes the stack→SSA map clean (no separate accumulator register needed).
- **Env / de Bruijn** (`vm/zincvm.c:1440-1448`): `lookup_env(n) = env[env_len-1-n]`
  — index 0 is the most-recently-pushed binding. `apply` builds
  `env = lambda.env ++ args` (`zincvm.c:1591-1603`).
- **Calling convention** (`zincvm.c:1559-1624`): `apply` pops function from stack
  top, collects args up to the mark, saves `CallFrame{code,pc+1,env,stack}` on the
  C call stack (`frame_stack`, depth 65536, `zinctypes.h:86`), sets
  `env = lambda_env ++ args`, `pc=0`. `appterm` (`:1669-1700`) identical but
  reuses the frame (tail call). `return` (`:1626-1638`) pops frame and pushes acc
  to caller stack. `grab` (`:1542-1557`) binds a stack value to env, or — when a
  mark is on top — pops frame and returns (partial application path).
- **RTL arg order** (`shen/zinc.shen:43-49`): for `[F A B]`, `zinc-c` emits
  `pushmark, codeB, codeA, F, apply` so leftmost arg ends up on stack top. Prims
  pop `a1=top=leftmost`, `a2=below=rightmost`, compute `a1 OP a2`
  (`zincvm.c:670-689`) — e.g. `-` yields `left - right`.
- **GC interface** (`vm/gc.h`): `gc_alloc`, `gc_alloc_atomic`, `GC_VALUE_ARRAY(n)`
  (`zincvm.h:17-19`), the precise-root shadow-stack API (`gc_root_push_ptr`,
  `gc_root_push_value`, etc.). Typed headers + tag-dispatch scavenger; precise
  roots are the sole root source (no conservative C-stack scan since 4a.6).
  Write barrier only on `address->` (`gc.h:83-88`).
- **Value constructors** (`vm/zincvm.h:79-86`, `zincvm.c:130-183`): `val_number`,
  `val_string`, `val_symbol`, `val_boolean`, `val_cons`, `val_nil`,
  `val_lambda(code,code_len,env,env_len)` (GC-copies env), `val_vector`. These are
  the *only* allocation paths a generated native function may use.
- **Primitives** (`vm/zincvm.c:603+`, `exec_primitive`): single C dispatch, pops
  from `ValueArray *stack`, writes `*acc`. Called from `OP_PRIM` (inline `P`) and
  `OP_APPLY`/`OP_APPTERM` when callee is `VAL_PRIM`. The trusted primitive boundary.
- **Error model** (`vm/zincvm.h:55-60`, `zincvm.c:358-366`): per-catch-site linked
  list of stack-allocated `CatchFrame`s; `vm_throw` writes `error_val` and
  `longjmp`s to chain head. `trap-error`/`simple-error`/`apply-non-callable`/
  `env_pop`/`eval-kl` throw sites stay always-on; primitive type guards are
  `PRIM_TYPE_ERROR`, **compiled out in release** (`zincvm.c:385-393`).
- **Typing today** (`shen/types.shen`): `zinc-value` (`:44-118`) and `zinc-code`
  (`:124-208`) are real structural datatypes. `klambda` (`:19-42`) is **degenerate**.
  `primitive?` (`:1-7`) is the single source of truth; `bytecode?` (`:120-122`)
  enumerates the no-operand ops.
- **Pipeline seam** (`shen/compile.shen:64-66,117`): `zinc->native = nat->csexp ∘
  compile-zinc`. The QBE emitter slots in as a sibling of `nat->csexp` over the
  **same resolved `klambda`** that `nat->csexp` consumes.
- **Bundle** (`shen/serialize.shen`, `shen/serialize-reduced.shen`): walks
  `global-table`, emits `(name c(cur-code…))` per closure via `zinc->native`. The
  reduced bundle is the type-safe subset; the full OS bundle is type-unsafe.

---

## 3. Scope & goal

**What "proven" means.** A *type-preservation theorem* in Shen's sequent calculus:

> If `C : zinc-code` is well-typed (against a non-degenerate `zinc-code` effect
> typing carrying a stack-effect type), then `lower(C) : qbe-ir` is well-typed
> (against a `qbe-ir` typing carrying QBE `w`/`l`/block SSA typing).

This is **not** full semantic equivalence between ZINC stack machine and QBE SSA,
and **not** a proof of QBE itself. QBE is trusted. Behavioral equivalence is
established **empirically** by differential testing against `vm_exec_env` (§6).

**Why type-preservation is the right target.** A well-typed QBE program cannot
miscompile into a memory-unsafe native binary by QBE's own type system — QBE
refuses ill-typed IL. If the lowering preserves the ZINC type, and the ZINC type
encodes "no value of the wrong tag ever reaches a primitive that would deref it"
(what `zinc-value` + `primitive?` give us), then the emitted native code inherits
the tag-safety the C VM guards out in release. Today release `vm_exec_env` runs
with no tag check, relying on the *unproven* assertion that the bundle is
type-safe. After this work, the QBE path makes that assertion a theorem for the
subset it compiles.

**Deliverables.**
1. `shen/qbe.shen` — `lower : zinc-code --> qbe-ir` (type-annotated, loaded under
   `tc +` once Stage 1 fixes land).
2. `shen/qbe-types.shen` — `qbe-ir` datatype, `qbe-typed` judgement, preservation
   theorem.
3. `shen/zinc-effects.shen` — non-degenerate `zinc-code` effect typing (stack
   effect + tag env), replacing the degenerate `klambda` rule for the lowering's
   premise.
4. `vm/qbe_emit.c` / `qberun` — thin host: parse `.qbe`, run `qbe` assembler, link
   `.o` against trusted runtime.
5. Makefile targets (`make qbe`, `make diff-test`) + differential harness.
6. This doc records the subset, proof sketch, and open obligations.

**Out of scope (deferred):** proof of phi correctness under joins with the C stack's
conservative GC roots; proof of `trap-error` longjmp ABI; floats (QBE `s`/`d`);
the `absvector`/`address->` memory model beyond typed `load`/`store`; partial
application (already absent from the C VM subset).

---

## 4. The ZINC → QBE lowering (stack-to-SSA)

### 4.1 Lowering state

Symbolic execution of the resolved instruction list (`compile-zinc` output):

| Lowering state | ZINC analogue | QBE rendering |
|---|---|---|
| `Stk : list temp` — compile-time stack; **top = last** | `ValueArray stack` + `acc` | each temp a `%tN` |
| `Env : list temp` — de Bruijn env, index 0 = newest | `env[]` | activation record (§4.4) |
| `Phi : block ↦ (Stk, Env, pc)` — join state | implicit in `pc` | `@phi` at block heads |

Because `acc == stack top` after every value op, we do not model a separate
accumulator register; `Stk`'s top *is* the accumulator. A value op pushes a fresh
`%t`; a consumer pops. (Standard "ZINC stack as stack of SSA temporaries"
transformation — Leroy's Zinc→Caml used the same idea; the C VM's `acc` is an
optimisation SSA makes redundant.)

**Enabling fact for `apply`/`appterm`:** they collect args "up to the mark" with a
*dynamic* count (`zincvm.c:1568-1573`). Per AGENTS.md the reduced bundle
guarantees every call site is full-arity, so the lowering can statically count
the args between a `pushmark` and its matching `apply`/`appterm`. Stage 4 makes
this a *checked* precondition via the `apply` effect rule.

### 4.2 CFG recovery

Block leaders: index 0; every `jmpf`/`jmp` target; the instruction after any
`jmp`/`return`/`appterm` (terminators). Map each leader to a QBE block `@b<index>`.
`jmpf L` → `jnz %cond, @b<L>, @b<pc+1>`. `jmp L` → `jmp @b<L>`. `phi` nodes at each
block head with ≥2 predecessors for every `Stk`/`Env` slot that differs across
predecessors. Since ZINC jumps only between statically-known positions and `Stk`
is compile-time, the lowering knows exact stack depth at each leader — only the
*temp name* feeding each slot varies, which is exactly what `phi` expresses.

### 4.3 Per-opcode lowering rules

Notation: `Stk=[…, t_{n-1}]` (top = `t_{n-1}`), `Env=[e_0,…,e_{m-1}]` (head =
newest). `fresh()` → new `%tN`. `emit(s)` appends to the current block.

**`number N | C`, `string S | C`, `symbol S | C`, `boolean B | C`**
(value producers; `zincvm.c:1524-1527`):
```
t = fresh(); emit("%t =l call $val_number(l <N>)");   // string/symbol/boolean analogous
Stk' = Stk ++ [t]
```
We go through the trusted constructors (not raw bit patterns) so GC headers are
correct.

**`access N | C`** (`zincvm.c:1640-1643`): `acc = lookup_env(N, env)`.
```
t = fresh(); emit("%t =l copy %e_N");   // Env[N], de Bruijn, 0 = newest
Stk' = Stk ++ [t]
```

**`global G | C`** (`zincvm.c:1644-1648`): `acc = global_get(G)`.
```
t = fresh(); emit("%t =l call $global_get(l $str_G)");
Stk' = Stk ++ [t]
```
Dynamic call site — **not** proven type-safe — so when `G` is a primitive name it
must go through `safe.<G>` (`shen/primitives.shen`), matching the
`[global X] → safe.X` convention (AGENTS.md). The lowering knows `primitive?`
(`shen/types.shen:1-7`) and emits `global_get("safe.<G>")` in that case.
Non-primitive globals are user closures whose well-typedness comes from the
bundle's own typing (Stage 1).

**`prim F | C`** (`zincvm.c:1528-1540`, `exec_primitive :603+`):
```
arity_F = arity(F)
(a1,…,a_k) = pop k from Stk   // a1 = top = leftmost (RTL)
t = fresh()
emit("%t =l call $prim_<F>(l %a1, …, l %a_k)")
Stk' = Stk ++ [t]
```
Recommended: the `prim_<F>` C shim takes args already in `a1=leftmost … a_k=rightmost`
order and computes directly, *bypassing* `exec_primitive`'s stack-popping protocol
— a plain C call with a documented ABI, far easier to reason about in the proof.
The shim still routes type-error cases through `vm_throw` for the dynamic path.

**`pushmark | C`** (`zincvm.c:1541`): do **not** push a real `VAL_MARK` sentinel;
record current `Stk` depth in `Marks : list int`. Works because all call sites are
full-arity — no runtime scan needed.
```
Marks' = [length(Stk) | Marks]
```

**`apply | C`** (`zincvm.c:1559-1624`):
```
fn = pop Stk; mark = head Marks; k = length(Stk)-mark; args = pop k; Marks' = tail Marks
t = fresh()
emit("%t =l call @clo_<fn>(l %env_capture, l %arg_0, …, l %arg_{k-1})")
Stk' = Stk ++ [t]   // continuation: push return value to caller stack
```
Static sub-case: if `fn` is a known `cur` literal just pushed by `OP_CUR`, `@clo_<fn>`
is the statically-known QBE function and `%env_capture` is the captured env temp.
Dynamic sub-case (callee from `global`/`access`, higher-order): a uniform trampoline
`@clo_call(l %fn, l %env, …)` — **excluded from the preservation theorem** and
routed through `safe.apply`-style wrappers. The effect type enforces "callee is a
known closure literal."

**`appterm | C`** (`zincvm.c:1669-1700`): same arg collection, tail call — no
continuation push. Emit a normal `call` whose result is immediately `ret`-ed (QBE
lowers it as a tail call). The current frame's `Stk`/`Env` are discarded (matching
C VM's frame reuse). *Cleaner in SSA than the C VM* — "the continuation is the
caller's continuation", captured naturally by the call graph, no stack surgery.

**`cur C1 | C`** (`zincvm.c:1663-1667`): create a closure capturing current env.
```
t = fresh()
emit("%t =l call $val_lambda(l $code_<C1>, l <code_len_C1>, l %env_ptr, l %env_len)")
Stk' = Stk ++ [t]
```
`$code_<C1>` is a QBE data symbol pointing at `@clo_<C1>`'s trampoline. `%env_ptr`
is the address of the current activation record's env slots. `val_lambda` GC-copies
the env, so the activation record can be freed after the closure returns. The
lowering records that `C1` is the callee if the next instruction is `apply`/`appterm`
with this temp as function (enabling the static-callee optimisation).

**`grab | C`** (`zincvm.c:1542-1557`): in the proven full-arity subset, `grab` is
only the parameter-binding **prologue** of a closure — never emitted mid-block. It
is the function *entry signature*:
```
@clo_F(l %env_cap, l %a0, …, l %a_{k-1}) {
@entry
  %env_full =l call $bind_args(l %env_cap, l %a0, …, l %a_{k-1})
  …body with Env = de Bruijn view over %env_full…
}
```
The "grab sees a mark → return" path is partial application, excluded. The effect
type *rejects* any non-prologue `grab`, making this enforced not conventional.

**`return | C`** (`zincvm.c:1626-1638`): `ret %t_top` (Stk top = acc). Closes block.

**`let | C`** (`zincvm.c:1650-1653`): pop stack, push to env — pure compile-time
list manipulation, no QBE instruction (`Env' = [v | Env]`).

**`endlet | C`** (`zincvm.c:1655`): `Env' = tail Env`. Pure compile-time; in the
activation-record model this just moves the env-depth watermark.

**`jmp L | C`**: `emit("jmp @b<L>")`; close block; carry `(Stk, Env)` to `@b<L>`
via `Phi`.

**`jmpf L | C`** (`zincvm.c:1657-1661`): pop cond, branch.
```
cond = pop Stk
emit("%c =w call $is_false(l %cond)"); emit("jnz %c, @b<L>, @b<pc+1>")
```
Both successors receive `(Stk', Env')` (after the pop). `$is_false` =
`cond.tag == VAL_BOOLEAN && !cond.boolean`.

### 4.4 Env representation in QBE

Compile-time de Bruijn list `Env : list temp`; runtime = fixed-size activation
record per closure invocation:

- Each `@clo_F` has max env depth `D_F` = captures + max nested `let`-depth
  (computable via a pre-pass analogous to `instr-count`, `shen/compile.shen:4-17`).
- Allocate `%env =l alloc8 D_F*8` (or a stack slot); write captures into slots
  `[0..c)` and `let`-bound values into `[c..)` as it descends.
- `access N` reads `env[D_F-1-N]` — exactly `lookup_env`'s formula
  (`zincvm.c:1440-1448`).
- `cur C1` passes `%env` (+ current depth) to `val_lambda`, which GC-copies the
  live prefix so a returned closure outlives its frame (`zincvm.c:177-181`).

So `access` = `load` from a known slot, `let` = `store`, `endlet` = watermark
decrement. All compile-time-trackable, all typeable as `l` (pointer-to-Value).

### 4.5 Trusted runtime ABI

| Generated call | C function | Source |
|---|---|---|
| `val_number(n)` | `val_number` | `zincvm.h:79` |
| `val_string(data,len)` | `val_string` | `:80` |
| `val_symbol(name)` | `val_symbol` | `:81` |
| `val_boolean(b)` | `val_boolean` | `:82` |
| `val_cons(a,b)` | `val_cons` | `:83` |
| `val_lambda(code,len,env,env_len)` | `val_lambda` | `:85` |
| `global_get(name)` | `global_get` | `:114` |
| `prim_<F>(args…)` | new shims around `exec_primitive` | `zincvm.c:603` |
| `bind_args(env_cap,args…)` | new helper around `GC_VALUE_ARRAY` | `zincvm.h:19` |

Closure call dispatch: each `@clo_F` has a companion trampoline `@tramp_F(l %fn)`
that ignores `%fn` and tail-calls `@clo_F` with captured env baked in — *only for
statically-known callees*. Dynamic case uses a generic `@apply` trampoline (reads
`fn.lambda.code`, jumps); **not in the proven subset**.

`trap-error` (`interp.shen:175`, C `vm_catch_chain`) maps to a C helper
`$trap_error(l %body_thunk, l %handler_thunk)` that manages the `CatchFrame` and
returns the result `Value`. Both thunks are `@clo` functions. This keeps
longjmp/setjmp entirely inside trusted C, never in generated QBE (QBE doesn't model
`setjmp`).

### 4.6 Fallback for the full (type-unsafe) bundle

If `globals-full.csexp` is ever lowered, static-arity breaks (partial application,
dynamic `apply`). Fallback keeps a real `Stk` as a `Value*` array (via
`GC_VALUE_ARRAY`), pushes real marks, and `apply`/`appterm` call a generic
`@apply_dynamic` scanning to the mark — C-VM-equivalent code in QBE, **not covered
by the proof**, exists only as a differential-test target. Recommend *not* building
this initially (§8).

---

## 5. The QBE subset to model

**Types:** `w` (32-bit; tags, booleans, small ints), `l` (64-bit; `Value` and
pointers).

**Instructions:** `lit w/l`, `add w`, `sub w` (tag juggling only), `call`,
`tail call`, `ret`, `jnz`, `jmp`, `phi`, `loadl`, `storel` (env slots only),
`alloc8` (env activation record).

**Excluded:** `s`/`d` (floats), `csel`/`cztr`/`vaarg`, `blit`, `loop`, `memset`,
function pointers (other than opaque `l` from `val_lambda`), variable-arity calls,
general memory beyond env slots.

**Justification:** the proof obligation is "every emitted QBE instruction is
well-typed." With floats/variadics/general-memory excluded, the `qbe-ir` typing
judgement has ~10 instruction cases, each trivial (call: args match prototype;
phi: all inputs agree; load/store: addr `l`, value `l`; jnz: cond `w`). Small
enough to prove in Shen's sequent calculus. Cost: all real work (cons, string,
primitive dispatch, GC) happens inside trusted C calls — which is the existing
architecture (`exec_primitive`, `val_*`).

### 5.1 The `Value`-as-`l` question

`Value` is a 24-byte tagged union (`zincvm.c:33-59`). Two options:
- **(A) Pointer-to-Value everywhere.** Every temp is `l` holding a `Value*`. Simple
  but every `val_number` heap-allocates — big slowdown.
- **(B) Value-by-value via two-word struct.** Model `Value` as a pair of `l`
  (tag, payload); QBE's ABI lowers small aggregates. Matches the C VM's by-value
  calling convention (`zincvm.h:79-86`), keeps the proof's call rule simple.

**Recommendation: (B).** The `qbe-ir` judgement treats every `Value` as opaque `l`;
the *tag* is tracked in the ZINC type, not QBE.

---

## 6. The type-preservation proof design

### 6.1 ZINC side: non-degenerate effect typing

`zinc-code` (`shen/types.shen:124-208`) is structural but tag-agnostic. Add a
stronger premise tracking (a) stack effect `[σ_in ⇒ σ_out]` (list of value tags)
and (b) a tag environment. In `shen/zinc-effects.shen`:

```
(datatype zinc-typed
  Pc : (list zinc-code); E : (list zinc-tag); Σ : (list zinc-tag);
  ___
  (typed-step Pc E Σ) : (list zinc-tag);
  …one rule per opcode…)
```

Example rules (mirror §4.3):
- `[number N | C]`: `(typed-step [number N|C] E Σ) = (typed-step C E (Σ ++ [number]))`.
- `[prim + | C]`: requires `Σ = Σ' ++ [number, number]`;
  result `(typed-step C E (Σ' ++ [number]))`.
- `[access N | C]`: requires `N < length E`, `T = nth E N`;
  result `(typed-step C E (Σ ++ [T]))`.
- `[cur C1 | C]`: requires `(typed-closure C1 E_closure)` derivable; pushes `lambda`.
- `[apply | C]` (static): requires `Σ = Σ' ++ [lambda] ++ arg_tags` and the top
  `lambda` is a known `cur C1` with `typed-closure C1 (E ++ arg_tags)`. **This is
  the arity check** the C VM skips in release (`zincvm.c:393`); the effect type
  makes it a compile-time obligation.
- `[jmpf L | C]`: requires `Σ = Σ' ++ [boolean]`; both branches type to the same
  `Σ'`-plus-continuation — where the join/`phi` discipline enters.

The degenerate `klambda` rule is **replaced** by this; `zinc-c`/`zinc-t` signatures
(`shen/zinc.shen:2,6,31`) are re-stated as `klambda-typed --> (list zinc-code)`.
`zinc-value`/`zinc-code` stay as structural substrate; `zinc-typed` is a *judgement*
over them (as `primitive` is over `symbol`).

### 6.2 QBE side: `qbe-ir` datatype

```
(datatype qbe-ir
  ___  w : qtype;
  ___  l : qtype;
  T : qtype; N : number;  ___  (lit T N) : qinstr;
  Args : (list qtemp); Proto : qproto;
  ___  (call Name Args) : qinstr;
  ___  (tailcall Name Args) : qinstr;
  T : qtype; V : qtemp;  ___  (ret V) : qterminator;
  C : qtemp; L1 L2 : qlabel;  ___  (jnz C L1 L2) : qterminator;
  L : qlabel;  ___  (jmp L) : qterminator;
  In : (list (qlabel qtemp)); T : qtype;  ___  (phi T In) : qinstr;
  P : qtemp;  ___  (loadl P) : qinstr;
  V P : qtemp;  ___  (storel V P) : qinstr;
)
```

Well-typed QBE = judgement `(qbe-typed Block) : qblock-typed` giving each block a
`(qtype, stack-depth)` typing — QBE's own internal type-check. Trusted lemma: QBE
rejects IL where a `phi`'s inputs disagree, a `call` arg type mismatches the
prototype, or a `load`/`store` address isn't `l`.

### 6.3 The preservation theorem

```
(define qbe-preservation
  { zinc-code --> (list zinc-tag) --> (list zinc-tag) --> qbe-ir }
  C E Σ_in -> (lower-typed C E Σ_in) where (typed-step C E Σ_in) : (list zinc-tag))
```

Shen sequent, per opcode:
```
 typed-step [op | C] E Σ = Σ'
 --------------------------------- (lower-pres)
 qbe-typed (lower-block [op | C] E Σ) = B
```
where `B`'s typing matches `Σ'`'s tags lifted to QBE `l`. Proof by induction on `C`,
one case per opcode. Two genuinely interesting cases:

1. **`apply` (static callee):** `typed-step` enforces `arg_tags` length =
   `typed-closure` arity; the lowering emits a `call` with exactly that many `l`
   args; QBE's `call` typing checks arity against the prototype (generated from
   `typed-closure`). Closes the central soundness gap: the C VM's unchecked `nargs`
   (`zincvm.c:1568-1573`) becomes a type-checked arity in the proof.
2. **`jmpf` joins:** both branches produce the same `Σ'`; the lowering emits `phi`
   nodes whose inputs are the branch-ending temps; QBE's `phi` typing requires all
   inputs `l`-typed with the same source block — holds because every `Value` is
   `l`. Tag agreement is enforced ZINC-side, not QBE-side.

### 6.4 What is NOT proven

- GC safety of generated code (trusted `vm/gc.c`).
- `trap-error` longjmp correctness (opaque `$trap_error` call; real semantics in C).
- Phi-memory interactions with conservative GC (a `phi` input held only in a QBE
  register at a `gc_alloc` could be missed by the conservative scan). Mitigation:
  trampoline-spill discipline (§7.3). Trusted invariant, not proven.
- The full OS bundle (excluded by static-arity premise).

### 6.5 Datatype fixes needed (concrete, vs `shen/types.shen`)

1. Delete the degenerate `klambda` rule (`:19-42`) — specifically the axiom
   `___ / X : klambda`. Replace with a simply-typed `klambda-typed` judgement (Stage 1).
2. `bytecode?` (`:120-122`) lists only `grab let appterm apply pushmark return
   endlet`; `zinc-code` handles `access/global/cur/jmpf/jmp/number/string/symbol/
   boolean/prim` via specific rules (`:155-208`). `label` has no rule and isn't in
   `bytecode?` — OK because `lower` runs post-`compile-zinc` (labels already gone).
   **Decision: `lower` runs post-`compile-zinc`, same as `nat->csexp`.**
3. Add `qbe-ir` + `qbe-typed` in new `shen/qbe-types.shen` (does not touch
   `types.shen`).
4. Add `zinc-effects`/`typed-step` in new `shen/zinc-effects.shen` (preserve
   `zinc-code` as structural substrate).

---

## 7. Integration & verification

### 7.1 Where `qbe.shen` sits

```
Shen source
  → kmacros → normalize-term → debruijn      (shen/normalize.shen)
  → zinc-c                                    (shen/zinc.shen)        -- zinc-code
  → compile-zinc                               (shen/compile.shen:64) -- resolved klambda
  ├─→ nat->csexp  → globals.csexp → zincvm     (existing path)
  └─→ lower       → <name>.qbe  → qbe-as       (new path)              -- qbe-ir
                    → <name>.o → link with vm/gc.c + shims
```

`lower` is a sibling of `nat->csexp`, consuming the same `compile-zinc` output. A
new `serialize-qbe.shen` (parallel to `serialize.shen`) walks `global-table` and
emits `lower(Code)` per closure into a `.qbe` file; closures reference each other
by `@clo_<Name>`; a shared header declares prototypes.

### 7.2 Differential testing against `vm_exec_env`

Both binaries run the **same bundle**. Harness: pick KLambda program `P`; marshal
through `zinc-c`/`compile-zinc` to a ZINC entry closure; run on `zincvm` via
`eval-kl` (`zincvm.c:1185-1211`) → `R_c`; run on native binary → `R_n`; assert
`marshal(R_c) == marshal(R_n)` (via `marshal_to_tagged`, `zincvm.c:418+`).

The reduced bundle's existing self-hosting tests (AGENTS.md: Tests 1-10, A-C)
become the first differential suite.

```make
qbe: qberun globals.qbe
diff-test: zinctest qberun
qbe-stress: qberun globals.qbe
```

### 7.3 GC integration

Generated functions call `val_*`/`prim_*` shims, which call `gc_alloc` (`vm/gc.h:35`).
The conservative scanner needs every live `Value` reachable from the C stack at
each `gc_alloc`. Disciplines:
- **Trampolines spill at call boundaries.** Each `@clo_F` is invoked from C
  trampoline `@tramp_F(Value fn)` which stores args + `fn` in C locals before any
  GC-capable call. QBE's caller-save allocation keeps those locals on the stack at
  the call. Enforce by always routing GC-capable calls through C shims — never let
  QBE hold the only copy of a `Value` across such a call.
- **Env activation records are rooted on the precise-root shadow stack**
  (`gc_root_push_value` / `gc.h`) for the call duration.

Same discipline the C VM already relies on. Not proven (§6.4); a documented trusted
invariant.

### 7.4 Linking

`qberun` = small `main()`: `gc_init`, `init_globals`, parse compiled `.o`, expose
the same `--trace`/`eval-kl` entry points as `zincvm`, link `vm/gc.c` + shim file
`vm/qbe_shims.c`. The shim file is hand-written ~200 lines: `prim_<F>` per
primitive, `bind_args`, `trap_error`, `is_false`, `clo_call_dynamic`. QBE assembles
each `@clo_F` to a standard C-ABI function, so the shim↔QBE boundary is plain C
calls.

---

## 8. Risks, trade-offs, staged plan

### Risks

1. **Stage 1 (klambda fix) is the long pole** — the "prove the metacircular interp
   type-safe" task (`docs/architecture.md:42-48`). Large, independent of QBE.
   *Mitigation:* Stage 2 (untyped emitter) proceeds in parallel; Stage 4 depends on
   both.
2. **Static-arity assumption may not hold for the reduced bundle.** Need to audit
   for `apply` of a `global`/`access`-sourced function. Suspects: `safe.trap-error`,
   `safe.eval-kl`, `map`/`fold`. *Mitigation:* Stage 0 precondition check rejects any
   non-`cur` callee; if it fails, restrict the proven subset to closures that pass.
3. **`phi` + GC root liveness** (§6.4). *Mitigation:* trampoline-spill discipline +
   ASan + GC-stress test allocating between every QBE call.
4. **QBE ABI for `Value`-by-value** (§5.1). *Mitigation:* Stage 2 validates a single
   round-trip; fall back to option A if mismatched.
5. **`trap-error` longjmp across QBE frames.** *Mitigation:* `CatchFrame` always set
   up inside trusted `$trap_error`, never in generated QBE; Stage 5 stress-tests.
6. **QBE version drift.** *Mitigation:* pin a QBE version in `vendor/`; the subset
   is stable across releases.

### Staged plan (each gated by tests)

**Stage 0 — Audit & skeleton.** Shen pass `proven-subset?` over the reduced bundle's
`global-table` flagging any `apply`/`appterm` whose callee isn't a syntactic `cur`
literal. *Gate:* produce the list of proven closures (or the offenders).

**Stage 1 — The klambda fix.** Replace `shen/types.shen:19-42` with a simply-typed
`klambda-typed` judgement; re-type `zinc-c`/`zinc-t`/`interp` under `tc +`.
*Gate:* `make test` passes; `interp` type-checks. **Largest, most uncertain; run in
parallel with Stages 2-3.**

**Stage 2 — Untyped QBE emitter.** `shen/qbe.shen` (`lower : zinc-code --> string`),
`vm/qbe_shims.c`, `qberun` `main`. *Gate:* `(+ 1 2)` lowers/assembles/runs → `3`; then
AGENTS.md tests 1-4. Validates ABI, env representation, call convention — without any
proof.

**Stage 3 — Differential suite.** Run reduced bundle through both `zincvm
globals.csexp` and `qberun globals.qbe`; assert equality on Tests 1-10, A-C.
*Gate:* all pass.

**Stage 4 — Effect typing + preservation proof.** `shen/zinc-effects.shen`
(`typed-step`), `shen/qbe-types.shen` (`qbe-ir`, `qbe-typed`); check
`qbe-preservation` under `tc +`. *Gate:* lemma type-checks for the proven subset;
any closure outside it is rejected (not silently admitted). Requires Stage 1.

**Stage 5 — GC & longjmp stress on the QBE path.** Port GC nursery stress
(AGENTS.md `make run-bundle`) under `qberun`; add trap-error-heavy differential test.
*Gate:* no leaks, no missed roots (ASan + `vm/gc.c` assertions).

**Stage 6 (optional) — Full-OS dynamic fallback.** Build §4.6 varargs lowering so
`qberun` can run `globals-full.csexp` (unproven). *Gate:* `shen.initialise` returns.

### Open questions

- Does the reduced bundle contain any higher-order `apply` to a non-`cur` callee?
  (Stage 0 answers.)
- Can QBE return a 16-byte `Value` struct by value, or split tag/payload into two
  `l`? (Stage 2 answers; §5.1.)
- Does the trampoline-spill discipline (§7.3) survive QBE's optimisation passes?
- Does any reduced-bundle closure `apply` a *primitive* via `OP_APPLY`/`OP_APPTERM`
  (the `acc.tag == VAL_PRIM` branch, `zincvm.c:1605-1611`)? Those don't go through
  `safe.X` — the lowering must replicate via `prim_<F>` shims, and the effect type
  must allow `apply` to a primitive literal.

---

## 9. Recommendation

**Build Stage 0 → Stage 2 → Stage 3 first.**

1. **Stage 0 (1-2 days):** `proven-subset?` audit. Tells us whether the central
   static-arity assumption holds at all, and the exact list of closures the proof
   will cover.
2. **Stage 2 (1-2 weeks):** *untyped* `qbe.shen` + `qbe_shims.c` + `qberun`, getting
   `(+ 1 2)` and the 4 builtin tests running native. De-risks ABI, env representation,
   call convention — the engineering core — independent of the proof and Stage 1.
3. **Stage 3 (1 week):** differential harness vs `vm_exec_env`. Once `qberun` matches
   `zincvm`, an empirically-equivalent native runtime exists — useful on its own even
   before any proof lands.

**Run Stage 1 (klambda fix) in parallel** with Stages 2-3 — it's the long pole and
doesn't block the engineering. Only after Stage 1 + Stage 3 both pass attempt
**Stage 4** (the preservation proof), by which time the lowering is battle-tested by
differential testing.

**Verified-reference + fast-runtime pairing:** C VM stays the fast production runtime;
QBE path is the proven reference. Reasons: (a) the C VM's compiled-out
`PRIM_TYPE_ERROR` (`zincvm.c:393`) is faster than a shim-routed QBE path; (b) the C VM
already passes the full self-hosting + GC stress suite; (c) keeping the C VM default
means no regression risk. QBE-becomes-default deferred to Stage 6+ if Stage 5 shows
parity.

**Do not** build the §4.6 full-OS dynamic lowering until Stages 0-5 are done — it
doubles complexity for a path that's *by construction* unproven. Defer until there's a
concrete reason to run the full OS natively rather than via `zincvm-debug
globals-full.csexp`.
