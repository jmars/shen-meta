\* serialize.shen — compile all safe wrappers to csexp bundle *\
(tc -)
(load "primitives.shen")
(load "normalize.shen")
(load "zinc.shen")
(load "compile.shen")

(define defun->lambda
  [defun Name [] Body]           -> [lambda (newvar) Body]
  [defun Name [Arg] Body]        -> [lambda Arg Body]
  [defun Name [Arg | Args] Body] -> [lambda Arg (defun->lambda [defun Name Args Body])]
  _                              -> (simple-error "defun->lambda: invalid arg"))

(define compile-fn
  Name -> (zinc->native (zinc-c (debruijn []
         (normalize-term (kmacros (defun->lambda (ps Name))))))))

(set safe-names
  [safe.+ safe.- safe.* safe./ safe.= safe.< safe.> safe.<= safe.>=
   safe.cn safe.cons safe.hd safe.tl safe.cons? safe.symbol?
   safe.boolean? safe.number? safe.string? safe.str safe.tlstr
   safe.n->string safe.string->n safe.pos safe.intern safe.value
   safe.set safe.eval-kl safe.absvector safe.<-address safe.address->
   safe.error-to-string safe.simple-error safe.trap-error
   safe.get-time safe.open safe.close safe.read-byte safe.write-byte])

(define entry-str N -> (cn (cn (cn (cn "(" (csexp-atom N)) " ")
                                (compile-fn N))
                           ")"))

(define entries-str
  [] -> ""
  [N | Rest] -> (cn (entry-str N) (entries-str Rest))
  _ -> "")

(print (cn (cn "(" (entries-str (value safe-names))) ")"))
