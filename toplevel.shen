\* toplevel.shen — eval/load entry points for the meta-circular interpreter
\* Loaded by interp.shen at the end of bootstrap *\

(tc -)

\* Evaluate a KLambda expression through the interpreter.
   defun forms are compiled and stored in global-table.
   Other forms are skipped (they're either already handled or not KLambda). *\
(define interp-eval
  [defun Name Args Body] -> (do
    (set global-table
      (cons [Name (toplevel-interp (kl->zinc (defun->lambda [defun Name Args Body])))]
            (value global-table)))
    Name)
  X -> X)
