(tc -)
(load "shen/interp.shen")
(tc -)
(load "shen/compile.shen")
(load "shen/load.shen")

\* Load all .kl files via interp-load-raw — bypasses the Shen reader's
   process-sexprs (macro expansion, arity finding, type-checking) since
   .kl files are already compiled KLambda.  This avoids the shen. prefix
   mismatch where bundled code patterns use bare 'define' but .kl forms
   have 'shen.define' baked in by the module system. *\
(interp-load-raw "/home/arch/github/shen-scheme/kl/core.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/declarations.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/types.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/macros.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/load.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/toplevel.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/sys.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/dict.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/track.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/reader.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/compiler.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/writer.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/yacc.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/prolog.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/sequent.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/t-star.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/overrides.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/extension-expand-dynamic.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/extension-features.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/extension-launcher.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/extension-programmable-pattern-matching.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/shen-scheme-extensions.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/stlib.kl")
(interp-load-raw "/home/arch/github/shen-scheme/kl/init.kl")

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
(set *out* (open "globals.csexp" out))
(pr (value *bundle*) (value *out*))
(close (value *out*))
