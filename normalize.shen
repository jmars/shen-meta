\* https://github.com/Shen-Language/wiki/wiki/KLambda#equivalent-forms *\
(define map-kmacros
  []      -> []
  [H | T] -> [(kmacros H) | (map-kmacros T)])

(define kmacros
  [freeze X]          -> [lambda (newvar) (kmacros X)]
  [thaw X]            -> [(kmacros X) 0]
  [and X Y]           -> (kmacros [if (kmacros X) (kmacros Y) false])
  [or X Y]            -> (kmacros [if (kmacros X) true (kmacros Y)])
  [cond [X Y] | Rest] -> (kmacros [if (kmacros X) (kmacros Y) (kmacros [cond | Rest])])
  [cond]              -> [simple-error "No condition was true"]
  [trap-error B E]    -> [trap-error (kmacros [lambda (newvar) (kmacros B)]) (kmacros E)]
  [if true X Y]       -> (kmacros X)
  [if false X Y]      -> (kmacros Y)
  [do X]              -> (kmacros X)
  [do X | Y]          -> (kmacros [let (newvar) (kmacros X) (kmacros [do | Y])])
  [X Y]               -> [(kmacros X) (kmacros Y)]
  [X | Y]             -> [(kmacros X) | (map-kmacros Y)]
  []                  -> [%% emptylist 0]
  X                   -> X)

\* http://matt.might.net/articles/a-normalization/ *\
(define atomic?
  X  -> true where (number? X)
  X  -> true where (symbol? X)
  X  -> true where (string? X)
  X  -> true where (boolean? X)
  X  -> true where (variable? X)
  _  -> false)

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