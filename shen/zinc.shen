\* https://caml.inria.fr/pub/papers/xleroy-zinc.pdf *\
(define map-zinc-c { klambda --> (list zinc-code) }
  []      -> []
  [H | T] -> [(zinc-c H) | (map-zinc-c T)])

(define zinc-t { klambda --> zinc-code }
  [lookup X]   -> [access X] where (number? X)
  [function X] -> [global X] where (symbol? X)
  \* Single-element symbol list [K] is a literal value, not a call *\
  [K]          -> [symbol K] where (symbol? K)
  [lambda X]   -> [grab | (zinc-t X)]
  [let X Y]    -> (append (zinc-c X) (append [let] (zinc-t Y)))
  [if X Y Z]   -> (let F (gensym l) (let E (gensym l)
                    (append (append (zinc-c X) (append [jmpf F] (zinc-c Y)))
                            (append [jmp E] (append [label F] (append (zinc-c Z) [label E]))))))
  [symbol X]   -> [symbol X] where (symbol? X)
  [F A]        <- (if (primitive? F) (append (zinc-c A) [prim F]) (fail)) where (symbol? F)
  [F | Args]   <- (if (primitive? F)
                    (append (fold-append [] (map-zinc-c (reverse (tl Args))))
                            (append (zinc-c (hd Args)) [prim F]))
                    (fail)) where (symbol? F)
  [F | Args]   -> (append [pushmark]
                          (append (fold-append [] (map-zinc-c (reverse Args)))
                                  (append (zinc-c F) [appterm])))
  X            -> [boolean X] where (boolean? X)
  X            -> [number X] where (number? X)
  X            -> [string X] where (string? X)
  []           -> []
  _            -> (simple-error "zinc-t: unknown expression"))

(define zinc-c { klambda --> zinc-code }
  [lookup X]   -> [access X] where (number? X)
  [function X] -> [global X] where (symbol? X)
  \* Single-element symbol list [K] is a literal value, not a call *\
  [K]          -> [symbol K] where (symbol? K)
  [lambda X]   -> [cur (append (zinc-t X) [return])]
  [let X Y]    -> (append (zinc-c X) (append [let] (append (zinc-c Y) [endlet])))
  [if X Y Z]   -> (let F (gensym l) (let E (gensym l)
                    (append (append (zinc-c X) (append [jmpf F] (zinc-c Y)))
                            (append [jmp E] (append [label F] (append (zinc-c Z) [label E]))))))
  [symbol X]   -> [symbol X] where (symbol? X)
  [F A]        <- (if (primitive? F) (append (zinc-c A) [prim F]) (fail)) where (symbol? F)
  [F | Args]   <- (if (primitive? F)
                    (append (fold-append [] (map-zinc-c (reverse (tl Args))))
                            (append (zinc-c (hd Args)) [prim F]))
                    (fail)) where (symbol? F)
  [F | Args]   -> (append [pushmark]
                          (append (fold-append [] (map-zinc-c (reverse Args)))
                                  (append (zinc-c F) [apply])))
  X            -> [boolean X] where (boolean? X)
  X            -> [number X] where (number? X)
  X            -> [string X] where (string? X)
  []           -> []
  _            -> (simple-error "zinc-c: unknown expression"))