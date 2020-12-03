(define id X -> X)

(define newvar -> (gensym (protect V)))

(define index*
  X [X | Rest] C -> C
  X [_ | Rest] C -> (index* X Rest (+ 1 C))
  _ _ _          -> -1)

(define index X L -> (index* X L 0))

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

(define primitive?
  X -> (element? X [+ / * - trap-error simple-error error-to-string intern
                    set value number? > < >= <= string? pos tlstr cn str
                    string->n n->string absvector address-> <-address
                    absvector? cons? cons hd tl write-byte read-byte open
                    close = eval-kl get-time type symbol?]))

(define arity?
  address->       -> 3
  +               -> 2
  /               -> 2
  *               -> 2
  -               -> 2
  trap-error      -> 2
  set             -> 2
  >               -> 2
  <               -> 2
  >=              -> 2
  <=              -> 2
  pos             -> 2
  cn              -> 2
  <-address       -> 2
  cons            -> 2
  write-byte      -> 2
  open            -> 2
  =               -> 2
  type            -> 2
  simple-error    -> 1
  error-to-string -> 1
  intern          -> 1
  value           -> 1
  number?         -> 1
  string?         -> 1
  tlstr           -> 1
  str             -> 1
  string->n       -> 1
  n->string       -> 1
  absvector       -> 1
  absvector       -> 1
  cons?           -> 1
  hd              -> 1
  tl              -> 1
  read-byte       -> 1
  close           -> 1
  eval-kl         -> 1
  get-time        -> 1
  symbol?         -> 1
  _               -> -1)

\* https://github.com/Shen-Language/wiki/wiki/KLambda#equivalent-forms *\
(define kmacros
  [freeze X]          -> [lambda (newvar) (kmacros X)]
  [thaw X]            -> [(kmacros X) 0]
  [and X Y]           -> (kmacros [if (kmacros X) (kmacros Y) false])
  [or X Y]            -> (kmacros [if (kmacros X) true (kmacros Y)])
  [cond [X Y] | Rest] -> (kmacros [if (kmacros X) (kmacros Y) (kmacros [cond | Rest])])
  [cond]              -> [simple-error "No condition was true"]
  [if true X Y]       -> (kmacros X)
  [if false X Y]      -> (kmacros Y)
  [X Y]               -> [(kmacros X) (kmacros Y)]
  [X | Y]             -> [(kmacros X) | (kmacros Y)]
  X                   -> X)

\* http://matt.might.net/articles/a-normalization/ *\
(define atomic?
  []      -> true
  X       -> true where (number? X)
  X       -> true where (symbol? X)
  X       -> true where (string? X)
  X       -> true where (boolean? X)
  X       -> true where (variable? X)
  _       -> false)

(define normalize-term Exp -> (normalize Exp (function id)))

(define normalize
  [lambda Param Body] K -> (K [lambda Param (normalize-term Body)])
  [let V X Y] K         -> (normalize X (/. Aexp
                             [let V Aexp (normalize Y K)]))
  [if X Y Z] K          -> (normalize-name X (/. T
                             (K [if T (normalize-term Y) (normalize-term Z)])))
  [set S E] K           -> (normalize-name E (/. T
                             [let (newvar) [set S T] (K [])]))
  [F | E] K             -> (normalize-name F (/. T
                             (normalize-names E (/. Ts
                               (K [T | Ts])))))
  X K                   -> (K X) where (atomic? X))

(define normalize-name
  E K -> (normalize E (/. Aexp
           (if (atomic? Aexp) (K Aexp)
             (let T (newvar)
               [let T Aexp (K T)])))))

(define normalize-names
  Exps K -> (if (empty? Exps)
              (K [])
              (normalize-name (hd Exps) (/. T
                (normalize-names (tl Exps) (/. Ts
                  (K [T | Ts])))))))

(define map-debruijn
  Scope []      -> []
  Scope [H | T] -> [(debruijn Scope H) | (map-debruijn Scope T)])

\* De Bruijn Index conversion *\
(define debruijn
  Scope [let X Y Z]  -> [let (debruijn Scope Y) (debruijn [X | Scope] Z)]
  Scope [lambda X Y] -> [lambda (debruijn [X | Scope] Y)]
  Scope [if X Y Z]   -> [if (debruijn Scope X) (debruijn Scope Y) (debruijn Scope Z)]
  Scope [%% X Y]     -> [X (debruijn Scope Y)] where (primitive? X)
  Scope [%% X | Y]   -> [X | (map-debruijn Scope Y)] where (primitive? X)
  Scope [X Y]        -> [[function X] (debruijn Scope Y)] where (and (symbol? X) (not (element? X Scope)))
  Scope [X | Y]      -> [[function X] | (map-debruijn Scope Y)] where (and (symbol? X) (not (element? X Scope)))
  Scope [X Y]        -> [(debruijn Scope X) (debruijn Scope Y)]
  Scope [X | Y]      -> [(debruijn Scope X) | (map-debruijn Scope Y)]
  Scope X            -> [lookup (index X Scope)] where (and (variable? X) (element? X Scope))
  Scope X            -> [symbol X] where (and (not (variable? X)) (symbol? X) (not (element? X Scope)))
  Scope X            -> X)

\* https://caml.inria.fr/pub/papers/xleroy-zinc.pdf *\
(define map-zinc-c
  []      -> []
  [H | T] -> [(zinc-c H) | (map-zinc-c T)])

(define zinc-t
  [lookup X]   -> [[access X]]
  [function X] -> [[global X]]
  [lambda X]   -> [grab | (zinc-t X)]
  [let X Y]    -> (append (zinc-c X) [let] (zinc-t Y))
  [if X Y Z]   -> (let F (gensym l) (let E (gensym l) (append (zinc-c X) [[jmpf F]] (zinc-c Y) [[jmp E]] [[label F]] (zinc-c Z) [[label E]])))
  [symbol X]   -> [[symbol X]] where (symbol? X)
  [F A]        -> (append (zinc-c A) [[prim F]]) where (primitive? F)
  [F | Args]   -> (append (fold-append [] (intersperse [push] (map-zinc-c (reverse (tl Args))))) [push] (zinc-c (hd Args)) [[prim F]]) where (primitive? F)
  [F | Args]   -> (append (fold-append [] (intersperse [push] (map-zinc-c (reverse Args)))) [push] (zinc-c F) [appterm])
  true         -> [[boolean true]]
  false        -> [[boolean false]]
  X            -> [[number X]] where (number? X)
  X            -> [[string X]] where (string? X)
  _            -> (simple-error "zinc-t: unknown expression"))

(define zinc-c
  [lookup X]   -> [[access X]]
  [function X] -> [[global X]]
  [lambda X]   -> [[cur (append (zinc-t X) [return])]]
  [let X Y]    -> (append (zinc-c X) [let] (zinc-c Y) [endlet])
  [if X Y Z]   -> (let F (gensym l) (let E (gensym l) (append (zinc-c X) [[jmpf F]] (zinc-c Y) [[jmp E]] [[label F]] (zinc-c Z) [[label E]])))
  [symbol X]   -> [[symbol X]] where (symbol? X)
  [F A]        -> (append (zinc-c A) [[prim F]]) where (primitive? F)
  [F | Args]   -> (append (fold-append [] (intersperse [push] (map-zinc-c (reverse (tl Args))))) [push] (zinc-c (hd Args)) [[prim F]]) where (primitive? F)
  [F | Args]   -> (append [pushmark] (fold-append [] (intersperse [push] (map-zinc-c (reverse Args)))) [push] (zinc-c F) [apply])
  true         -> [[boolean true]]
  false        -> [[boolean false]]
  X            -> [[number X]] where (number? X)
  X            -> [[string X]] where (string? X)
  []           -> []
  _            -> (simple-error "zinc-c: unknown expression"))

(define lookup
  0 [X | _] -> X
  X [_ | Z] -> (lookup (- X 1) Z)
  _ _       -> (simple-error "failed lookup"))

(define interp-jmp
  [[label L] | C] L -> C
  [C1 | C] L        -> (interp-jmp C L)   
  _ _               -> (simple-error "failed jump"))

(define unwrap
  [number N]  -> N
  [string S]  -> S
  [boolean S] -> S
  [symbol S]  -> S
  _           -> (simple-error "unwrap: unknown value"))

(define wrap
  N -> [number N] where (number? N)
  B -> [boolean B] where (boolean? B)
  S -> [symbol S] where (symbol? S)
  S -> [string S] where (string? S)
  _ -> (simple-error "wrap: unknown value"))

\* Reference implementation *\
(define interp
  [[access N] | C] A E S R                         -> (interp C (lookup N E) E S R)
  [[global G] | C] A E S R                         -> (interp C (get interp G) E S R)
  [[jmpf L] | C] [boolean false] E S R             -> (interp (interp-jmp C L) [boolean false] E S R)
  [[jmpf L] | C] A E S R                           -> (interp C A E S R)
  [[jmp L] | C] A E S R                            -> (interp (interp-jmp C L) A E S R)
  [[label L] | C] A E S R                          -> (interp C A E S R)
  [appterm | C] [lambda C1 E1] E [V | S] R         -> (interp C1 [lambda C1 E1] [V | E1] S R)
  [apply | C] [lambda C1 E1] E [V | S] R           -> (interp C1 [lambda C1 E1] [V | E1] S [[lambda C E] | R])
  [push | C] A E S R                               -> (interp C A E [A | S] R)
  [pushmark | C] A E S R                           -> (interp C A E [mark | S] R)
  [[cur C1] | C] A E S R                           -> (interp C [lambda C1 E] E S R)
  [grab | C] A E [mark | S] [[lambda C1 E1] | R]   -> (interp C1 [lambda C E] E1 S R)
  [grab | C] A E [V | S] R                         -> (interp C A [V | E] S R)
  [return | C] A E [mark | S] [[lambda C1 E1] | R] -> (interp C1 A E1 S R)
  [return | C] [lambda C1 E1] E [V | S] R          -> (interp C1 [lambda C1 E1] [V | E1] S R)
  [let | C] A E S R                                -> (interp C A [A | E] S R)
  [endlet | C] A [V | E] S R                       -> (interp C A E S R)
  [[number N] | C] A E S R                         -> (interp C [number N] E S R)
  [[string Ss] | C] A E S R                        -> (interp C [string Ss] E S R)
  [[symbol Ss] | C] A E S R                        -> (interp C [symbol Ss] E S R)
  [[boolean B] | C] A E S R                        -> (interp C [boolean B] E S R)
  [[prim P] | C] A E S R                           -> (interp C (wrap ((function P) (unwrap A))) E S R) where (= (arity? P) 1)
  [[prim P] | C] A E [A1 | S] R                    -> (interp C (wrap ((function P) (unwrap A) (unwrap A1))) E S R) where (= (arity? P) 2)
  [[prim P] | C] A E [A1 A2 | S] R                 -> (interp C (wrap ((function P) (unwrap A) (unwrap A1) (unwrap A2))) E S R) where (= (arity? P) 3)
  [] A E S R                                       -> A
  _ _ _ _ _                                        -> (simple-error "interp: unknown expression"))

(define toplevel-interp
  X -> (interp X [] [] [] []))

(define kl->zinc
  X -> (zinc-c (debruijn [] (normalize-term (kmacros X)))))

(define defun->lambda
  [defun Name [Arg] Body] -> [lambda Arg Body]
  [defun Name [Arg | Args] Body] -> [lambda Arg (defun->lambda [defun Name Args Body])]
  _ -> (simple-error "defun->lambda: invalid arg"))

===
self-checks
===

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

\* Restricted subset of klambda to QBE SSA *\
(define normalize-body
  Body -> (normalize-term (kmacros Body)))

(define callargs->qbe
  [H] -> [[% H]] where (variable? H)
  [V] -> [[d_ V]] where (number? V)
  [H | T] -> [[% H] , | (callargs->qbe T)]) where (variable? H)

(define body->qbe
  [F | Args] -> [F (callargs->qbe Args)] where (symbol? F)
  V -> [% V] where (variable? V))

(define args->qbe
  [H] -> [[l [% H]]]
  [H | T] -> [[l [% H]] , | (args->qbe T)])

(define defun->qbe
  [defun Name Args Body] -> [
    function l [$ Name] (args->qbe Args) [
      [@ start]
      [ret (body->qbe (normalize-body Body))]
    ]
  ])

\* printing *\
(define mnl -> (make-string "~%"))

(define qbebody->str
  [[ret L]] -> (@s "ret " (qbe->str L) (mnl))
  [[@ L] | T] -> (@s "@" (str L) (mnl) (qbebody->str T))
  [V] -> (@s "ret " (qbe->str V)))

(define qbeargs->str
  [[T N]] -> (@s (str T) " " (qbe->str N))
  [[T N] , | Tl] -> (@s (str T) " " (qbe->str N) ", " (qbeargs->str Tl)))

(define qbe->str
  [$ N] -> (@s "$" (str N))
  [% N] -> (@s "%" (str N))
  [@ N] -> (@s "@" (str N))
  [function R N Args Body] -> (@s
    "function " (str R) " " (qbe->str N) " (" (qbeargs->str Args) ") {" (mnl)
      (qbebody->str Body)
    "}" (mnl)
    ))