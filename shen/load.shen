(tc -)
(load "shen/toplevel.shen")

(define interp-load
  File -> (interp-eval-all (read-file File)))

(define interp-load-safe
  File -> (trap-error (interp-eval-all (read-file File))
                      (/. X X)))

(define interp-load-raw
  File -> (interp-eval-all (read-file-raw File)))

(define interp-eval-all
  [] -> loaded
  [E | Rest] -> (do (interp-eval-safe E) (interp-eval-all Rest)))

(define interp-eval-safe
  E -> (trap-error (interp-eval E) (/. X X)))

(define strlen
  Str -> (strlen-acc Str 0))

(define strlen-acc
  Str N -> (if (string? (trap-error (pos Str N) (/. E 0)))
              (strlen-acc Str (+ N 1))
              N))

(define chars->str
  [] -> ""
  [Ch | Rest] -> (cn Ch (chars->str Rest)))

(define digit-ch?
  Ch -> (let N (string->n Ch)
           (and (>= N 48) (<= N 57))))

(define ws-ch?
  Ch -> (or (= Ch " ")
            (= Ch (n->string 9))
            (= Ch (n->string 10))
            (= Ch (n->string 13))))

(define str->num
  Str -> (str->num-acc Str 0 0))

(define str->num-acc
  Str Pos Acc ->
  (if (>= Pos (strlen Str))
      Acc
      (let D (- (string->n (pos Str Pos)) 48)
        (str->num-acc Str (+ Pos 1) (+ (* Acc 10) D)))))

(define parse-num-str
  Str -> (if (= (pos Str 0) "-")
            (- 0 (str->num (tlstr Str)))
            (str->num Str)))

(define skip-comment
  Str Pos Len ->
  (if (>= Pos Len)
      Pos
      (let Ch (pos Str Pos)
        (if (or (= Ch (n->string 10))
                (= Ch (n->string 13)))
            (skip-ws Str (+ Pos 1) Len)
            (skip-comment Str (+ Pos 1) Len)))))

\* skip-ws consumes whitespace and comments. Comment:
   \ \  line comment (backslash backslash ... to end of line) *\
(define skip-ws
  Str Pos Len ->
  (if (>= Pos Len)
      Pos
      (let Ch (pos Str Pos)
        (if (ws-ch? Ch)
            (skip-ws Str (+ Pos 1) Len)
            (if (= Ch (n->string 92))
                (let NextPos (+ Pos 1)
                  (if (>= NextPos Len)
                      Pos
                      (let Ch2 (pos Str NextPos)
                        (if (= Ch2 (n->string 92))
                            (skip-comment Str (+ Pos 2) Len)
                            (skip-ws Str NextPos Len)))))
                Pos)))))

(define parse-string-chars
  Str Pos Acc Len ->
  (if (>= Pos Len)
      (simple-error "unterminated string")
      (let Ch (pos Str Pos)
        (if (= Ch (n->string 34))
            [Acc (+ Pos 1)]
            (parse-string-chars Str (+ Pos 1) [Ch | Acc] Len)))))

(define parse-string
  Str Pos Len ->
  (let Pair (parse-string-chars Str Pos [] Len)
    (let Chars (hd Pair)
      (let FinalPos (hd (tl Pair))
        [(chars->str (reverse Chars)) FinalPos]))))

(define read-atom-chars
  Str Pos Acc Len ->
  (if (>= Pos Len)
      [Acc Pos]
      (let Ch (pos Str Pos)
        (if (or (ws-ch? Ch)
                (= Ch "(")
                (= Ch ")")
                (= Ch (n->string 34))
                (= Ch (n->string 92)))
            [Acc Pos]
            (read-atom-chars Str (+ Pos 1) [Ch | Acc] Len)))))

(define parse-atom
  Str Pos Len ->
  (let Pair (read-atom-chars Str Pos [] Len)
    (let Chars (hd Pair)
      (let FinalPos (hd (tl Pair))
        (let Token (chars->str (reverse Chars))
          (if (= Token "")
              [(intern "") FinalPos]
              (if (or (digit-ch? (pos Token 0))
                      (and (> (strlen Token) 1)
                           (= (pos Token 0) "-")
                           (digit-ch? (pos Token 1))))
                  [(parse-num-str Token) FinalPos]
                  [(intern Token) FinalPos])))))))

(define parse-list-tail
  Str Pos Len ->
  (let P (skip-ws Str Pos Len)
    (if (>= P Len)
        (simple-error "unterminated list")
        (if (= (pos Str P) ")")
            [[] (+ P 1)]
            (let Pair1 (parse-expr Str P Len)
              (let First (hd Pair1)
                (let AfterFirst (hd (tl Pair1))
                  (let Pair2 (parse-list-tail Str AfterFirst Len)
                    (let Rest (hd Pair2)
                      (let AfterRest (hd (tl Pair2))
                        [[First | Rest] AfterRest]))))))))))

(define parse-list
  Str Pos Len ->
  (let P (skip-ws Str Pos Len)
    (if (>= P Len)
        (simple-error "unterminated list")
        (if (= (pos Str P) ")")
            [[] (+ P 1)]
            (let Pair1 (parse-expr Str P Len)
              (let First (hd Pair1)
                (let AfterFirst (hd (tl Pair1))
                  (let Pair2 (parse-list-tail Str AfterFirst Len)
                    (let Rest (hd Pair2)
                      (let AfterRest (hd (tl Pair2))
                        [[First | Rest] AfterRest]))))))))))

(define parse-expr
  Str Pos Len ->
  (let P (skip-ws Str Pos Len)
    (if (>= P Len)
        (simple-error "unexpected end of input")
        (let Ch (pos Str P)
          (if (= Ch "(")
              (parse-list Str (+ P 1) Len)
              (if (= Ch ")")
                  (simple-error "unexpected )")
                  (if (= Ch (n->string 34))
                      (parse-string Str (+ P 1) Len)
                      (parse-atom Str P Len))))))))

(define parse-exprs
  Str Pos Len ->
  (let P (skip-ws Str Pos Len)
    (if (>= P Len)
        [[] P]
        (let Pair1 (parse-expr Str P Len)
          (let Expr (hd Pair1)
            (let NewPos (hd (tl Pair1))
              (let Pair2 (parse-exprs Str NewPos Len)
                (let Rest (hd Pair2)
                  (let FinalPos (hd (tl Pair2))
                    [[Expr | Rest] FinalPos])))))))))

(define read-file-raw
  Path -> (let Str (read-file-as-string Path)
            (let Len (strlen Str)
              (hd (parse-exprs Str 0 Len)))))
