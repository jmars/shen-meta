(load "util.shen")

\* Count instructions in flat code, skipping labels *\
(define instr-count { zinc-code --> number }
  [cur C1 | C]  -> (+ 1 (instr-count C1) (instr-count C))
  [label _ | C] -> (instr-count C)
  [_ | C]       -> (+ 1 (instr-count C))
  []            -> 0)

\* First pass: build label -> position map *\
(define label-positions { zinc-code --> number --> (list (list symbol number)) --> (list (list symbol number)) }
  [label L | C]  N Acc -> (label-positions C N (cons [L N] Acc))
  [cur C1 | C]   N Acc -> (label-positions C (+ N 1 (instr-count C1)) (label-positions C1 0 Acc))  \* cur takes 1 slot + recurse into body *\
  [_ | C]        N Acc -> (label-positions C (+ 1 N) Acc)
  []             _ Acc -> Acc)

(define resolve-label { symbol --> (list (list symbol number)) --> number }
  L Labels -> (let P (assoc L Labels)
                (if (empty? P)
                    (simple-error (cn "compile: unresolved label - " (str L)))
                    (hd (tl P)))))

\* Second pass: resolve labels, remove label markers, recurse into cur *\
(define resolve-code { zinc-code --> (list (list symbol number)) --> klambda }
  []                _ -> []
  [label _ | C]    Labels -> (resolve-code C Labels)
  [jmpf L | C]     Labels -> [jmpf (resolve-label L Labels) | (resolve-code C Labels)]
  [jmp L | C]      Labels -> [jmp (resolve-label L Labels) | (resolve-code C Labels)]
  [cur C1 | C]     Labels -> [cur (resolve-code C1 Labels) | (resolve-code C Labels)]
  [access N | C]   Labels -> [access N | (resolve-code C Labels)]
  [global G | C]   Labels -> [global G | (resolve-code C Labels)]
  [appterm | C]    Labels -> [appterm | (resolve-code C Labels)]
  [apply | C]      Labels -> [apply | (resolve-code C Labels)]
  [push | C]       Labels -> [push | (resolve-code C Labels)]
  [pushmark | C]   Labels -> [pushmark | (resolve-code C Labels)]
  [grab | C]       Labels -> [grab | (resolve-code C Labels)]
  [return | C]     Labels -> [return | (resolve-code C Labels)]
  [let | C]        Labels -> [letz | (resolve-code C Labels)]
  [endlet | C]     Labels -> [endlet | (resolve-code C Labels)]
  [number N | C]   Labels -> [number N | (resolve-code C Labels)]
  [string Ss | C]  Labels -> [string Ss | (resolve-code C Labels)]
  [symbol Ss | C]  Labels -> [symbol Ss | (resolve-code C Labels)]
  [boolean B | C]  Labels -> [boolean B | (resolve-code C Labels)]
  [prim P | C]     Labels -> [prim P | (resolve-code C Labels)]
  [Op | _]         _      -> (simple-error (cn "compile: unknown op " (str Op))))

(define compile-zinc { zinc-code --> klambda }
  Code -> (let Labels (label-positions Code 0 [])
            (resolve-code Code Labels)))

\* Canonical s-expression encoding *\
\* Atoms: [len:type]value where type = s(symbol), n(number), S(string), b(boolean) *\
\* Lists: (elem1 elem2 ...) *\
(define string-size { string --> number }
  S -> (string-size-h S 0))

(define string-size-h { string --> number --> number }
  S N -> (if (= S "")
            N
            (string-size-h (tlstr S) (+ N 1))))

(define csexp-atom { klambda --> string }
  X -> (cn (cn (cn (cn "[" (str (string-size (str X)))) ":s") "]") (str X)) where (symbol? X)
  X -> (cn (cn (cn (cn "[" (str (string-size (str X)))) ":n") "]") (str X)) where (number? X)
  X -> (cn (cn (cn (cn "[" (str (string-size X))) ":S") "]") X) where (string? X)
  X -> (cn (cn (cn (cn "[" (str (string-size (str X)))) ":b") "]") (str X)) where (boolean? X))

(define csexp-list { (list string) --> string }
  Strs -> (cn (cn "(" (fold-str Strs)) ")"))

(define csexp-str { string --> string }
  S -> (cn (cn (cn (cn "[" (str (string-size S))) ":S") "]") S))

\* Convert compiled klambda to canonical s-expression *\
(define nat->csexp { klambda --> string }
  [] -> "()"
  Code -> (cn "(" (csexp-body Code)))

(define csexp-body { klambda --> string }
  [] -> ")"
  [appterm | C]  -> (cn "t" (csexp-body C))
  [apply | C]    -> (cn "p" (csexp-body C))
  [push | C]     -> (cn "u" (csexp-body C))
  [pushmark | C] -> (cn "m" (csexp-body C))
  [grab | C]     -> (cn "r" (csexp-body C))
  [return | C]   -> (cn "v" (csexp-body C))
  [letz | C]     -> (cn "e" (csexp-body C))
  [endlet | C]   -> (cn "d" (csexp-body C))
  [access N | C]  -> (cn (cn "a" (csexp-atom N)) (csexp-body C)) where (number? N)
  [global G | C]  -> (cn (cn "g" (csexp-atom G)) (csexp-body C)) where (symbol? G)
  [jmpf N | C]    -> (cn (cn "f" (csexp-atom N)) (csexp-body C)) where (number? N)
  [jmp N | C]     -> (cn (cn "j" (csexp-atom N)) (csexp-body C)) where (number? N)
  [cur C1 | C]    -> (cn "c" (cn (nat->csexp C1) (csexp-body C)))
  [number N | C]  -> (cn (cn "n" (csexp-atom N)) (csexp-body C)) where (number? N)
  [string Ss | C] -> (cn (cn "S" (csexp-str Ss)) (csexp-body C)) where (string? Ss)
  [symbol Ss | C] -> (cn (cn "s" (csexp-atom Ss)) (csexp-body C)) where (symbol? Ss)
  [boolean B | C] -> (cn (cn "b" (csexp-atom B)) (csexp-body C)) where (boolean? B)
  [prim P | C]    -> (cn (cn "P" (csexp-atom P)) (csexp-body C)) where (symbol? P)
  [Op | _]        -> (simple-error (cn "compile: nat->csexp bad " (str Op))))

(define zinc->native { zinc-code --> string }
  Code -> (nat->csexp (compile-zinc Code)))
