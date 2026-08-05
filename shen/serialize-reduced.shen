(tc -)
(load "shen/interp.shen")
(tc -)
(load "shen/compile.shen")
(load "shen/load.shen")

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

\* Load shen/util.shen via interp-load-raw (bypasses reader). *\
(interp-load-raw "shen/util.shen")

\* Bundle the load/eval infrastructure so the interpreter can compile and
   load further .kl source at runtime (close the loop).  These are plain
   Shen defuns in toplevel.shen / load.shen that are NOT otherwise
   set-toplevel'd; without this they'd be val_prim placeholders and the
   runtime could not load anything.  Order matters: interp-eval first,
   then the load.shen chain (read-file-raw before interp-load-raw). *\
(tc -)
(set-toplevel defun->lambda defun->lambda)
(set-toplevel interp-eval interp-eval)
(set-toplevel strlen strlen)
(set-toplevel strlen-acc strlen-acc)
(set-toplevel chars->str chars->str)
(set-toplevel digit-ch? digit-ch?)
(set-toplevel ws-ch? ws-ch?)
(set-toplevel str->num str->num)
(set-toplevel str->num-acc str->num-acc)
(set-toplevel parse-num-str parse-num-str)
(set-toplevel skip-comment skip-comment)
(set-toplevel skip-ws skip-ws)
(set-toplevel parse-string-chars parse-string-chars)
(set-toplevel parse-string parse-string)
(set-toplevel read-atom-chars read-atom-chars)
(set-toplevel parse-atom parse-atom)
(set-toplevel parse-list-tail parse-list-tail)
(set-toplevel parse-list parse-list)
(set-toplevel parse-expr parse-expr)
(set-toplevel parse-exprs parse-exprs)
(set-toplevel read-file-raw read-file-raw)
(set-toplevel interp-load interp-load)
(set-toplevel interp-load-safe interp-load-safe)
(set-toplevel interp-load-raw interp-load-raw)
(set-toplevel interp-eval-all interp-eval-all)
(set-toplevel interp-eval-safe interp-eval-safe)
(tc +)

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
