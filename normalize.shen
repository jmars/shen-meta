\* https://github.com/Shen-Language/wiki/wiki/KLambda#equivalent-forms *\
(load "util.shen")
(load "types.shen")

(define map-kmacros { (list klambda) --> (list klambda) }
  []      -> []
  [H | T] -> [(kmacros H) | (map-kmacros T)])

(define kmacros { klambda --> klambda }
  [freeze X]                                   -> [lambda (newvar) (kmacros X)]
  [thaw X]                                     -> [(kmacros X) 0]
  [and X Y]                                    -> (kmacros [if (kmacros X) (kmacros Y) false])
  [or X Y]                                     -> (kmacros [if (kmacros X) true (kmacros Y)])
  [cond [X Y] | Rest]                          -> (kmacros [if (kmacros X) (kmacros Y) (kmacros [cond | Rest])])
  [cond]                                       -> [simple-error "No condition was true"]
  [trap-error B E]                             -> [trap-error (kmacros [lambda (newvar) (kmacros B)]) (kmacros E)]
  [if true X Y]                                -> (kmacros X)
  [if false X Y]                               -> (kmacros Y)
  [do X]                                       -> (kmacros X)
  [do X | Y]                                   -> (kmacros [let (newvar) (kmacros X) (kmacros [do | Y])])
  [number? [type X number]]                    -> true
  [symbol? [type X symbol]]                    -> true
  [string? [type X string]]                    -> true
  [boolean? [type X boolean]]                  -> true
  [cons? [type X [list _]]]                    -> true
  [simple-error [type X string]]               -> [%% simple-error X]
  [get-time [type unix symbol]]                -> [%% get-time unix]
  [get-time [type run symbol]]                 -> [%% get-time run]
  [close [type X stream]]                      -> [%% close X]
  [read-byte [type X stream]]                  -> [%% read-byte X]
  [absvector [type X number]]                  -> [%% absvector X]
  [n->string [type X number]]                  -> [%% n->string X]
  [string->n [type X string]]                  -> [%% string->n X]
  [value [type X symbol]]                      -> [%% value X]
  [intern [type "true" string]]                -> true
  [intern [type "false" string]]               -> false
  [intern [type X string]]                     -> [%% intern X]
  [error-to-string [type X exception]]         -> [%% error-to-string X]
  [open [type X string] [type in symbol]]      -> [%% open X in]
  [open [type X string] [type out symbol]]     -> [%% open X in]
  [write-byte [type N number] [type S stream]] -> [%% write-byte N S]
  [cn [type S string] [type S1 string]]        -> [%% cn S S1]
  [pos [type S string] [type N number]]        -> [%% pos S N]
  [<= [type N number] [type N1 number]]        -> [%% <= N N1]
  [>= [type N number] [type N1 number]]        -> [%% >= N N1]
  [< [type N number] [type N1 number]]         -> [%% < N N1]
  [> [type N number] [type N1 number]]         -> [%% > N N1]
  [set [type S symbol] Y]                      -> [%% set S Y]
  [- [type N number] [type N1 number]]         -> [%% - N N1]
  [* [type N number] [type N1 number]]         -> [%% * N N1]
  [/ [type N number] [type N1 number]]         -> [%% / N N1]
  [+ [type N number] [type N1 number]]         -> [%% + N N1]
  [type X Y]                                   -> X
  [X]                                          -> [X 0]
  [X Y]                                        -> [(kmacros X) (kmacros Y)]
  [X | Y]                                      -> [(kmacros X) | (map-kmacros Y)]
  []                                           -> [%% emptylist 0]
  X                                            -> X)

\* http://matt.might.net/articles/a-normalization/ *\
(define atomic? { A --> boolean }
  X  -> true where (number? X)
  X  -> true where (symbol? X)
  X  -> true where (string? X)
  X  -> true where (boolean? X)
  X  -> true where (variable? X)
  _  -> false)

(define normalize-term { klambda --> klambda } Exp -> (normalize Exp (function id)))

(define normalize { klambda --> (klambda --> klambda) --> klambda }
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

(define normalize-name { klambda --> (klambda --> klambda) --> klambda }
  E K -> (normalize E (/. Aexp
           (if (atomic? Aexp) (K Aexp)
             (let T (newvar)
               [let T Aexp (K T)])))))

(define normalize-names { klambda --> (klambda --> klambda) --> klambda }
  Exps K -> (if (empty? Exps)
              (K [])
              (normalize-name (hd Exps) (/. T
                (normalize-names (tl Exps) (/. Ts
                  (K [T | Ts])))))))

(define map-debruijn { (list symbol) --> (list klambda) --> (list klambda) }
  Scope []      -> []
  Scope [H | T] -> [(debruijn Scope H) | (map-debruijn Scope T)])

(define debruijn { (list symbol) --> klambda --> klambda }
  Scope [let X Y Z]  -> [let (debruijn Scope Y) (debruijn [X | Scope] Z)]
  Scope [lambda X Y] -> [lambda (debruijn [X | Scope] Y)]
  Scope [if X Y Z]   -> [if (debruijn Scope X) (debruijn Scope Y) (debruijn Scope Z)]
  Scope [%% X Y]     <- (if (primitive? X) [X (debruijn Scope Y)] (fail)) where (symbol? X)
  Scope [%% X | Y]   <- (if (primitive? X) [X | (map-debruijn Scope Y)] (fail)) where (symbol? X)
  Scope [X Y]        <- (if (not (element? X Scope)) [[function X] (debruijn Scope Y)] (fail)) where (symbol? X)
  Scope [X | Y]      <- (if (not (element? X Scope)) [[function X] | (map-debruijn Scope Y)] (fail)) where (symbol? X)
  Scope [X Y]        -> [(debruijn Scope X) (debruijn Scope Y)]
  Scope [X | Y]      -> [(debruijn Scope X) | (map-debruijn Scope Y)]
  Scope X            <- (if (element? X Scope) [lookup (index X Scope)] (fail)) where (variable? X)
  Scope X            <- (if (and (not (variable? X)) (symbol? X) (not (element? X Scope))) [symbol X] (fail))
  Scope X            -> X)