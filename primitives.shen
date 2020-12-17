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
  L  -> (%% tl L) where (%% cons? L)
  _  -> (simple-error "tl: arg cannot be empty list"))

(define safe.hd
  [] -> (simple-error "hd: arg cannot be empty list")
  L  -> (%% hd L) where (%% cons? L)
  _  -> (simple-error "hd: arg cannot be empty list"))

(define safe.+
  A B -> (%% + A B) where (and (number? A) (number? B))
  _ _ -> (simple-error "+: invalid args"))