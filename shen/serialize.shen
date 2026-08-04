(tc -)
\* FULL Shen OS bundle (type-unsafe; requires a guards-enabled build to run).
   The guard-free release C VM CANNOT run this — shen.initialise segfaults
   (see AGENTS.md).  The canonical guard-free bundle is built by
   serialize-reduced.shen → globals.csexp.  This target writes
   globals-full.csexp for debug/testing with the full OS. *\

(load "shen/interp.shen")
(tc -)
(load "shen/compile.shen")
(load "shen/load.shen")

\* Load all .kl files via interp-load-raw — bypasses the Shen reader's
   process-sexprs (macro expansion, arity finding, type-checking) since
   .kl files are already compiled KLambda.  This avoids the shen. prefix
   mismatch where bundled code patterns use bare 'define' but .kl forms
   have 'shen.define' baked in by the module system.
   
   Source: standard Shen OS Kernel 41.2 distribution (pure KLambda, no
   host-specific overrides).  The shen-scheme kl/ directory has 3 extra
   files (overrides.kl, compiler.kl, shen-scheme-extensions.kl) that use
   scm.* Scheme primitives unavailable in our C VM — we don't load them.
   The 2 host primitives needed by the standard distribution
   (shen.char-stinput?, shen.char-stoutput?) are provided by
   overrides-pure.kl. *\
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/core.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/declarations.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/types.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/macros.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/load.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/toplevel.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/sys.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/dict.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/track.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/reader.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/writer.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/yacc.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/prolog.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/sequent.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/t-star.kl")
(interp-load-raw "/home/arch/github/shen-meta/shen/overrides-pure.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/extension-expand-dynamic.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/extension-features.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/extension-launcher.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/extension-programmable-pattern-matching.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/stlib.kl")
(interp-load-raw "/home/arch/github/shen-scheme/ShenOSKernel-41.2/klambda/init.kl")

\* Load shen/util.shen via interp-load-raw — bypasses process-sexprs/find-arities
   to avoid the { } type annotation handling bug in the bundled reader.
   Functions like id, newvar, defun->lambda, primitive? are needed by the pipeline. *\
(interp-load-raw "/home/arch/github/shen-meta/shen/util.shen")

\* Add shen. prefix aliases for unprefixed closures.
   The Shen module system adds the package prefix during Shen->KLambda
   compilation, so bytecode references shen.<name>.  But yacc.kl defines
   <e>, <!>, <end> without the prefix.  Create aliases so both lookup
   paths work. *\
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
(set *out* (open "globals-full.csexp" out))
(pr (value *bundle*) (value *out*))
(close (value *out*))
