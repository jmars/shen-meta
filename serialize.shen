\* serialize.shen — compile all closures from global-table to csexp bundle *\
(tc -)
(load "interp.shen")
(tc -)           \* interp.shen turns tc + on at the end *\
(load "compile.shen")

\* Serialize one global-table entry: (Name (c<code>)) *\
(define entry-str
  [N [lambda Code []]] -> (cn (cn (cn (cn "(" (csexp-atom N)) " ")
                                  (zinc->native [cur Code]))
                             ")")
  _ -> "")

\* Walk the global table alist *\
(define entries-str
  [] -> ""
  [E | Rest] -> (cn (entry-str E) (entries-str Rest))
  _ -> "")

(print (cn (cn "(" (entries-str (value global-table))) ")"))
