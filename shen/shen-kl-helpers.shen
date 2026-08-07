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
  Var Expr [H | T] -> [(beta-substitute Var Expr H) | (beta-substitute-list Var Expr T)])

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

\* cond-form: wrap compiled clauses in cond, or unwrap single true clause. *\

(define cond-form
  [[true Body]] -> Body
  Clauses -> [cond | Clauses])

\* compile-clause: compile one clause's patterns + body + guard into [Test Body]. *\

(define compile-clause
  C Params -> (let Pats (fst C)
                (let BodyGuard (snd C)
                  (let Body (fst BodyGuard)
                    (let Guard (snd BodyGuard)
                      (let AlphaBody (alpha-convert Body)
                        (let Compiled (compile-patterns Pats Params [] [])
                          (let Tests (hd Compiled)
                            (let Subs (hd (tl Compiled))
                              (let SubBody (apply-subs Subs AlphaBody)
                                (let AllTests (if (empty? Guard)
                                                 Tests
                                                 (append Tests [(apply-subs Subs Guard)]))
                                  [(rectify-test AllTests) SubBody])))))))))))

\* compile-clauses: compile each clause to a [Test Body] pair. *\

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

\* split-at-top-arrow: split a flat rule list at the FIRST -> (top level).
   Returns [Pats AfterArrow] — Pats is the first clause's patterns. *\

(define split-at-top-arrow
  [] -> (simple-error "no -> in define")
  [H | Rest] -> (if (= H (intern "->"))
                    [[] Rest]
                    (let SplitRest (split-at-top-arrow Rest)
                      [[H | (hd SplitRest)] | (tl SplitRest)])))

\* parse-clause: given a clause's patterns and the list AFTER its ->, return
   [Clause Remainder] where Clause = (@p Pats (@p Body Guard)) and Remainder
   is the flat list of subsequent clauses.  The body is the single element
   right after ->; a following 'where' introduces the guard. *\

(define parse-clause
  Pats AfterArrow ->
    (if (empty? AfterArrow)
        (simple-error "malformed clause: no body after ->")
        (let Body (hd AfterArrow)
          (let AfterBody (tl AfterArrow)
            (if (and (cons? AfterBody) (= (hd AfterBody) (intern "where")))
                (let GuardList (tl AfterBody)
                  (if (empty? GuardList)
                      (simple-error "malformed clause: where with no guard")
                      (@p (@p Pats (@p Body (hd GuardList))) (tl GuardList))))
                (@p (@p Pats (@p Body [])) AfterBody))))))

\* parse-clauses-arity: parse clauses after the first, each with `Arity`
   patterns.  Returns a list of (@p Pats (@p Body Guard)) clauses. *\

(define parse-clauses-arity
  [] Arity -> []
  L Arity ->
    (let Pats (kl-take Arity L)
      (let AfterPats (kl-drop Arity L)
        (if (and (cons? AfterPats) (= (hd AfterPats) (intern "->")))
            (let Clause (parse-clause Pats (tl AfterPats))
              [(fst Clause) | (parse-clauses-arity (snd Clause) Arity)])
            (simple-error "malformed define: expected -> after patterns")))))

\* parse-clauses: parse a flat rule list into a list of clauses.  The first
   clause fixes the arity; the rest are parsed with that arity. *\

(define parse-clauses
  [] -> []
  L ->
    (let Split (split-at-top-arrow L)
      (let FirstPats (hd Split)
        (let Arity (my-length FirstPats)
          (let FirstClause (parse-clause FirstPats (hd (tl Split)))
            [(fst FirstClause) | (parse-clauses-arity (snd FirstClause) Arity)])))))

\* check-arity: all clauses must have the same number of patterns. *\

(define check-arity
  Name Clauses -> (let Arity (my-length (fst (hd Clauses)))
                    (check-arity-h Name Arity (tl Clauses))))

(define check-arity-h
  Name Arity [] -> Arity
  Name Arity [C | Rest] -> (if (= Arity (my-length (fst C)))
                               (check-arity-h Name Arity Rest)
                               (simple-error (cn "arity error in " (str Name)))))

\* compile-define-h: parse clauses, check arity, compile patterns, emit defun. *\

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
  [X | R] -> (if (= X (intern "->")) true (has-rule-arrow R))
  _      -> false)

(define compile-define
  Name Rules -> (compile-define-h Name (strip-sig Rules)))
