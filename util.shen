(define id
  X -> X)

(define newvar
  -> (gensym (protect V)))

(define index_h
  X [X | Rest] C -> C
  X [_ | Rest] C -> (index_h X Rest (+ 1 C))
  _ _ _          -> -1)

(define index
  X L -> (index_h X L 0))

(define intersperse
  V []         -> []
  V [X]        -> [X]
  V [X | Rest] -> [X V | (intersperse V Rest)]
  _ _          -> [])

(define fold-append
  A []      -> A
  A [H]     -> (fold-append (append A H) [])
  A [H | T] -> (fold-append (append A H) T)
  _ _       -> (simple-error "impossible"))