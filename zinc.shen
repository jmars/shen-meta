\* https://caml.inria.fr/pub/papers/xleroy-zinc.pdf *\
(define map-zinc-c { klambda --> (list zinc-code) }
  []      -> []
  [H | T] -> [(zinc-c H) | (map-zinc-c T)])

(define zinc-t { klambda --> zinc-code }
  [lookup X]   -> [access X] where (number? X)
  [function X] -> [global X] where (symbol? X)
  [lambda X]   -> [grab | (zinc-t X)]
  [let X Y]    -> (append (zinc-c X) [let] (zinc-t Y))
  [if X Y Z]   -> (let F (gensym l) (let E (gensym l) (append (zinc-c X) [jmpf F] (zinc-c Y) [jmp E] [label F] (zinc-c Z) [label E])))
  [symbol X]   -> [symbol X] where (symbol? X)
  [F A]        <- (if (primitive? F) (zinc-c [F A]) (fail)) where (symbol? F)
  [F | Args]   <- (if (primitive? F) (zinc-c [F | Args]) (fail)) where (symbol? F)
  [F | Args]   -> (append (fold-append [] (intersperse [push] (map-zinc-c (reverse Args)))) [push] (zinc-c F) [appterm])
  X            -> [boolean X] where (boolean? X)
  X            -> [number X] where (number? X)
  X            -> [string X] where (string? X)
  []           -> []
  _            -> (simple-error "zinc-t: unknown expression"))

(define zinc-c { klambda --> zinc-code }
  [lookup X]   -> [access X] where (number? X)
  [function X] -> [global X] where (symbol? X)
  [lambda X]   -> [cur (append (zinc-t X) [return])]
  [let X Y]    -> (append (zinc-c X) [let] (zinc-c Y) [endlet])
  [if X Y Z]   -> (let F (gensym l) (let E (gensym l) (append (zinc-c X) [jmpf F] (zinc-c Y) [jmp E] [label F] (zinc-c Z) [label E])))
  [symbol X]   -> [symbol X] where (symbol? X)
  [F A]        <- (if (primitive? F) (append (zinc-c A) [prim F]) (fail)) where (symbol? F)
  [F | Args]   <- (if (primitive? F) (append (fold-append [] (intersperse [push] (map-zinc-c (reverse (tl Args))))) [push] (zinc-c (hd Args)) [prim F]) (fail)) where (symbol? F)
  [F | Args]   -> (append [pushmark] (fold-append [] (intersperse [push] (map-zinc-c (reverse Args)))) [push] (zinc-c F) [apply])
  X            -> [boolean X] where (boolean? X)
  X            -> [number X] where (number? X)
  X            -> [string X] where (string? X)
  []           -> []
  _            -> (simple-error "zinc-c: unknown expression"))