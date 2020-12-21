(define id { A --> A }
  X -> X)

(define newvar { --> symbol }
  -> (gensym (protect V)))

(define index_h { A --> (list A) --> number --> number }
  X [X | Rest] C -> C
  X [_ | Rest] C -> (index_h X Rest (+ 1 C))
  _ _ _          -> -1)

(define index { A --> (list A) --> number }
  X L -> (index_h X L 0))

(define intersperse { A --> (list A) --> (list A) }
  V []         -> []
  V [X]        -> [X]
  V [X | Rest] -> [X V | (intersperse V Rest)]
  _ _          -> [])

(define fold-append { (list A) --> (list (list A)) --> (list A) }
  A []      -> A
  A [H]     -> (fold-append (append A H) [])
  A [H | T] -> (fold-append (append A H) T)
  _ _       -> (simple-error "impossible"))

(define primitive? { symbol --> boolean }
  X -> (element? X [+ / * - trap-error simple-error error-to-string intern
                    set value number? > < >= <= string? pos tlstr cn str
                    string->n n->string absvector address-> <-address emptylist
                    absvector? cons? cons hd tl write-byte read-byte open function?
                    close = eval-kl get-time symbol? boolean? error? stream?]))