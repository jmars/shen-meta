(define strlen
  (@s S "") -> 1
  (@s _ Ss) -> (+ 1 (strlen Ss)))

(define sexp-str
  []      -> ""
  [X | Y] -> (@s X (sexp-str Y)))

(define atom-type
  X -> "s" where (symbol? X)
  X -> "b" where (boolean? X)
  X -> "n" where (number? X)
  X -> "S" where (string? X))

(define sexp->csexp
  []      -> "()"
  [X | Y] -> (@s "(" (sexp-str (map (function sexp->csexp) [X | Y])) ")")
  X       -> (@s "[" (str (strlen X)) ":" "S" "]" X) where (string? X)
  X       -> (let S (str X) (@s "[" (str (strlen S)) ":" (atom-type X) "]" S)))