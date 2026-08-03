(define safe.number?
  N -> true where (%% number? N)
  _ -> false)

(define safe.symbol?
  S -> true where (%% symbol? S)
  _ -> false)

(define safe.string?
  S -> true where (%% string? S)
  _ -> false)

(define safe.boolean?
  B -> true where (%% boolean? B)
  _ -> false)

(define safe.cons?
  C -> true where (%% cons? C)
  _ -> false)

(define safe.simple-error
  S -> (%% simple-error S) where (string? S)
  _ -> (%% simple-error "simple-error: arg must be a string"))

(define safe.get-time
  unix -> (%% get-time unix)
  run  -> (%% get-time run)
  _    -> (simple-error "get-time: arg must be 'unix' or 'run'"))

(define safe.close
  S -> (%% close S) where (%% stream? S)
  _ -> (simple-error "close: argument must be a stream"))

(define safe.read-byte
  S -> (%% read-byte S) where (%% stream? S)
  _ -> (simple-error "read-byte: argument must be a stream"))

(define safe.tl
  [] -> (simple-error "tl: arg cannot be empty list")
  L  -> (%% tl L) where (cons? L)
  _  -> (simple-error "tl: arg cannot be empty list"))

(define safe.hd
  [] -> (simple-error "hd: arg cannot be empty list")
  L  -> (%% hd L) where (cons? L)
  _  -> (simple-error "hd: arg cannot be empty list"))

(define safe.absvector
  N -> (%% absvector N) where (and (number? N) (>= N 0))
  _ -> (simple-error "absvector: arg must be a non-negative number"))

(define safe.n->string
  N -> (%% n->string N) where (number? N)
  _ -> (simple-error "n->string: arg must be a number"))

(define safe.string->n
  S -> (%% string->n S) where (string? S)
  _ -> (simple-error "string->n: arg must be a string"))

(define safe.str
  S -> (%% str S))

(define safe.tlstr
  "" -> (simple-error "tlstr: empty string")
  S  -> (%% tlstr S) where (string? S)
  _  -> (simple-error "tlstr: arg must be a string"))

(define safe.hdstr
  "" -> (simple-error "hdstr: empty string")
  S  -> (%% hdstr S) where (string? S)
  _  -> (simple-error "hdstr: arg must be a string"))

(define safe.value
  S -> (%% value S) where (symbol? S)
  _ -> (simple-error "value: arg must be a symbol"))

(define safe.intern
  "true"  -> true
  "false" -> false
  S       -> (%% intern S) where (string? S)
  _       -> (simple-error "intern: arg must be a string"))

(define safe.error-to-string
  E -> (%% error-to-string E) where (%% error? E)
  _ -> (simple-error "error-to-string: arg must be an error"))

(define safe.trap-error
  T C -> (%% trap-error T C) where (and (%% function? T) (%% function? C))
  _ _ -> (simple-error "trap-error: args must be functions"))

(define safe.=
  A B -> (%% = A B))

(define safe.open-ok
  S -> S where (%% stream? S)
  _ -> (simple-error "open: cannot open file"))

(define safe.open
  P in  -> (safe.open-ok (%% open P in)) where (string? P)
  P out -> (safe.open-ok (%% open P out)) where (string? P)
  _ _   -> (simple-error "open: invalid arguments"))

(define safe.write-byte
  N S -> (%% write-byte N S) where (and (number? N) (%% stream? S))
  _ _ -> (simple-error "write-byte: invalid arguments"))

(define safe.cons
  H T -> (%% cons H T))

(define safe.fst
  C -> (%% fst C) where (cons? C)
  _ -> (simple-error "fst: arg must be a cons"))

(define safe.snd
  C -> (%% snd C) where (cons? C)
  _ -> (simple-error "snd: arg must be a cons"))

(define safe.emptylist
  0 -> (%% emptylist 0)
  _ -> (simple-error "emptylist: arg must be 0"))

(define safe.<-address
  V N -> (%% <-address V N) where (and (absvector? V) (number? N))
  _ _ -> (simple-error "<-address: invalid args"))

(define safe.cn
  S S1 -> (%% cn S S1) where (and (string? S) (string? S1))
  _ _  -> (simple-error "cn: invalid args"))

(define safe.pos
  S N -> (%% pos S N) where (and (string? S) (number? N))
  _ _ -> (simple-error "pos: invalid args"))

(define safe.<=
  N N1 -> (%% <= N N1) where (and (number? N) (number? N1))
  _ _  -> (simple-error "<=: invalid args"))

(define safe.>=
  N N1 -> (%% >= N N1) where (and (number? N) (number? N1))
  _ _  -> (simple-error ">=: invalid args"))

(define safe.<
  N N1 -> (%% < N N1) where (and (number? N) (number? N1))
  _ _  -> (simple-error "<: invalid args"))

(define safe.>
  N N1 -> (%% > N N1) where (and (number? N) (number? N1))
  _ _  -> (simple-error ">: invalid args"))

(define safe.set
  S V -> (%% set S V) where (symbol? S)
  _ _ -> (simple-error "set: first arg must be a symbol"))

(define safe.-
  A B -> (%% - A B) where (and (number? A) (number? B))
  _ _ -> (simple-error "+: invalid args"))

(define safe.*
  A B -> (%% * A B) where (and (number? A) (number? B))
  _ _ -> (simple-error "+: invalid args"))

(define safe./
  A 0 -> (simple-error "division by zero")
  A B -> (%% / A B) where (and (number? A) (number? B))
  _ _ -> (simple-error "+: invalid args"))

(define safe.+
  A B -> (%% + A B) where (and (number? A) (number? B))
  _ _ -> (simple-error "+: invalid args"))

(define safe.address->
  V N A -> (%% address-> V N A) where (and (absvector? V) (number? N))
  _ _ _ -> (simple-error "address->: invalid args"))

(define safe.eval-kl
  A -> (%% eval-kl A))