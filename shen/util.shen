(define id { A --> A }
  X -> X)

(define newvar { --> symbol }
  -> (gensym (protect V)))

(define index_h { A --> (list A) --> number --> number }
  X [X | Rest] C -> C
  X [_ | Rest] C -> (index_h X Rest (+ 1 C))
  _ _ _          -> -1)

(define idx { A --> (list A) --> number }
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

(define fold-str { (list string) --> string }
  [] -> ""
  [S] -> S
  [S | Rest] -> (cn S (fold-str Rest)))

(define defun->lambda { klambda --> klambda }
  [defun Name [] Body]           -> [lambda (newvar) Body]
  [defun Name [Arg] Body]        -> [lambda Arg Body]
  [defun Name [Arg | Args] Body] -> [lambda Arg (defun->lambda [defun Name Args Body])]
  _                              -> (simple-error "defun->lambda: invalid arg"))

(define dedupe-globals { (list (list symbol zinc-value)) --> (list (list symbol zinc-value)) }
  [] -> []
  [[N V] | Rest] -> (if (empty? (assoc N Rest))
                         [[N V] | (dedupe-globals Rest)]
                         (dedupe-globals Rest))
  _ -> [])

(define primitive? { symbol --> boolean }
  X -> (element? X [+ / * - trap-error simple-error error-to-string intern
                    set value number? > < >= <= string? pos tlstr hdstr cn str
                    string->n n->string absvector address-> <-address emptylist
                    absvector? cons? cons hd tl write-byte read-byte open function?
                    close = eval-kl get-time symbol? boolean? error? stream?
                    @p fst snd gensym variable? newvar]))

\* Zinc instruction keywords used as list constructors in zinc-c/zinc-t RHS.
   These must NOT be wrapped with [function ...] by debruijn. *\
(define instruction-keyword? { symbol --> boolean }
  X -> (element? X [access global grab let jmpf jmp label
                    cons symbol prim push appterm number string boolean
                    cur endlet pushmark apply mark
                    error lambda absvector stream in out]))