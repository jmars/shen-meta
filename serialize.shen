(tc -)
(load "interp.shen")
(tc -)
(load "compile.shen")

(define entry-str
  [N [lambda Code []]] -> (cn (cn (cn (cn "(" (csexp-atom N)) " ")
                                  (zinc->native [cur Code]))
                             ")")
  _ -> "")

(define entries-str
  [] -> ""
  [E | Rest] -> (cn (entry-str E) (entries-str Rest))
  _ -> "")

(print (cn (cn "(" (entries-str (dedupe-globals (value global-table)))) ")"))
