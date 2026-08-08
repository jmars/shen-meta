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
| Root-set dump | `--gc-dump-roots` | Dumps the shadow stack at each collection |

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

## Open investigation — precise-root-miss

**Symptom:** `interp-load-raw` / `interp-eval-all` return the **symbol `or`**
instead of `loaded`; `appterm non-lambda` fires on a symbol. Trace-dependent:
adding `--trace interp-load-raw` makes it succeed.

### What the tooling established (important)

- **`gc_check_closure` does NOT fire** (0 GC-CHECK lines). Every closure's
  `Instr`/env array header is structurally valid at `APPLY`/`APPTERM` entry.
- **Therefore the bug is NOT "a closure's code array got collected/freed."** It is
  a **wrong-value** result — a closure **resolves to the symbol `or`** somewhere in
  the metacircular interp path (a stale/wrong pointer, or `lookup-global`/`assoc`
  returning the wrong entry). This redirects the hypothesis away from a torn
  `Instr` array toward a value-on-stack/env/frame resolving wrong.
- Probe run: 54 collections (42 NURSERY + 12 FULL); triggers 42 PREEMPTIVE, 11
  ALLOC, 1 THRESHOLD (no REACTIVE/LASTRESORT). `shadow_depth` mostly 0–2 during
  bundle load, spiking to 45 at one FULL collection in the deep recursion.
- Isolation (5 direct-call probes): `strlen` clean (incl. 2nd call), `read-file-as-string`
  clean, `read-file-raw` **first corruption**; downstream stages fail only because
  `read-file-raw` already corrupted.

### Next debugging steps

1. Use `--gc-verbose`/`--gc-dump-roots` to correlate the first bad collection with
   the shadow-stack contents at that point.
2. Find where the symbol `or` enters the value stream during the deep `read-file-raw`
   recursion (a `lookup-global`/`assoc` returning `or`, or a stale value).
3. Confirm the root-miss: which C local holding a GC pointer is not pushed on the
   shadow stack at collection time.

---

## Deferred GC tooling items

Not built. Higher cost, and not the right tool for the current wrong-value bug.
Revisit only if the three tools above don't crack it.

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
