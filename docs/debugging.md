# GC debugging — tooling & open investigation

Working notes for the custom moving generational collector (`vm/gc.c`, `vm/gc.h`,
`vm/zinctypes.h`). This documents the GC debugging tooling we built, the open
precise-root-miss investigation it serves, and the **deferred tooling items**.

## Context

The C VM loads a reduced self-contained bundle (`globals.csexp`) containing the
metacircular Shen interpreter. Loading a Shen OS `.kl` file at runtime via the
bundled `interp-load-raw` triggers deep Shen recursion
(`read-file-raw → parse-exprs → parse-expr → parse-list → … → strlen-acc`) that
produces **trace-dependent corruption** (classic precise-root-miss signature).

## GC debugging tooling (built, opt-in)

All tools are **opt-in argv flags** (matching the existing `--trace`), pure
observability, no GC correctness/semantics change. Release build and default
`make test` are unaffected.

| Tool | Flag | What it does |
|---|---|---|
| Per-collection stats | `--gc-verbose` | One line per collect: trigger, `shadow_depth`, `nursery_free` / `live_pages` |
| Closure-header guard | `--gc-check-closures` | `gc_check_closure` validates every `VAL_LAMBDA` code/env header at `APPLY`/`APPTERM` entry |
| Root-set dump | `--gc-dump-roots` | Dumps the shadow stack at each collection (now cross-checks each root's pointee page liveness — flags `DEAD-SPACE` roots) |
| Stale-ref scan | `--gc-stale-scan` | Walks the native C stack after each collection, flagging words that point into the just-abandoned old-gen semi-space or the nursery. `FORWARDED` header = smoking-gun root-miss (object moved, this ref not updated). Prints per-frame attribution so the owning C function can be resolved. |
| GC log file | `--gc-log <path>` | Routes opt-in GC diagnostics to a file instead of interleaving with Shen `fn`/`run time` stderr noise |

### Where things live

- `vm/gc.c` / `vm/gc.h` — new statics (`gc_verbose`, `gc_check_closures`,
  `gc_dump_roots`, `gc_collect_seq`) + setters; `gc_check_closure(Value*, const
  char *where)`; `collect()`/`collect_nursery()` now take a `const char *trigger`
  (`PREEMPTIVE`/`REACTIVE`/`THRESHOLD`/`ALLOC`/`LASTRESORT`); root-set dump at the
  top of `gc_scan_roots()`.
- `vm/zincvm.c:216` — `#define check_closure(cl, where)` replaced with
  `gc_check_closure(&(cl), where)` (was a no-op); call sites `APPLY` (~1752) and
  `APPTERM` (~1859) now invoke the real check. Flag scan added to both `main()`
  argv loops.
- `Makefile` — `gcdebug` convenience target (no new compile target; uses the
  existing `zinctest-debug`).

### Build / run

```sh
make                      # release (zincvm, zincdec, zinctest)
make debug                # -O0 -g -DZINCVM_DEBUG
make test                 # 34/34 built-in tests (release, unaffected)
make gcdebug              # prints available flags

# Probe build (runtime .kl load experiment):
cosmocc -Wall -Wextra -O2 -I vm -DZINCTEST -DZINC_TEST_OS_LOAD \
  -o zinctest-osload vm/zinctest.c vm/zincvm.c vm/gc.c
./zinctest-osload.com.dbg globals.csexp \
  --gc-verbose --gc-check-closures --gc-dump-roots
```

## Open investigation — precise-root-miss (GC bug confirmed in compile path)

**Two distinct bugs.** The first (a compiler bug, below) was resolved; the second
(a genuine precise-root-miss GC bug in the defun-compile path) is still open.

### Bug 1 (RESOLVED, commit `98f98bb`) — n-ary `and`/`or` compiler bug

`kmacros` in `shen/normalize.shen` only expanded 2-arg `[and X Y]`/`[or X Y]`.
Bundled source uses n-ary forms — `read-atom-chars` (`load.shen:108`) has a
5-arg `(or ...)`, `parse-atom` (`load.shen:124`) a 3-arg `(and ...)`. These fell
through to the general `[X | Y]` rule and compiled to `[global or]`/`[global and]`
+ apply. `or`/`and` are not C primitives, so `global_get` returned the symbol,
giving `appterm non-lambda` returning symbol `or`. This surfaced only when the
OS-load probe first exercised `read-file-raw` (built-in tests use the YACC
parser, not `read-file-raw`).

**Fix:** added n-ary `and`/`or` rules to `kmacros` (normalize.shen). Rebuilt
`globals.csexp` via `vendor/shen-scheme/bin/shen-scheme script shen/serialize-reduced.shen`.
`read-atom-chars`/`parse-atom` now compile to `if`/`jmpf` chains; `make test`
34/34; the **parse** path now works (`read-file-raw` returns the parse list).

### Bug 2 (OPEN) — precise-root-miss GC corruption in the defun-compile path

**Symptom:** compiling a `.kl` defun via `interp-load-raw` returns `loaded` but
the defun is NOT actually stored — `lookup-global my-add` → `[error "global not
found: my-add"]`. (`interp-eval-safe`'s `trap-error` swallows the per-form
compile failure, so `interp-eval-all` still returns `loaded`.) `eval-kl` on the
loaded name returns the input identity (its CatchFrame swallows the error).

**Localization:** `interp-eval` on `[defun my-add [X Y] [+ X Y]]` →
`kl->zinc (defun->lambda ...)` → the general lambda path (`kmacros →
normalize-term → debruijn → zinc-c`) produces `runtime: unknown op '\x02'` —
a corrupted Instr array. `--gc-check-closures` **now fires**: a closure applied
at `APPLY` has `code ptr=... page=<garbage huge number> space=2` — the closure's
`.code` pointer is stale/out-of-heap. This is a genuine precise-root-miss: an
object was moved by GC but a C local holding the closure (its `.code` interior
pointer) was not rooted/updated during the deep recursion.

**Progress (commits `40c6d31`, `de28c3d`, `98f98bb`):**
- **Root cause of the full-collect stale pointer identified:** a full collect
  treats the nursery as opaque (`gc_move` leaves nursery objects untouched when
  `!in_scavenge`), so nursery-resident closures pointing to old-gen Instr arrays
  were not updated when those arrays were evacuated → stale `.code` into the dead
  semi-space. **Fixed** with a nursery-promotion pass at the top of `collect()`
  (scan roots + non-dirty globals with `in_scavenge=1`, Cheney-drain to promote
  to old-gen before the swap), plus `CallFrame.code` evacuation in the CallFrame
  scanner. Reviewer-verified sound; no regression.
- **Fixed a masked compiler bug:** `not` was missing from the bundle (debruijn
  uses `(not (element? ...))` → `[global not]` → bare symbol → compile failed
  silently). Added `not` to `os-helpers.shen`.
- **STILL OPEN:** a residual precise-root-miss remains in the deep defun-compile
  recursion. The OS-load probe still shows 12 stale-code-pointer GC-CHECK lines
  (`space=2`) and `unknown op` (trace-sensitive: `\x00` vs `\x04` by traced
  closure). The `not` fix lets debruijn execute further, but a closure's `.code`
  is still corrupted mid-compile. The specific unrooted C local is not yet found.

### Bug 2 — stale-scan localization (commit `---`)

`--gc-stale-scan` now pinpoints the root-miss precisely. On the probe
(`./zinctest-osload globals.csexp --gc-stale-scan --gc-check-closures --gc-log /tmp/gc.log`):

- **108 scans** (one per collection; 42 NURSERY + 12 FULL + threshold), **509
  FORWARDED + 637 dead-space** stale hits total. Most early hits are nursery
  scavenges (`old_space=3`) — noisy, low-signal.
- **The smoking gun:** the exact Instr page of the later `GC-CHECK` failure
  (e.g. page `274537990562`, closure `.code=0x7fd77a934408`) appears as a
  `FORWARDED` stale hit **8 bytes earlier** (`ptr=0x7fd77a934400`) at
  **full collect #48** (`old_space=2`, dead old-gen semi-space), stack offset
  `local+449`. So a nursery-resident (or otherwise unrooted) C local held an
  interior `Instr*` into a live array; when full collect #48 evacuated that
  array to the other semi-space, the local was NOT updated → stale `.code`.
- Frame attribution: the stale slot sits in the frame chain
  `gc_stale_scan_stack → collect → gc_alloc_oldgen → gc_nursery_tests → main`
  (return addresses resolved via symtab). The unrooted interior pointer is
  held across the full-collect's promotion/evacuation, most likely in a
  `vm_exec_env`-driven helper (e.g. `val_cons`, `marshal_to_tagged`,
  `deep_equal`, or an eval-kl wrapper) that keeps a `Value*`/`Instr*` local
  alive across an allocating call without a `gc_root_push_*`.

This confirms Bug 2 is a genuine precise-root-miss, localized to full-collect
#48 during `interp-eval → kl->zinc → normalize → debruijn → zinc-c`. The fix
is to find the specific C local (via the frame attribution) and root it with
`gc_root_push_*` (or re-derive `acc.lambda.code`/`.env` after each allocating
call). The stale-scan's frame map turns the root-miss into a nameable C site.
  Next: trace the exact collection (#51 FULL, THRESHOLD, shadow_depth=70 in the
  deep recursion) with `--gc-dump-roots` and find which closure's `.code` is
  left in the dead semi-space during `interp-eval → kl->zinc → normalize →
  debruijn → zinc-c`.

### What the GC tooling established (during the hunt)

- Bug 1 (parse): `gc_check_closure` did NOT fire — every closure header was valid;
  the failure was a wrong-value (`[global or]` → symbol `or`), not a root-miss.
- Bug 2 (compile): `gc_check_closure` DOES fire — a closure's `.code` pointer is
  stale/garbage (page number out of heap). A real root-miss during the
  `interp-eval → kl->zinc → normalize → debruijn → zinc-c` recursion.
- Probe run: 54 collections (42 NURSERY + 12 FULL); triggers 42 PREEMPTIVE, 11
  ALLOC, 1 THRESHOLD (no REACTIVE/LASTRESORT). `shadow_depth` mostly 0–2 during
  bundle load, spiking to 45 at one FULL collection in the deep recursion.
- Isolation (5 direct-call probes): `strlen` clean (incl. 2nd call), `read-file-as-string`
  clean, `read-file-raw` first corruption (before the n-ary fix); downstream
  stages fail only because `read-file-raw` already corrupted.

### Still open / follow-up

- **Bug 2 (the real GC precise-root-miss)** is the blocker to runtime `.kl`
  loading. The probe wiring is now correct (eval-kl → namespace 2); the failure
  is purely the compile-path GC corruption. Next: find which C local holding a
  closure's `.code` pointer is not rooted during `interp-eval → kl->zinc →
  normalize → debruijn → zinc-c`, using `--gc-verbose`/`--gc-dump-roots`/`--gc-check-closures`.
- The two-namespace split (AGENTS.md): runtime-loaded defuns live in the interp's
  Shen `global-table` (namespace 2), not the C VM native `global_table[]`
  (namespace 1). Drive loaded closures through `eval-kl`/`interp`, not raw
  `global` bytecode. The probe now does this correctly.
- The GC tooling (3 flags) is in place and actively used to catch Bug 2.

### Next debugging steps (superseded by the resolution — kept for reference)

1. Use `--gc-verbose`/`--gc-dump-roots` to correlate the first bad collection with
   the shadow-stack contents at that point.
2. Find where the symbol `or` enters the value stream during the deep `read-file-raw`
   recursion (a `lookup-global`/`assoc` returning `or`, or a stale value).
3. Confirm the root-miss: which C local holding a GC pointer is not pushed on the
   shadow stack at collection time.

---

## Deferred GC tooling items

Not built. Revisit only if the tools above don't crack it.

### Deferred: real heap verifier (`--gc-verify`)

`verify_heap()` is currently a **no-op** macro (`((void)0)` in `zincvm.h:25`);
call sites exist (e.g. `zinctest.c:81`, `:1271`) but do nothing.

- Replace with a real `gc_verify_heap()` guarded by a flag.
- What to check: header type tag in `[0,4]`; header word count nonzero and within
  remaining page words; `OBJECT`/`CONTINUED` chain consistent (CONTINUED pages share
  the OBJECT's space tag); nursery pages (space==3) have fresh headers; no page in
  `current_space` has a `FORWARDED` header; all CONTINUED pages have a preceding
  OBJECT in the same space run.
- **Cost:** O(live_pages) — slow under deep recursion. Mitigate with
  `--gc-verify-every=N` or verify only pages allocated since last verify.
- **Value:** proves GC algorithm correctness (already covered by the binary
  pass/fail churn/compaction tests); won't explain *why* a root was missed.

### Deferred: object-allocation ring buffer (`--gc-alloc-log`)

- Bounded ring buffer (e.g. 4096 entries), each:
  `{seq, type_tag, bytes, addr, is_nursery, shadow_depth_at_alloc}`.
- Written on every `gc_alloc`/`gc_alloc_oldgen`; dump the tail after a crash
  (SIGSEGV handler or `atexit`).
- **Cost:** a few writes per allocation — measurable.
- **Value:** post-mortem trail ("closure X allocated at addr Y → collection Z →
  used & crashed"), but doesn't directly pinpoint a root-miss. Heavier; lowest ROI.

### Deferred: AddressSanitizer (`-fsanitize=address`)

- **Does NOT link under cosmocc** — `undefined reference to __asan_*`
  (`__asan_init`, `__asan_report_*`). The cosmopolitan toolchain ships no `libasan`
  runtime. Instrumentation compiles but linking fails.
- Only `-fsanitize=undefined` links (existing `*-asan` Makefile targets).
- Revisit only if a native-compiler build path (non-cosmocc) is introduced.
