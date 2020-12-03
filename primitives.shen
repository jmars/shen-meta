(define safe.number?
  N -> true where (%% number? N)
  _ -> false)

(define safe.+
  A B -> (%% + A B) where (and (number? A) (number? B))
  _ _ -> (simple-error "+: invalid args"))