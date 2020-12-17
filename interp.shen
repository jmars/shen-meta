(load "normalize.shen")
(load "util.shen")
(load "zinc.shen")

\* Reference implementation, this is basically a transliteration
  of the rules in the paper *\
(define lookup
  0 [X | _] -> X
  X [_ | Z] -> (lookup (- X 1) Z)
  _ _       -> (simple-error "failed lookup"))

(define interp-jmp
  [label L | C] L -> C
  [C1 | C] L        -> (interp-jmp C L)   
  _ _               -> (simple-error "failed jump"))

(define interp
  [access N | C] A E S R                                        -> (interp C (lookup N E) E S R)
  [global G | C] A E S R                                        -> (interp C (get interp G) E S R)
  [jmpf L | C] [boolean false] E S R                            -> (interp (interp-jmp C L) [boolean false] E S R)
  [jmpf L | C] A E S R                                          -> (interp C A E S R)
  [jmp L | C] A E S R                                           -> (interp (interp-jmp C L) A E S R)
  [label L | C] A E S R                                         -> (interp C A E S R)
  [appterm | C] [lambda C1 E1] E [V | S] R                      -> (interp C1 [lambda C1 E1] [V | E1] S R)
  [apply | C] [lambda C1 E1] E [V | S] R                        -> (interp C1 [lambda C1 E1] [V | E1] S [[lambda C E] | R])
  [push | C] A E S R                                            -> (interp C A E [A | S] R)
  [pushmark | C] A E S R                                        -> (interp C A E [mark | S] R)
  [cur C1 | C] A E S R                                          -> (interp C [lambda C1 E] E S R)
  [grab | C] A E [mark | S] [[lambda C1 E1] | R]                -> (interp C1 [lambda C E] E1 S R)
  [grab | C] A E [V | S] R                                      -> (interp C A [V | E] S R)
  [return | C] A E [mark | S] [[lambda C1 E1] | R]              -> (interp C1 A E1 S R)
  [return | C] [lambda C1 E1] E [V | S] R                       -> (interp C1 [lambda C1 E1] [V | E1] S R)
  [let | C] A E S R                                             -> (interp C A [A | E] S R)
  [endlet | C] A [V | E] S R                                    -> (interp C A E S R)
  [number N | C] A E S R                                        -> (interp C [number N] E S R)
  [string Ss | C] A E S R                                       -> (interp C [string Ss] E S R)
  [symbol Ss | C] A E S R                                       -> (interp C [symbol Ss] E S R)
  [boolean B | C] A E S R                                       -> (interp C [boolean B] E S R)

  [prim symbol? | C] [symbol _] E S R                           -> (interp C [boolean true] E S R)
  [prim symbol? | C] A E S R                                    -> (interp C [boolean false] E S R)
  [prim boolean? | C] [boolean _] E S R                         -> (interp C [boolean true] E S R)
  [prim boolean? | C] A E S R                                   -> (interp C [boolean false] E S R)
  [prim stream? | C] [stream _] E S R                           -> (interp C [boolean true] E S R)
  [prim stream? | C] A E S R                                    -> (interp C [boolean false] E S R)
  [prim get-time | C] [symbol A] E S R                          -> (interp C [number (get-time A)] E S R)

  [prim eval-kl | C] A E S R                                    -> (interp C A E S R)

  [prim close | C] [stream A] E S R                             -> (interp C (do (close A) [cons []]) E S R)
  [prim read-byte | C] [stream A] E S R                         -> (interp C [number (read-byte A)] E S R)
  [prim tl | C] [cons A] E S R                                  -> (interp C [cons (tl A)] E S R)
  [prim hd | C] [cons A] E S R                                  -> (interp C [cons (hd A)] E S R)
  [prim cons? | C] [cons _] E S R                               -> (interp C [boolean true] E S R)
  [prim cons? | C] A E S R                                      -> (interp C [boolean false] E S R)
  [prim absvector | C] [number A] E S R                         -> (interp C [absvector (absvector A)] E S R)
  [prim n->string | C] [number A] E S R                         -> (interp C [string (n->string A)] E S R)
  [prim string->n | C] [string A] E S R                         -> (interp C [number (string->n A)] E S R)
  [prim str | C] [_ A] E S R                                    -> (interp C [string (str A)] E S R)
  [prim tlstr | C] [string A] E S R                             -> (interp C [string (tlstr A)] E S R)
  [prim string? | C] [string _] E S R                           -> (interp C [boolean true] E S R)
  [prim string? | C] A E S R                                    -> (interp C [boolean false] E S R)
  [prim number? | C] [number _] E S R                           -> (interp C [boolean true] E S R)
  [prim number? | C] A E S R                                    -> (interp C [boolean false] E S R)
  [prim value | C] [symbol A] E S R                             -> (interp C (value A) E S R)
  [prim intern | C] [string A] E S R                            -> (interp C [symbol (intern A)] E S R)
  [prim error-to-string | C] [error A] E S R                    -> (interp C [string (error-to-string A)] E S R)
  [prim simple-error | C] [string A] E S R                      -> (simple-error A)
  [prim trap-error | C] [lambda C1 E1] E S R                    -> (interp C (trap-error (interp C1 [lambda C1 E1] E1 S R) (/. Err [error Err])))

  [prim type | C] A E [A1 | S] R                                -> (interp C A E S R)
  [prim = | C] A E [A1 | S] R                                   -> (interp C [boolean (= A A1)] E S R)
  [prim open | C] [string A] E [[symbol A1] | S] R              -> (interp C [stream (open A A1)] E S R)
  [prim write-byte | C] [number A] E [[stream A1] | S] R        -> (interp C [number (write-byte A A1)] E S R)
  [prim cons | C] A E [[cons A1] | S] R                         -> (interp C [cons (cons A A1)] E S R)
  [prim <-address | C] [absvector A] E [[number A1] | S] R      -> (interp C (<-address A A1) E S R)
  [prim cn | C] [string A] E [[string A1] | S] R                -> (interp C [string (cn A A1)] E S R)
  [prim pos | C] [string A] E [[number A1] | S] R               -> (interp C [string (pos A A1)] E S R)
  [prim <= | C] [number A] E [[number A1] | S] R                -> (interp C [number (<= A A1)] E S R)
  [prim <= | C] [number A] E [[number A1] | S] R                -> (interp C [number (>= A A1)] E S R)
  [prim > | C] [number A] E [[number A1] | S] R                 -> (interp C [number (> A A1)] E S R)
  [prim < | C] [number A] E [[number A1] | S] R                 -> (interp C [number (< A A1)] E S R)
  [prim set | C] [symbol A] E [A1 | S] R                        -> (interp C (set A A1) E S R)

  [prim error? | C] [error A] E S R                             -> (interp C [boolean true] E S R)
  [prim error? | C] A E S R                                     -> (interp C [boolean false] E S R)

  [prim - | C] [number A] E [[number A1] | S] R                 -> (interp C [number (- A A1)] E S R)
  [prim * | C] [number A] E [[number A1] | S] R                 -> (interp C [number (* A A1)] E S R)
  [prim / | C] [number A] E [[number A1] | S] R                 -> (interp C [number (/ A A1)] E S R)
  [prim + | C] [number A] E [[number A1] | S] R                 -> (interp C [number (+ A A1)] E S R)

  [prim address-> | C] [absvector A] E [[number A1] A2 | S] R   -> (interp C [absvector A A1 A2] E S R)

  [] A E S R                                                    -> A
  _ _ _ _ _                                                     -> (simple-error "interp: unknown expression"))


(define defun->lambda
  [defun Name [Arg] Body]        -> [lambda Arg Body]
  [defun Name [Arg | Args] Body] -> [lambda Arg (defun->lambda [defun Name Args Body])]
  _                              -> (simple-error "defun->lambda: invalid arg"))

(define toplevel-interp
  X -> (interp X [] [] [] []))

(define kl->zinc
  X -> (zinc-c (debruijn [] (normalize-term (kmacros X)))))

(define set-toplevel
  N X -> (put interp N (toplevel-interp (zinc-c (debruijn [] (normalize-term (kmacros (defun->lambda (ps X)))))))))

(load "primitives.shen")

(set-toplevel + safe.+)
(set-toplevel number? safe.number?)
(put interp first (interp (kl->zinc [lambda X [lambda Y X]]) [] [] [] []))
(put interp id (interp (kl->zinc [lambda X X]) [] [] [] []))
(put interp t (interp (kl->zinc [lambda X true]) [] [] [] []))

(= [number 40] (toplevel-interp (kl->zinc [[lambda X [lambda Y X]] 40 2])))
(= [number 40] (toplevel-interp (kl->zinc [first 40 2])))
(= [number 40] (toplevel-interp (kl->zinc [id 40])))
(= [number 1] (toplevel-interp (kl->zinc [let X 1 X])))
(= [number 1] (toplevel-interp (kl->zinc [if true 1 0])))
(= [number 1] (toplevel-interp (kl->zinc [if true [if false 0 1] 0])))
(= [boolean true] (toplevel-interp (kl->zinc [%% number? 1])))
(= [boolean true] (toplevel-interp (kl->zinc [number? 1])))
(= [number 0] (toplevel-interp (kl->zinc [if false 1 0])))
(= [boolean false] (toplevel-interp (kl->zinc [and true false])))
(= [boolean true] (toplevel-interp (kl->zinc [or false true])))
(= [number 2] (toplevel-interp (kl->zinc [let X 1 [%% + X 1]])))
(= [number 1] (toplevel-interp (kl->zinc [[lambda X [if [%% number? X] X 0]] 1])))
(= [number 41] (toplevel-interp (kl->zinc [let X [if true 1 0] [%% + X [first 40 2]]])))
(= [boolean true] (toplevel-interp (kl->zinc [and [number? 1] [number? 2]])))
(= [number 2] (toplevel-interp (kl->zinc [+ 1 1])))
(= [number 41] (toplevel-interp (kl->zinc [let X [if true 1 0] [+ X [first 40 2]]])))