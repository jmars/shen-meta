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
   
   primitives.shen is NOT shen-load'd here: its %% escapes in where-guards
   need zinc.shen %% support that is deferred to a later unit.  Its safe
   wrappers are already installed by the bootstrap set-toplevel calls. *\
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
\* primitives.shen: kept host-loaded (%% in guards, deferred) *\
(shen-load "shen/interp.shen")
(shen-load "shen/toplevel.shen")
(shen-load "shen/load.shen")

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
