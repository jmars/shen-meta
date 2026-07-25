\* serialize.shen — compile safe wrappers, output csexp *\
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

(print (compile-fn safe.+))
