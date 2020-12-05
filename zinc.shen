(define callargs->qbe
  [H]     -> [[l [% H]]] where (variable? H)
  [V]     -> [[d [d_ V]]] where (number? V)
  [H | T] -> [(callargs->qbe [H]) | (callargs->qbe T)]) where (variable? H)

(define body->qbe
  [let V X Y] -> [[]]
  [if C T F] -> [[]]
  [F | Args] -> [[call [$ F] (callargs->qbe Args)]] where (symbol? F))

(define args->qbe
  [H]     -> [[l [% H]]]
  [H | T] -> [[l [% H]] | (args->qbe T)])

(define defun->qbe
  [defun Name Args Body] -> [
    function l [$ Name] (args->qbe Args) (let B (reverse (body->qbe (normalize-body Body))) (append 
      [[@ start]]
      [[[% r] [= l] (hd B)]]
      (reverse (tl B))
      [[ret [% r]]]
    ))
  ])

\* printing *\
(define mnl -> (make-string "~%"))

(define qbebody->str
  [[[% V] [= l] F] | R] -> (@s (qbe->str [% V]) " =l " (qbe->str F) (mnl) (qbebody->str R))
  [[ret L]]             -> (@s "ret " (qbe->str L) (mnl))
  [[@ L] | T]           -> (@s "@" (str L) (mnl) (qbebody->str T)))

(define qbeargs->str
  [A]      -> (qbe->str A)
  [A | Tl] -> (@s (qbe->str A) ", " (qbeargs->str Tl)))

(define qbe->str
  [$ N]                    -> (@s "$" (str N))
  [% N]                    -> (@s "%" (str N))
  [@ N]                    -> (@s "@" (str N))
  [d_ N]                   -> (@s "d_" (str N)) where (number? N)
  [l X]                    -> (@s "l " (qbe->str X))
  [call [$ F] Args]        -> (@s "call " (qbe->str [$ F]) "(" (qbeargs->str Args) ")") where (symbol? F)
  [function R N Args Body] -> (@s
    "function " (str R) " " (qbe->str N) " (" (qbeargs->str Args) ") {" (mnl)
      (qbebody->str Body)
    "}"
    ))