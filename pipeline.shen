\* pipeline.shen — compile Shen KLambda to csexp bytecode for the native C VM
\* Usage:
\*   shen-scheme script pipeline.shen > output.csexp
\*   ./zincvm output.csexp

\* Disable type checker for loading untyped Shen 22.2 code *\
(tc -)

(load "normalize.shen")
(load "zinc.shen")
(load "compile.shen")

(define compile-expr { klambda --> string }
  X -> (zinc->native (zinc-c (debruijn [] (normalize-term (kmacros X))))))

\* Output bytecode to stdout *\
(print (compile-expr [+ 1 2]))
