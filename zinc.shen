\* https://caml.inria.fr/pub/papers/xleroy-zinc.pdf *\
(define map-zinc-c
  []      -> []
  [H | T] -> [(zinc-c H) | (map-zinc-c T)])

(define zinc-t
  [lookup X]   -> [access X]
  [function X] -> [global X]
  [lambda X]   -> [grab | (zinc-t X)]
  [let X Y]    -> (append (zinc-c X) [let] (zinc-t Y))
  [if X Y Z]   -> (let F (gensym l) (let E (gensym l) (append (zinc-c X) [jmpf F] (zinc-c Y) [jmp E] [label F] (zinc-c Z) [label E])))
  [symbol X]   -> [symbol X] where (symbol? X)
  [F A]        -> (zinc-c [F A]) where (primitive? F)
  [F | Args]   -> (zinc-c [F | Args]) where (primitive? F)
  [F | Args]   -> (append (fold-append [] (intersperse [push] (map-zinc-c (reverse Args)))) [push] (zinc-c F) [appterm])
  true         -> [boolean true]
  false        -> [boolean false]
  X            -> [number X] where (number? X)
  X            -> [string X] where (string? X)
  []           -> []
  _            -> (simple-error "zinc-t: unknown expression"))

(define zinc-c
  [lookup X]   -> [access X]
  [function X] -> [global X]
  [lambda X]   -> [cur (append (zinc-t X) [return])]
  [let X Y]    -> (append (zinc-c X) [let] (zinc-c Y) [endlet])
  [if X Y Z]   -> (let F (gensym l) (let E (gensym l) (append (zinc-c X) [jmpf F] (zinc-c Y) [jmp E] [label F] (zinc-c Z) [label E])))
  [symbol X]   -> [symbol X] where (symbol? X)
  [F A]        -> (append (zinc-c A) [prim F]) where (primitive? F)
  [F | Args]   -> (append (fold-append [] (intersperse [push] (map-zinc-c (reverse (tl Args))))) [push] (zinc-c (hd Args)) [prim F]) where (primitive? F)
  [F | Args]   -> (append [pushmark] (fold-append [] (intersperse [push] (map-zinc-c (reverse Args)))) [push] (zinc-c F) [apply])
  true         -> [boolean true]
  false        -> [boolean false]
  X            -> [number X] where (number? X)
  X            -> [string X] where (string? X)
  []           -> []
  _            -> (simple-error "zinc-c: unknown expression"))