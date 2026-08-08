(tc -)
(load "shen/interp.shen")
(tc -)
(load "shen/compile.shen")
(load "shen/load.shen")
(tc -)
(load "shen/shen-kl-helpers.shen")
(load "shen/shen->kl.shen")

\* === Compile .shen files through our own full-arity compiler ===
   shen-load reads a .shen file via shen-read-file (extended reader),
   compiles each form through shen->kl (full-arity KLambda defuns), and
   installs them into global-table via interp-eval.  dedupe-globals at
   serialize time keeps the first (most recent = shen-load'd) occurrence,
   so these full-arity closures win over the host-compiled bootstrap ones.
   
   primitives.shen is shen-load'd below (its %% escapes are now supported by
   zinc.shen, so the safe wrappers compile through our own full-arity
   compiler instead of the host bootstrap). *\
(tc -)
(define shen-eval-forms
  [] -> loaded
  [F | R] -> (do (interp-eval F) (shen-eval-forms R)))
(define shen-load
  Path -> (shen-eval-forms (shen->kl-forms (shen-read-file Path))))
(tc +)

(shen-load "shen/util.shen")
(shen-load "shen/types.shen")
(shen-load "shen/zinc.shen")
(shen-load "shen/compile.shen")
(shen-load "shen/normalize.shen")
(shen-load "shen/primitives.shen")
(shen-load "shen/interp.shen")
(shen-load "shen/toplevel.shen")
(shen-load "shen/load.shen")

\* === Safe-wrapper aliases: point short names to our-compiled safe.N closures ===
   serialize.shen (full bundle) relies on interp.shen's set-toplevel calls for
   these toplevel aliases.  Here we OVERWRITE them with our full-arity compiled
   closures by prepending [Short (lookup-global Safe)] to global-table, so the
   bundle's N resolves to the SAME closure object as safe.N (eliminating the
   host-compiled `(ps X)` path from the reduced bundle).  dedupe-globals keeps
   the first (prepended) occurrence.  Must run AFTER the shen-loads above so
   lookup-global finds the our-compiled safe.N. *\
(tc -)
(set safe-alias-pairs
  [[number? safe.number?]
   [symbol? safe.symbol?]
   [string? safe.string?]
   [boolean? safe.boolean?]
   [cons? safe.cons?]
   [simple-error safe.simple-error]
   [get-time safe.get-time]
   [close safe.close]
   [read-byte safe.read-byte]
   [tl safe.tl]
   [hd safe.hd]
   [absvector safe.absvector]
   [n->string safe.n->string]
   [string->n safe.string->n]
   [str safe.str]
   [tlstr safe.tlstr]
   [value safe.value]
   [intern safe.intern]
   [error-to-string safe.error-to-string]
   [trap-error safe.trap-error]
   [= safe.=]
   [open safe.open]
   [write-byte safe.write-byte]
   [cons safe.cons]
   [fst safe.fst]
   [snd safe.snd]
   [emptylist safe.emptylist]
   [hdstr safe.hdstr]
   [read-file-as-string safe.read-file-as-string]
   [<-address safe.<-address]
   [cn safe.cn]
   [pos safe.pos]
   [<= safe.<=]
   [>= safe.>=]
   [< safe.<]
   [> safe.>]
   [set safe.set]
   [- safe.-]
   [* safe.*]
   [/ safe./]
   [+ safe.+]
   [address-> safe.address->]
   [eval-kl safe.eval-kl]])
(define install-safe-aliases
  [] -> aliases-installed
  [[Short Safe] | Rest] -> (do
    (set global-table (cons [Short (lookup-global Safe)]
                            (value global-table)))
    (install-safe-aliases Rest)))
(install-safe-aliases (value safe-alias-pairs))
(tc +)

\* REDUCED bundle: the self-contained meta-interpreter + type-safe Shen OS
   base .kl only.  Skips the heavy / type-unsafe Shen OS components (yacc,
   prolog, sequent, t-star, stlib, init, extensions) which crash the
   guard-free release C VM during shen.initialise.
   
   Because the release C VM compiles out all primitive type guards, the
   shipped bundle MUST only ever run type-safe code.  This reduced set is
   exactly that: the proven type-safe meta-interpreter plus the type-safe
   KLambda base it can run without C-level checks.  The load/eval
   infrastructure is bundled (set-toplevel below) so the interpreter can
   ALSO load and compile further type-safe .kl source at runtime —
   "closing the loop" by compiling KL through the interpreter itself. *\

\* Type-safe .kl base (no heavy OS). *\
(interp-load-raw "vendor/ShenOSKernel-41.2/klambda/core.kl")
(interp-load-raw "vendor/ShenOSKernel-41.2/klambda/declarations.kl")
(interp-load-raw "vendor/ShenOSKernel-41.2/klambda/types.kl")
(interp-load-raw "vendor/ShenOSKernel-41.2/klambda/macros.kl")
(interp-load-raw "vendor/ShenOSKernel-41.2/klambda/load.kl")
(interp-load-raw "vendor/ShenOSKernel-41.2/klambda/toplevel.kl")
(interp-load-raw "vendor/ShenOSKernel-41.2/klambda/sys.kl")
(interp-load-raw "vendor/ShenOSKernel-41.2/klambda/dict.kl")
(interp-load-raw "vendor/ShenOSKernel-41.2/klambda/track.kl")
(interp-load-raw "vendor/ShenOSKernel-41.2/klambda/reader.kl")
(interp-load-raw "vendor/ShenOSKernel-41.2/klambda/writer.kl")
(interp-load-raw "shen/overrides-pure.kl")

\* Load shen/util.shen via shen-load above (full-arity compiler). *\

\* Add shen. prefix aliases for unprefixed closures. *\
(tc -)
(define shen.has-dot?
  "" -> false
  S -> (if (= "." (hdstr S)) true (shen.has-dot? (tlstr S))))
(define shen.add-prefix-aliases
  [] -> aliases-added
  [[Name Closure] | Rest] -> (do
    (if (shen.has-dot? (str Name))
        shen.skip
        (set global-table (cons [(intern (cn "shen." (str Name))) Closure]
                                (value global-table))))
    (shen.add-prefix-aliases Rest)))
(shen.add-prefix-aliases (value global-table))

(define entry-str
  [N [lambda Code []]] -> (cn (cn (cn (cn "(" (csexp-atom N)) " ")
                                  (zinc->native [cur Code]))
                             ")")
  _ -> "")

(define entries-str
  [] -> ""
  [E | Rest] -> (cn (entry-str E) (entries-str Rest))
  _ -> "")

(set *bundle* (cn (cn "(" (entries-str (dedupe-globals (value global-table)))) ")"))
(set *out* (open "globals.csexp" out))
(pr (value *bundle*) (value *out*))
(close (value *out*))
