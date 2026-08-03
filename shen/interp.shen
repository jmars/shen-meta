(load "shen/normalize.shen")
(load "shen/util.shen")
(load "shen/zinc.shen")

\* Global symbol table - association list of [name . closure] pairs *\
(set global-table [])

(define lookup-global { symbol --> zinc-value }
  G -> (let Table (value global-table)
         (let Pair (assoc G Table)
           (if (empty? Pair)
               (simple-error (cn "global not found: " (str G)))
               (hd (tl Pair))))))

\* Reference implementation, this is basically a transliteration
  of the rules in the paper *\
(define lookup { number --> (list zinc-value) --> zinc-value }
  0 [X | _] -> X
  X [_ | Z] -> (lookup (- X 1) Z)
  _ _       -> (simple-error "failed lookup"))

(define interp-jmp { zinc-code --> symbol --> zinc-code }
  [label L | C] L -> C
  [C1 | C] L      -> (interp-jmp C L)
  _ _             -> (simple-error "failed jump"))

(define extract-kl { zinc-value --> klambda }
  [cons]      -> []
  [cons X Y]  -> (cons (extract-kl X) (extract-kl Y))
  [number X]  -> X
  [symbol X]  -> X
  [string X]  -> X
  [boolean X] -> X
  [lambda C E] -> [lambda C E]
  [error X]    -> X
  [absvector X] -> X
  [stream in X] -> X
  [stream out X] -> X
  mark         -> []
  X            -> X)



(define collect-apply-args { (list zinc-value) --> number --> (list zinc-value) }
  \* Hit A0 before mark: A0 is the spurious old-acc from pre-pushmark.
     Second element is the literal symbol mark — skip both, return saved stack. *\
  [V1 V2 | S] N -> [[] S] where (= V2 mark)
  \* At A0 position but N > 64: too many args pushed before mark *\
  [V1 V2 | S] N -> (simple-error "too many args (>64)") where (> N 64)
  \* Collect V1 as an arg, recurse with incremented counter *\
  [V1 | S] N -> (let Result (collect-apply-args S (+ N 1))
                [[V1 | (hd Result)] | (tl Result)])
  \* Empty stack — no mark found, bytecode is malformed *\
  [] _ -> (simple-error "missing pushmark"))

(define interp { zinc-code --> zinc-value --> (list zinc-value) --> (list zinc-value) --> (list zinc-value) --> zinc-value }
  [access N | C] A E S R                                        -> (interp C (lookup N E) E [A | S] R)
  [global G | C] A E S R                                        -> (interp C (lookup-global G) E [A | S] R)
  [jmpf L | C] [boolean false] E S R                            -> (interp (interp-jmp C L) [boolean false] E S R)
  [jmpf L | C] A E S R                                          -> (interp C A E S R)
  [jmp L | C] A E S R                                           -> (interp (interp-jmp C L) A E S R)
  [label L | C] A E S R                                         -> (interp C A E S R)
  \* Apply: collect all args, isolate stack, save caller context *\
  [apply | C] [lambda C1 E1] E S R ->
    (let Collected (collect-apply-args S 0)
      (let Args (hd Collected)
        (let Rest (hd (tl Collected))
          (interp C1 [lambda C1 E1] (append (reverse Args) E1) [] [[C E Rest] | R]))))
  \* Appterm — tail call with return frame: collect all args up to mark *\
  [appterm | C] [lambda C1 E1] E S [[C_call E_call _] | R] ->
    (let Collected (collect-apply-args S 0)
      (let Args (hd Collected)
        (if (empty? Args)
            (simple-error "appterm zero args")
            (let Rest (hd (tl Collected))
              (interp C1 [lambda C1 E1] (append (reverse Args) E1) [] [[C_call E_call Rest] | R])))))
  \* Appterm — tail call at top level: collect all args up to mark *\
  [appterm | C] [lambda C1 E1] E S [] ->
    (let Collected (collect-apply-args S 0)
      (let Args (hd Collected)
        (if (empty? Args)
            (simple-error "appterm zero args")
            (interp C1 [lambda C1 E1] (append (reverse Args) E1) [] []))))
  [push | C] A E S R                                            -> (interp C A E [A | S] R)
  [pushmark | C] A E S R                                        -> (interp C A E [mark | S] R)
  [cur C1 | C] A E S R                                          -> (interp C [lambda C1 E] E [A | S] R)
  \* Grab: with stack isolation, args are already in env. Empty stack = no-op. *\
  [grab | C] A E [] R                                              -> (interp C A E [] R)
  \* Grab with value: bind it (curried partial application fallback) *\
  [grab | C] A E [V | S] R                                      -> (interp C A [V | E] S R)
  \* Return: restore caller's code, env, and saved stack. Return value in A. *\
  [return | C] A E S [[C_caller E_caller S_saved] | R] ->
    (interp C_caller A E_caller S_saved R)
  \* Return at top level: just return the accumulator *\
  [return | C] A E S [] -> A
  [let | C] A E S R                                             -> (interp C A [A | E] S R)
  [endlet | C] A [V | E] S R                                    -> (interp C A E S R)
  [number N | C] A E S R                                        -> (interp C [number N] E [A | S] R)
  [string Ss | C] A E S R                                       -> (interp C [string Ss] E [A | S] R)
  [symbol Ss | C] A E S R                                       -> (interp C [symbol Ss] E [A | S] R)
  [boolean B | C] A E S R                                       -> (interp C [boolean B] E [A | S] R)
  [prim emptylist | C] [number 0] E S R                         -> (interp C [cons] E S R)
  [prim cn | C] A E [mark | S] R                                 -> (interp [prim cn | C] A E S R)
  [prim cn | C] mark E S R                                       -> (interp [prim cn | C] [cons] E S R)  
  [prim cn | C] [string A] E [[string A1] | S] R                -> (interp C [string ((function cn) A A1)] E S R)
  [prim cn | C] A E [A1 | S] R                                   -> (interp C [string ((function cn) (extract-kl A) (extract-kl A1))] E S R)
  [prim symbol? | C] [symbol _] E S R                           -> (interp C [boolean true] E S R)
  [prim symbol? | C] A E S R                                    -> (interp C [boolean false] E S R)
  [prim boolean? | C] [boolean _] E S R                         -> (interp C [boolean true] E S R)
  [prim boolean? | C] A E S R                                   -> (interp C [boolean false] E S R)
  [prim stream? | C] [stream in _] E S R                        -> (interp C [boolean true] E S R)
  [prim stream? | C] [stream out _] E S R                       -> (interp C [boolean true] E S R)
  [prim stream? | C] A E S R                                    -> (interp C [boolean false] E S R)
  [prim get-time | C] [symbol A] E S R                          -> (interp C [number ((function get-time) A)] E S R)
  [prim eval-kl | C] A E S R                                    -> (interp C (toplevel-interp (kl->zinc (extract-kl A))) E S R)
  [prim close | C] [stream in A] E S R                          -> (interp C (do ((function close) A) [cons]) E S R)
  [prim close | C] [stream out A] E S R                         -> (interp C (do ((function close) A) [cons]) E S R)
  [prim read-byte | C] [stream in A] E S R                      -> (interp C [number ((function read-byte) A)] E S R)
  [prim tl | C] [cons _ A] E S R                                -> (interp C A E S R)
  [prim hd | C] [cons A _] E S R                                -> (interp C A E S R)
  [prim cons? | C] [cons _ _] E S R                             -> (interp C [boolean true] E S R)
  [prim cons? | C] [cons] E S R                                 -> (interp C [boolean true] E S R)
  [prim cons? | C] A E S R                                      -> (interp C [boolean false] E S R)
  [prim absvector | C] [number A] E S R                         -> (interp C [absvector ((function absvector) A)] E S R)
  [prim absvector? | C] [absvector _] E S R                     -> (interp C [boolean true] E S R)
  [prim absvector? | C] A E S R                                 -> (interp C [boolean false] E S R)
  [prim n->string | C] [number A] E S R                         -> (interp C [string ((function n->string) A)] E S R)
  [prim string->n | C] [string A] E S R                         -> (interp C [number ((function string->n) A)] E S R)
  [prim str | C] [symbol A] E S R                               -> (interp C [string ((function str) A)] E S R)
  [prim str | C] [number A] E S R                               -> (interp C [string ((function str) A)] E S R)
  [prim str | C] [string A] E S R                               -> (interp C [string A] E S R)
  [prim str | C] [boolean true] E S R                           -> (interp C [string "true"] E S R)
  [prim str | C] [boolean false] E S R                          -> (interp C [string "false"] E S R)
  [prim str | C] A E S R                                        -> (interp C [string ((function str) A)] E S R)
  [prim tlstr | C] [string A] E S R                             -> (interp C [string ((function tlstr) A)] E S R)
  [prim hdstr | C] [string A] E S R                             -> (interp C [string ((function hdstr) A)] E S R)
  [prim read-file-as-string | C] [string A] E S R               -> (interp C [string ((function read-file-as-string) A)] E S R)
  [prim string? | C] [string _] E S R                           -> (interp C [boolean true] E S R)
  [prim string? | C] A E S R                                    -> (interp C [boolean false] E S R)
  [prim number? | C] [number _] E S R                           -> (interp C [boolean true] E S R)
  [prim number? | C] A E S R                                    -> (interp C [boolean false] E S R)
  [prim value | C] [symbol A] E S R                             -> (interp C ((function value) A) E S R)
  [prim intern | C] [string A] E S R                            -> (interp C [symbol ((function intern) A)] E S R)
  [prim error-to-string | C] [error A] E S R                    -> (interp C [string ((function error-to-string) A)] E S R)
  [prim simple-error | C] [string A] E S R                      -> (simple-error A)
  [prim trap-error | C] [lambda C1 E1] E S R                 -> (interp C (trap-error (interp C1 [lambda C1 E1] E1 S R) (/. Err [error Err])) E S R)
  [prim = | C] A E [A1 | S] R                                   -> (interp C [boolean ((function =) A A1)] E S R)
  [prim open | C] [string A] E [[symbol in] | S] R              -> (interp C [stream in ((function open) A in)] E S R)
  [prim open | C] [string A] E [[symbol out] | S] R             -> (interp C [stream out ((function open) A out)] E S R)
  [prim write-byte | C] [number A] E [[stream out A1] | S] R    -> (interp C [number ((function write-byte) A A1)] E S R)
  [prim cons | C] A E [A1 | S] R                                -> (interp C [cons A A1] E S R)
  [prim @p | C] A E [A1 | S] R                                  -> (interp C [cons A A1] E S R)
  [prim fst | C] [cons A _] E S R                               -> (interp C A E S R)
  [prim snd | C] [cons _ A] E S R                               -> (interp C A E S R)
  [prim gensym | C] [symbol A] E S R                            -> (interp C [symbol (gensym A)] E S R)
  [prim variable? | C] [symbol A] E S R                         -> (interp C [boolean (variable? A)] E S R)
  [prim variable? | C] A E S R                                  -> (interp C [boolean false] E S R)
  [prim <-address | C] [absvector A] E [[number A1] | S] R      -> (interp C ((function <-address) A A1) E S R)

  [prim pos | C] [string A] E [[number A1] | S] R               -> (interp C [string ((function pos) A A1)] E S R)
  [prim <= | C] [number A] E [[number A1] | S] R                -> (interp C [boolean ((function <=) A A1)] E S R)
  [prim >= | C] [number A] E [[number A1] | S] R                -> (interp C [boolean ((function >=) A A1)] E S R)
  [prim > | C] [number A] E [[number A1] | S] R                 -> (interp C [boolean ((function >) A A1)] E S R)
  [prim < | C] [number A] E [[number A1] | S] R                 -> (interp C [boolean ((function <) A A1)] E S R)
  [prim set | C] [symbol A] E [A1 | S] R                        -> (interp C ((function set) A A1) E S R)
  [prim error? | C] [error A] E S R                             -> (interp C [boolean true] E S R)
  [prim error? | C] A E S R                                     -> (interp C [boolean false] E S R)
  [prim function? | C] [lambda _ _] E S R                      -> (interp C [boolean true] E S R)
  [prim function? | C] A E S R                                  -> (interp C [boolean false] E S R)
  [prim - | C] [number A] E [[number A1] | S] R                 -> (interp C [number ((function -) A A1)] E S R)
  [prim * | C] [number A] E [[number A1] | S] R                 -> (interp C [number ((function *) A A1)] E S R)
  [prim / | C] [number A] E [[number A1] | S] R                 -> (interp C [number ((function /) A A1)] E S R)
  [prim + | C] [number A] E [[number A1] | S] R                 -> (interp C [number ((function +) A A1)] E S R)
  [prim address-> | C] [absvector A] E [[number A1] A2 | S] R   -> (interp C [absvector ((function address->) A A1 A2)] E S R)
  [] A E S R                                                    -> A
  [prim P | _] _ _ _ _                                          -> (simple-error (cn "interp: unknown prim - " (str P)))
  [Op | _] _ _ _ _                                              -> (simple-error (str Op))
  _ _ _ _ _                                                     -> (simple-error "interp: unknown expression"))

(define toplevel-interp { zinc-code --> zinc-value }
  X -> (interp X [cons] [] [] []))

(define kl->zinc { klambda --> zinc-code }
  \* Primitive call: zinc-c handles primitive? check directly.
     Bypass normalize/debruijn to avoid CPS closure capture bugs
     in the self-compiled normalize-term chain. *\
  [F | Args] -> (zinc-c [F | Args]) where (primitive? F)
  \* General case: full normalize/debruijn pipeline *\
  X -> (zinc-c (debruijn [] (normalize-term (kmacros X)))))

(define set-toplevel { symbol --> symbol --> symbol }
  N X -> (do
    (set global-table (cons [N (toplevel-interp (zinc-c (debruijn [] (normalize-term (kmacros (defun->lambda (ps X)))))))] (value global-table)))
    N))

(optimise +)

(tc -)
(load "shen/primitives.shen")
(tc +)

(set-toplevel number? safe.number?)
(set-toplevel symbol? safe.symbol?)
(set-toplevel string? safe.string?)
(set-toplevel boolean? safe.boolean?)
(set-toplevel cons? safe.cons?)
(set-toplevel simple-error safe.simple-error)
(set-toplevel get-time safe.get-time)
(set-toplevel close safe.close)
(set-toplevel read-byte safe.read-byte)
(set-toplevel tl safe.tl)
(set-toplevel hd safe.hd)
(set-toplevel absvector safe.absvector)
(set-toplevel n->string safe.n->string)
(set-toplevel string->n safe.string->n)
(set-toplevel str safe.str)
(set-toplevel tlstr safe.tlstr)
(set-toplevel value safe.value)
(set-toplevel intern safe.intern)
(set-toplevel error-to-string safe.error-to-string)
(set-toplevel trap-error safe.trap-error)
(set-toplevel = safe.=)
(set-toplevel open safe.open)
(set-toplevel write-byte safe.write-byte)
(set-toplevel cons safe.cons)
(set-toplevel fst safe.fst)
(set-toplevel snd safe.snd)
(set-toplevel emptylist safe.emptylist)
(set-toplevel hdstr safe.hdstr)
(set-toplevel read-file-as-string safe.read-file-as-string)
(set-toplevel <-address safe.<-address)
(set-toplevel cn safe.cn)
(set-toplevel pos safe.pos)
(set-toplevel <= safe.<=)
(set-toplevel >= safe.>=)
(set-toplevel < safe.<)
(set-toplevel > safe.>)
(set-toplevel set safe.set)
(set-toplevel - safe.-)
(set-toplevel * safe.*)
(set-toplevel / safe./)
(set-toplevel + safe.+)
(set-toplevel address-> safe.address->)
(set-toplevel eval-kl safe.eval-kl)
(set-toplevel extract-kl extract-kl)
(set-toplevel kl->zinc kl->zinc)
(set-toplevel toplevel-interp toplevel-interp)

\* Bundle compiler dependencies: kl->zinc calls zinc-c → zinc-t, map-zinc-c,
  debruijn → map-debruijn, normalize-term → normalize → normalize-name →
  normalize-names → flatten-%%app, kmacros → map-kmacros, plus
  atomic?, primitive?, fold-append, intersperse.  Without these in
  global-table, bundled closures that use them will fail at runtime.
  
  id must be bundled BEFORE normalize-term — normalize-term's source
  contains (function id), and when set-toplevel executes the compiled
  bytecode via toplevel-interp → interp, interp resolves global id
  via lookup-global which checks global-table. *\
(tc -)
(set-toplevel id id)
(set-toplevel zinc-c zinc-c)
(set-toplevel zinc-t zinc-t)
(set-toplevel map-zinc-c map-zinc-c)
(set-toplevel kmacros kmacros)
(set-toplevel map-kmacros map-kmacros)
(set-toplevel normalize-term normalize-term)
(set-toplevel normalize normalize)
(set-toplevel normalize-name normalize-name)
(set-toplevel normalize-names normalize-names)
(set-toplevel flatten-%%app flatten-%%app)
(set-toplevel atomic? atomic?)
(set-toplevel debruijn debruijn)
(set-toplevel map-debruijn map-debruijn)
(set-toplevel intersperse intersperse)
(set-toplevel fold-append fold-append)
(set-toplevel primitive? primitive?)
(set-toplevel instruction-keyword? instruction-keyword?)

\* Bundle the meta-circular interpreter and its helpers.
   Without interp in global-table, toplevel-interp's bytecode
   falls through to val_prim("interp") — hence the "[prim interp]"
   result from eval-kl.  lookup-global, lookup, and interp-jmp
   are transitive dependencies; interp's 97 rules call them via
   global lookups at runtime. *\
(set-toplevel lookup-global lookup-global)
(set-toplevel lookup lookup)
(set-toplevel interp-jmp interp-jmp)
(set-toplevel collect-apply-args collect-apply-args)
(set-toplevel interp interp)
(tc +)

\* Load eval/load infrastructure into the host for serialization *\
(tc -)
(load "shen/toplevel.shen")
(load "shen/load.shen")
(tc +)

