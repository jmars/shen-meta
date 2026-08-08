(tc -)
(load "shen/util.shen")

\* shen-kl-helpers.shen — all helper functions for the shen->kl compiler.
   Kept separate from shen->kl.shen so the host compiler can
   compile these at full speed before shen->kl is redefined. *\

\* my-length: local length. *\

(define my-length
  [] -> 0
  [_ | Rest] -> (+ 1 (my-length Rest)))

\* constructor?: only 'cons' is supported. *\

(define constructor?
  X -> (= X cons))

\* op-test / op1 / op2: constructor type-tests and accessors. *\

(define op-test cons -> cons?)
(define op1 cons -> hd)
(define op2 cons -> tl)

\* beta-substitute: replace Var with Expr in Body, respecting lambda/let
   shadowing.  Ported from shen.beta (core.kl:105).  Uses explicit conditionals
   (no where guards) to keep host compilation fast. *\

(define beta-substitute
  Var Expr Body -> (if (= Var Body) Expr
                        (if (cons? Body) (beta-substitute-cons Var Expr Body) Body)))

(define beta-substitute-cons
  Var Expr [lambda Arg B] -> (if (= Var Arg) [lambda Arg B]
                                  [lambda Arg (beta-substitute Var Expr B)])
  Var Expr [let V E B] -> (if (= Var V) [let V E B]
                              [let V (beta-substitute Var Expr E) (beta-substitute Var Expr B)])
  Var Expr [H | T] -> [(beta-substitute Var Expr H) | (beta-substitute-list Var Expr T)]
  Var Expr X -> X)

(define beta-substitute-list
  Var Expr [] -> []
  Var Expr [H | T] -> [(beta-substitute Var Expr H) | (beta-substitute-list Var Expr T)]
  Var Expr X -> (beta-substitute Var Expr X))

\* alpha-convert: rename lambda/let bound variables to fresh gensyms. *\

(define alpha-convert
  [lambda Arg Body] -> (let New (gensym (protect Z))
                          (let NewBody (beta-substitute Arg New Body)
                            [lambda New (alpha-convert NewBody)]))
  [let V E B] -> (let New (gensym (protect W))
                   (let NewBody (beta-substitute V New B)
                     [let New (alpha-convert E) (alpha-convert NewBody)]))
  [H | T] -> [(alpha-convert H) | (alpha-convert T)]
  X -> X)

\* apply-subs: apply a list of [Var Expr] substitutions to a body expression. *\

(define apply-subs
  [] Body -> Body
  [[Var Expr] | Rest] Body -> (apply-subs Rest (beta-substitute Var Expr Body)))

\* generate-params: produce N fresh gensym param names. *\

(define generate-params
  0 -> []
  N -> [(gensym (protect V)) | (generate-params (- N 1))])

\* ===== Pattern Compiler ===== *\

\* cons-constructor-pattern?: check if Pat is [cons P1 P2] (3-element list). *\

(define cons-constructor-pattern?
  Pat -> (if (cons? Pat)
             (if (constructor? (hd Pat))
                 (if (cons? (tl Pat))
                     (if (cons? (tl (tl Pat)))
                         (= [] (tl (tl (tl Pat))))
                         false)
                     false)
                 false)
             false))

\* compile-pattern: compile a single pattern against a slot expression.
   Returns [Tests Subs] — both lists (may be empty). *\

(define compile-pattern
  Pat Slot -> [[] [[Pat Slot]]] where (variable? Pat)
  Pat Slot -> [[] []] where (= Pat (intern "_"))
  [] Slot -> [[[= Slot []]] []]
  Pat Slot -> (compile-constructor-pattern Pat Slot)
              where (cons-constructor-pattern? Pat)
  Pat Slot -> (compile-list-pattern Pat Slot) where (cons? Pat)
  Pat Slot -> [[[= Slot Pat]] []])

\* compile-constructor-pattern: handle [cons P1 P2]. *\

(define compile-constructor-pattern
  [cons P1 P2] Slot -> (let Result1 (compile-pattern P1 [hd Slot])
                          (let Result2 (compile-pattern P2 [tl Slot])
                            [(append [[cons? Slot]]
                                     (append (hd Result1) (hd Result2)))
                             (append (hd (tl Result1)) (hd (tl Result2)))]))
  _ _ -> (simple-error "compile-constructor-pattern: not a constructor pattern"))

\* compile-list-pattern: handle proper and improper list patterns. *\

(define compile-list-pattern
  Pat Slot -> (let H (hd Pat)
                (let T (tl Pat)
                  (let ResultH (compile-pattern H [hd Slot])
                    (let ResultT (compile-pattern T [tl Slot])
                      [(append [[cons? Slot]]
                               (append (hd ResultH) (hd ResultT)))
                       (append (hd (tl ResultH)) (hd (tl ResultT)))])))))

\* compile-patterns: walk patterns and params, accumulating tests and subs. *\

(define compile-patterns
  [] [] Tests Subs -> [Tests Subs]
  [Pat | PRest] [Param | ParamRest] Tests Subs ->
    (let Result (compile-pattern Pat Param)
      (compile-patterns PRest ParamRest
                        (append Tests (hd Result))
                        (append Subs (hd (tl Result)))))
  _ _ _ _ -> (simple-error "compile-patterns: pattern/param count mismatch"))

\* rectify-test: convert a list of tests into a single boolean expression. *\

(define rectify-test
  [] -> true
  [T] -> T
  [T1 | Rest] -> [and T1 (rectify-test Rest)])

\* fail-form?: true if X is either the symbol fail or the list [fail]. *\

(define fail-form?
  X -> (if (= X (intern "fail")) true
          (if (cons? X)
              (if (= (hd X) (intern "fail"))
                  (empty? (tl X))
                  false)
              false)))

\* ===== Body Rewriter (consifies list literals) ===== *\

(define shen->kl-body
  X -> (if (cons? X) (shen->kl-form X) X))

(define shen->kl-form
  [lambda Arg Body]   -> [lambda Arg (shen->kl-body Body)]
  [let V E B]         -> [let V (shen->kl-body E) (shen->kl-body B)]
  [protect X]         -> (shen->kl-body X)
  [/. Arg Body]       -> (shen->kl-shorthand Arg Body)
  []                  -> []
  X                   -> (if (cons? X) (shen-kl-expr X) X))

(define shen-kl-expr
  [] -> []
  [H | R] -> (if (shen-kl-app-head? H)
                 [(shen->kl-body H) | (shen-kl-app-args R)]
                 (let Split (shen-kl-split-tail R)
                   [cons (shen->kl-body H) (shen-kl-build-tail Split)]))
  X -> X)

(define shen-kl-app-head?
  H -> (if (symbol? H)
           (let S (str H)
             (if (= S "") false
                 (let Ch (string->n (pos S 0))
                   (not (and (>= Ch 65) (<= Ch 90))))))
           false))

(define shen-kl-var-head?
  H -> (if (symbol? H)
           (let S (str H)
             (if (= S "") false
                 (let Ch (string->n (pos S 0))
                   (and (>= Ch 65) (<= Ch 90)))))
           false))

(define shen-kl-split-tail
  [] -> [[] []]
  [X | R] -> (if (shen-kl-var-head? X)
                 (let Inner (shen-kl-split-tail R)
                   [[X | (hd Inner)] | (tl Inner)])
                 [[] [X | R]])
  X -> [[] X])

(define shen-kl-build-tail
  [[] Suffix] -> (shen-kl-expr Suffix)
  [[H | R] Suffix] -> [cons (shen->kl-body H) (shen-kl-build-tail [R Suffix])])

(define shen-kl-app-args
  [] -> []
  [X | R] -> [(shen->kl-body X) | (shen-kl-app-args R)]
  X -> (shen->kl-body X))

(define shen->kl-shorthand
  Arg Body -> (if (cons? Arg)
                  (shen->kl-shorthand-args Arg Body)
                  [lambda Arg (shen->kl-body Body)]))

(define shen->kl-shorthand-args
  [A]       Body -> [lambda A (shen->kl-body Body)]
  [A | Rest] Body -> [lambda A (shen->kl-shorthand-args Rest Body)])

\* cond-form: wrap compiled clauses in cond, or unwrap single true clause.
   Each compiled clause is (@p [Test Body] Guarded) — a cons pair.
   For unguarded-only functions, extracts [Test Body] and uses cond. *\

(define cond-form
  Compiled -> (let Clauses (map-hd Compiled)
                (if (and (cons? Clauses) (empty? (tl Clauses))
                         (= (hd (hd Clauses)) true))
                    (hd (tl (hd Clauses)))
                    [cond | Clauses])))

\* map-hd: extract the hd (the [Test Body]) from each (@p [Test Body] _) tuple. *\

(define map-hd
  [] -> []
  [C | Rest] -> [(fst C) | (map-hd Rest)])

\* has-guarded?: true if any compiled clause uses <-.
   Each compiled clause is (@p [Test Body] Guarded); Guarded = snd. *\

(define has-guarded?
  [] -> false
  [C | Rest] -> (if (snd C) true (has-guarded? Rest)))

\* build-guarded-dispatch: build right-nested if tree from compiled clauses.
   Each clause is (@p [Test Body] Guarded); fst=[Test Body], snd=Guarded.
   For -> (Guarded=false): (if Test Body REST).
   For <- (Guarded=true): (if Test (let TMP Body
     (if (= TMP fail) REST TMP)) REST).
   Base case: simple-error. *\

(define build-guarded-dispatch
  [] Name -> [simple-error (cn "no matching clause: " (str Name))]
  [C | Rest] Name ->
    (let Test (hd (fst C))
      (let Body (hd (tl (fst C)))
        (let Guarded (snd C)
          (let DispatchRest (build-guarded-dispatch Rest Name)
            (if Guarded
                (let TmpVar (newvar)
                  [if Test
                       [let TmpVar Body
                         [if [= TmpVar fail] DispatchRest TmpVar]]
                       DispatchRest])
                [if Test Body DispatchRest]))))))

\* compile-clause: compile one clause's patterns + body + guard into
   (@p [Test Body] Guarded).
   For a guarded (<-) clause whose body is (if Cond RHS (fail)), fold the
   condition into the test (eliminating fail, which throws on the C VM) and
   emit RHS as the body.  Guarded is retained so the caller can choose the
   dispatch form, but a folded clause is emitted with the condition already
   in the test. *\

(define compile-clause
  C Params -> (let Pats (fst C)
                (let BodyGuard (snd C)
                  (let Body (fst BodyGuard)
                    (let GuardGuarded (snd BodyGuard)
                      (let Guard (fst GuardGuarded)
                        (let Guarded (snd GuardGuarded)
                          (let AlphaBody (alpha-convert Body)
                            (let Compiled (compile-patterns Pats Params [] [])
                              (let Tests (hd Compiled)
                                (let Subs (hd (tl Compiled))
                                  (let SubBody (apply-subs Subs AlphaBody)
                                    (let GuardTest (if (empty? Guard)
                                                      []
                                                      [(apply-subs Subs (shen->kl-body Guard))])
                                      (let Folded (fold-guard-body Guarded SubBody Subs)
                                        (let KLBody (shen->kl-body (tl Folded))
                                          (let AllTests (append Tests (append GuardTest (hd Folded)))
                                            (@p [(rectify-test AllTests) KLBody] Guarded)))))))))))))))))

\* fold-guard-body: for a guarded (<-) clause, if the body is (if Cond RHS (fail)),
   return [ExtraTest RHS] so the condition is folded into the test and the body
   becomes RHS (fail eliminated).  Otherwise return [[] Body] unchanged.
   ExtraTest may be [] or [Cond-substituted]. *\

(define fold-guard-body
  Guarded Body Subs ->
    (if (and Guarded (cons? Body)
             (= (hd Body) (intern "if"))
             (cons? (tl Body)) (cons? (tl (tl Body))) (cons? (tl (tl (tl Body))))
             (fail-form? (hd (tl (tl (tl Body)))))
             (empty? (tl (tl (tl (tl Body))))))
        [[(apply-subs Subs (hd (tl Body)))] | (apply-subs Subs (hd (tl (tl Body))))]
        [[] | Body]))

\* compile-clauses: compile each clause to an (@p [Test Body] Guarded) pair. *\

(define compile-clauses
  [] Params -> []
  [C | Rest] Params -> [(compile-clause C Params) | (compile-clauses Rest Params)])

\* ===== Clause Parser =====
   The reader produces a FLAT rule list for a multi-clause define, e.g.
   (define lookup {sig} 0 [X | _] -> X  X [_ | Z] -> ...  _ _ -> ...)
   parses (after sig strip) to:
     [0 [X|_] -> X X [_|Z] -> ... _ _ -> (simple-error ...)]
   This is a flat list: patterns of each clause, then ->, then a single body
   expression (the reader nests it), then the next clause's patterns, etc.

   We parse ARITY-DRIVEN, mirroring the real Shen <rules>/<rule> parser:
   the FIRST clause establishes the arity (= number of patterns before the
   first top-level ->), and every subsequent clause has exactly that many
   patterns, then ->, then one body, then an optional where guard. *\

\* kl-take / kl-drop: list helpers (arity-driven clause splitting).
   Named kl-* to avoid clashing with reserved Shen words. *\

(define kl-take
  0 _ -> []
  N [H | Rest] -> [H | (kl-take (- N 1) Rest)]
  N [] -> (simple-error "kl-take: not enough elements"))

(define kl-drop
  0 L -> L
  N [_ | Rest] -> (kl-drop (- N 1) Rest)
  N [] -> (simple-error "kl-drop: not enough elements"))

\* rule-arrow?: true if X is -> or <- (clause separators). *\

(define rule-arrow?
  X -> (if (= X (intern "->")) true
         (if (= X (intern "<-")) true false)))

\* split-at-top-arrow: split a flat rule list at the FIRST -> or <- (top level).
   Returns [Pats [Sep AfterArrow]] — Pats is the first clause's patterns,
   Sep is the separator symbol. *\

(define split-at-top-arrow
  [] -> (simple-error "no arrow in define")
  [H | Rest] -> (if (rule-arrow? H)
                    [[] [H | Rest]]
                    (let SplitRest (split-at-top-arrow Rest)
                      [[H | (hd SplitRest)] | (tl SplitRest)])))

\* parse-clause: given a clause's patterns, the separator (-> or <-), and
   the list AFTER it, return
   [Clause Remainder] where Clause = (@p Pats (@p Body (@p Guard Guarded?)))
   and Remainder is the flat list of subsequent clauses.  Guarded? is true
   for <-, false for ->. *\

(define parse-clause
  Pats Sep AfterArrow ->
    (if (empty? AfterArrow)
        (simple-error "malformed clause: no body after arrow")
        (let Body (hd AfterArrow)
          (let AfterBody (tl AfterArrow)
            (if (and (cons? AfterBody) (= (hd AfterBody) (intern "where")))
                (let GuardList (tl AfterBody)
                  (if (empty? GuardList)
                      (simple-error "malformed clause: where with no guard")
                      [(@p Pats (@p Body (@p (hd GuardList) (= Sep (intern "<-")))))
                       | (tl GuardList)]))
                [(@p Pats (@p Body (@p [] (= Sep (intern "<-"))))) | AfterBody])))))

\* parse-clauses-arity: parse clauses after the first, each with `Arity`
   patterns.  Returns a list of (@p Pats (@p Body (@p Guard Guarded?))) clauses. *\

(define parse-clauses-arity
  [] Arity -> []
  L Arity ->
    (let Pats (kl-take Arity L)
      (let AfterPats (kl-drop Arity L)
        (if (and (cons? AfterPats) (rule-arrow? (hd AfterPats)))
            (let Sep (hd AfterPats)
              (let Result (parse-clause Pats Sep (tl AfterPats))
                (let Clause (hd Result)
                  (let Remainder (tl Result)
                    [Clause | (parse-clauses-arity Remainder Arity)]))))
            (simple-error "malformed define: expected -> or <- after patterns")))))

\* parse-clauses: parse a flat rule list into a list of clauses.  The first
   clause fixes the arity; the rest are parsed with that arity. *\

(define parse-clauses
  [] -> []
  L ->
    (let Split (split-at-top-arrow L)
      (let FirstPats (hd Split)
        (let SepAfter (hd (tl Split))
          (let Sep (hd SepAfter)
            (let AfterArrow (tl SepAfter)
              (let Arity (my-length FirstPats)
                (let Result (parse-clause FirstPats Sep AfterArrow)
                  (let FirstClause (hd Result)
                    (let Remainder (tl Result)
                      [FirstClause | (parse-clauses-arity Remainder Arity)]))))))))))

\* check-arity: all clauses must have the same number of patterns. *\

(define check-arity
  Name Clauses -> (let Arity (my-length (fst (hd Clauses)))
                    (check-arity-h Name Arity (tl Clauses))))

(define check-arity-h
  Name Arity [] -> Arity
  Name Arity [C | Rest] -> (if (= Arity (my-length (fst C)))
                               (check-arity-h Name Arity Rest)
                               (simple-error (cn "arity error in " (str Name)))))

\* compile-define-h: parse clauses, check arity, compile patterns, emit defun.
   Uses nested-if dispatch for functions with <- clauses, cond otherwise. *\

(define compile-define-h
  Name Rules -> (let Clauses (parse-clauses Rules)
                  (let Arity (check-arity Name Clauses)
                    (let Params (generate-params Arity)
                      (let Compiled (compile-clauses Clauses Params)
                        [defun Name Params (cond-form Compiled)])))))

\* ===== Signature stripping ===== *\

(define strip-sig
  [First | Rest] -> (if (type-sig? First) Rest [First | Rest])
  _              -> [])

(define type-sig?
  L -> (if (cons? L)
           (if (has-sig-arrow L) true false)
           false))

(define has-sig-arrow
  []     -> false
  [X | R] -> (if (= X (intern "-->")) true (has-sig-arrow R))
  _      -> false)

(define has-rule-arrow
  []     -> false
  [X | R] -> (if (rule-arrow? X) true (has-rule-arrow R))
  _      -> false)

(define compile-define
  Name Rules -> (compile-define-h Name (strip-sig Rules)))

\* ===== Extended .shen reader =====
   The simple reader in load.shen (read-file-raw / parse-exprs) is used for
   .kl files, where { } [ ] | are ordinary ATOMS (e.g. (= { (hd V1264)) in
   shen.typetable).  That reader CANNOT parse .shen source, where
   { A --> B } is a type signature (must be grouped into ONE element so
   strip-sig can remove it) and [X | Rest] / [a b | (f x)] are list syntax.

   This separate reader (shen-read-file / shen-parse-*) handles .shen source:
     - { } delimit a grouped type signature (list with braces retained)
     - [ ] delimit lists, | marks an improper/dotted cdr (only inside [ ])
     - block comments and line comments are skipped
   It is used by shen->kl (via shen->kl-forms) to read .shen files.  The
   .kl reader in load.shen is left untouched. *\

(define shen-skip-block-comment
  Str Pos Len ->
  (if (>= Pos Len)
      (simple-error "unterminated block comment")
      (let Ch (pos Str Pos)
        (if (= Ch (n->string 42))
            (let NextPos (+ Pos 1)
              (if (>= NextPos Len)
                  (simple-error "unterminated block comment")
                  (if (= (pos Str NextPos) (n->string 92))
                      (shen-skip-ws Str (+ NextPos 1) Len)
                      (shen-skip-block-comment Str (+ Pos 1) Len))))
            (shen-skip-block-comment Str (+ Pos 1) Len)))))

(define shen-skip-ws
  Str Pos Len ->
  (if (>= Pos Len)
      Pos
      (let Ch (pos Str Pos)
        (if (ws-ch? Ch)
            (shen-skip-ws Str (+ Pos 1) Len)
            (if (= Ch (n->string 92))
                (let NextPos (+ Pos 1)
                  (if (>= NextPos Len)
                      Pos
                      (let Ch2 (pos Str NextPos)
                        (if (= Ch2 (n->string 92))
                            (skip-comment Str (+ Pos 2) Len)
                            (if (= Ch2 (n->string 42))
                                (shen-skip-block-comment Str (+ NextPos 1) Len)
                                (shen-skip-ws Str NextPos Len))))))
                Pos)))))

(define shen-read-atom-chars
  Str Pos Acc Len ->
  (if (>= Pos Len)
      [Acc Pos]
      (let Ch (pos Str Pos)
        (if (or (ws-ch? Ch)
                (= Ch "(")
                (= Ch ")")
                (= Ch (n->string 91))
                (= Ch (n->string 93))
                (= Ch (n->string 123))
                (= Ch (n->string 125))
                (= Ch (n->string 124))
                (= Ch (n->string 34))
                (= Ch (n->string 92)))
            [Acc Pos]
            (shen-read-atom-chars Str (+ Pos 1) [Ch | Acc] Len)))))

(define shen-parse-atom
  Str Pos Len ->
  (let Pair (shen-read-atom-chars Str Pos [] Len)
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

\* shen-parse-list-tail: continue parsing a list after its first element.
   Close is the closing char for the enclosing opener. Dotted is true only
   inside [ ... ], where | marks the start of the cdr (improper list). *\

(define shen-parse-list-tail
  Str Pos Close Dotted Len ->
  (let P (shen-skip-ws Str Pos Len)
    (if (>= P Len)
        (simple-error "unterminated list")
        (if (= (pos Str P) Close)
            [[] (+ P 1)]
            (if (and Dotted (= (pos Str P) (n->string 124)))
                (let Pair1 (shen-parse-expr Str (+ P 1) Len)
                  (let Cdr (hd Pair1)
                    (let After (hd (tl Pair1))
                      (if (= (pos Str After) Close)
                          [Cdr (+ After 1)]
                          (simple-error "unterminated dotted pair")))))
                (let Pair1 (shen-parse-expr Str P Len)
                  (let First (hd Pair1)
                    (let AfterFirst (hd (tl Pair1))
                      (let Pair2 (shen-parse-list-tail Str AfterFirst Close Dotted Len)
                        (let Rest (hd Pair2)
                          (let AfterRest (hd (tl Pair2))
                            (if Dotted
                                [[cons First Rest] AfterRest]
                                [[First | Rest] AfterRest]))))))))))))

(define shen-parse-list
  Str Pos Close Dotted Len ->
  (let P (shen-skip-ws Str Pos Len)
    (if (>= P Len)
        (simple-error "unterminated list")
        (if (= (pos Str P) Close)
            [[] (+ P 1)]
            (if (and Dotted (= (pos Str P) (n->string 124)))
                (let Pair1 (shen-parse-expr Str (+ P 1) Len)
                  (let Cdr (hd Pair1)
                    (let After (hd (tl Pair1))
                      (if (= (pos Str After) Close)
                          (if Dotted
                              [[cons [] Cdr] (+ After 1)]
                              [[[] | Cdr] (+ After 1)])
                          (simple-error "unterminated dotted pair")))))
                (let Pair1 (shen-parse-expr Str P Len)
                  (let First (hd Pair1)
                    (let AfterFirst (hd (tl Pair1))
                      (let Pair2 (shen-parse-list-tail Str AfterFirst Close Dotted Len)
                        (let Rest (hd Pair2)
                          (let AfterRest (hd (tl Pair2))
                            (if Dotted
                                [[cons First Rest] AfterRest]
                                [[First | Rest] AfterRest]))))))))))))

\* shen-parse-string: shared with the .kl reader via parse-string. *\

\* shen-parse-sig: parse a { ... } type signature.  Real Shen groups it as a
   single sublist WITH the braces retained as atoms: { A --> B } reads as
   ({ A --> B }) (head {, tail ... }), which is what shen.typetable /
   shen.find-arities check for (= { (hd ...)).  Returns [Sig NextPos]. *\

(define shen-parse-sig
  Str Pos Len ->
  (let P (shen-skip-ws Str Pos Len)
    (if (>= P Len)
        (simple-error "unterminated type signature")
        (if (= (pos Str P) (n->string 125))
            [[(intern "{") (intern "}")] (+ P 1)]
            (let Pair1 (shen-parse-expr Str P Len)
              (let First (hd Pair1)
                (let AfterFirst (hd (tl Pair1))
                  (let Pair2 (shen-parse-sig-tail Str AfterFirst Len)
                    (let Rest (hd Pair2)
                      (let AfterRest (hd (tl Pair2))
                        [[(intern "{") First | Rest] AfterRest]))))))))))

(define shen-parse-sig-tail
  Str Pos Len ->
  (let P (shen-skip-ws Str Pos Len)
    (if (>= P Len)
        (simple-error "unterminated type signature")
        (if (= (pos Str P) (n->string 125))
            [[(intern "}")] (+ P 1)]
            (let Pair1 (shen-parse-expr Str P Len)
              (let First (hd Pair1)
                (let AfterFirst (hd (tl Pair1))
                  (let Pair2 (shen-parse-sig-tail Str AfterFirst Len)
                    (let RestContent (hd Pair2)
                      (let AfterRest (hd (tl Pair2))
                        [[First | RestContent] AfterRest]))))))))))

\* shen-parse-expr: parse one s-expression, returning [Expr NextPos].  { ... }
   groups a type signature via shen-parse-sig (braces retained). *\

(define shen-parse-expr
  Str Pos Len ->
  (let P (shen-skip-ws Str Pos Len)
    (if (>= P Len)
        (simple-error "unexpected end of input")
        (let Ch (pos Str P)
          (if (= Ch "(")
              (shen-parse-list Str (+ P 1) ")" false Len)
              (if (= Ch (n->string 91))
                  (shen-parse-list Str (+ P 1) (n->string 93) true Len)
                  (if (= Ch (n->string 123))
                      (shen-parse-sig Str (+ P 1) Len)
                      (if (or (= Ch ")")
                              (= Ch (n->string 93))
                              (= Ch (n->string 125)))
                          (simple-error (cn "unexpected " Ch))
                          (if (= Ch (n->string 34))
                              (parse-string Str (+ P 1) Len)
                              (shen-parse-atom Str P Len))))))))))

(define shen-parse-exprs
  Str Pos Len ->
  (let P (shen-skip-ws Str Pos Len)
    (if (>= P Len)
        [[] P]
        (let Pair1 (shen-parse-expr Str P Len)
          (let Expr (hd Pair1)
            (let NewPos (hd (tl Pair1))
              (let Pair2 (shen-parse-exprs Str NewPos Len)
                (let Rest (hd Pair2)
                  (let FinalPos (hd (tl Pair2))
                    [[Expr | Rest] FinalPos])))))))))

(define shen-read-file
  Path -> (let Str (read-file-as-string Path)
            (let Len (strlen Str)
              (hd (shen-parse-exprs Str 0 Len)))))
