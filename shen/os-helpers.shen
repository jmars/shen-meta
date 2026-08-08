(tc -)

\* os-helpers.shen — type-safe helpers replacing Shen OS .kl code in the
   reduced meta-interpreter bundle.  All are compiled by our own full-arity
   shen->kl compiler, no partial application or type-unsafe constructs.
   Non-linear patterns avoided: use 'where' guards to check equality
   instead of repeating a variable in the same clause's patterns. *\

(define append
  [] Y -> Y
  [H | T] Y -> [H | (append T Y)])

(define reverse
  L -> (reverse-help L []))

(define reverse-help
  [] Acc -> Acc
  [H | T] Acc -> (reverse-help T [H | Acc]))

(define empty?
  X -> (if (= X []) true false))

(define element?
  _ [] -> false
  X [H | T] -> true where (= X H)
  X [_ | T] -> (element? X T))

(define assoc
  _ [] -> []
  K [[H V] | T] -> [H V] where (= K H)
  K [_ | T] -> (assoc K T))

(tc +)
