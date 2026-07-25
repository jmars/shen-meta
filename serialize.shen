(tc -)
(load "interp.shen")
(tc -)
(load "compile.shen")

(interp-load "/home/arch/github/shen-scheme/kl/core.kl")

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
