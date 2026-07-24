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
  [cur C1 | C]   N Acc -> (label-positions C (+ N (instr-count C1)) Acc)  \* cur itself takes no slot *\
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

\* Convert native representation to csexp string *\
\* The instruction stream is flat, e.g. [pushmark number 2 push ...].
   Process it instruction-by-instruction rather than element-by-element.
   Each instruction becomes an s-expression (op ARGS). *\
(define nat->csexp { klambda --> string }
  [] -> "()"
  Code -> (cn "(" (csexp-body Code)))

(define csexp-body { klambda --> string }
  [] -> ")"
  [appterm | C]  -> (cn "appterm" (csexp-body C))
  [apply | C]    -> (cn "apply" (csexp-body C))
  [push | C]     -> (cn "push" (csexp-body C))
  [pushmark | C] -> (cn "pushmark" (csexp-body C))
  [grab | C]     -> (cn "grab" (csexp-body C))
  [return | C]   -> (cn "return" (csexp-body C))
  [letz | C]     -> (cn "let" (csexp-body C))
  [endlet | C]   -> (cn "endlet" (csexp-body C))
  [access N | C]  -> (cn (cn (cn "(access " (str N)) ")") (csexp-body C))  where (number? N)
  [global G | C]  -> (cn (cn (cn "(global " (str G)) ")") (csexp-body C))  where (symbol? G)
  [jmpf N | C]    -> (cn (cn (cn "(jmpf " (str N)) ")") (csexp-body C))    where (number? N)
  [jmp N | C]     -> (cn (cn (cn "(jmp " (str N)) ")") (csexp-body C))     where (number? N)
  [cur C1 | C]    -> (cn (cn (cn "(cur " (nat->csexp C1)) ")") (csexp-body C))
  [number N | C]  -> (cn (cn (cn "(number " (str N)) ")") (csexp-body C))  where (number? N)
  [string Ss | C] -> (cn (cn (cn "(string " (csexp-str Ss)) ")") (csexp-body C)) where (string? Ss)
  [symbol Ss | C] -> (cn (cn (cn "(symbol " (str Ss)) ")") (csexp-body C)) where (symbol? Ss)
  [boolean B | C] -> (cn (cn (cn "(boolean " (str B)) ")") (csexp-body C)) where (boolean? B)
  [prim P | C]    -> (cn (cn (cn "(prim " (str P)) ")") (csexp-body C))    where (symbol? P)
  [Op | _]        -> (simple-error (cn "compile: nat->csexp bad " (str Op))))

(define csexp-str { string --> string }
  S -> (cn (cn (str (strlen S)) ":S") S))

(define zinc->native { zinc-code --> string }
  Code -> (nat->csexp (compile-zinc Code)))
