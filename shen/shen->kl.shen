(tc -)
(load "shen/util.shen")
(load "shen/shen-kl-helpers.shen")

\* shen->kl.shen — top-level dispatcher for the Shen-source -> KLambda
   compiler front-end.

   All helper functions (pattern compiler, clause parser, etc.) are in
   shen-kl-helpers.shen which is loaded above.  This file defines only
   the body rewriter and the dispatcher.

   shen->kl MUST be the last definition so that loading this file
   does not affect the host compiler's processing of earlier forms. *\

\* ===== Body rewriter (Units 1-2) ===== *\

(define shen->kl-body
  X -> (if (cons? X) (shen->kl-form X) X))

(define shen->kl-form
  [lambda Arg Body]   -> [lambda Arg (shen->kl-body Body)]
  [let V E B]         -> [let V (shen->kl-body E) (shen->kl-body B)]
  [protect X]         -> (shen->kl-body X)
  [/. Arg Body]       -> (shen->kl-shorthand Arg Body)
  X                   -> (shen->kl-list X))

(define shen->kl-list
  []     -> []
  [X | R] -> [(shen->kl-body X) | (shen->kl-list R)])

(define shen->kl-shorthand
  Arg Body -> (if (cons? Arg)
                  (shen->kl-shorthand-args Arg Body)
                  [lambda Arg (shen->kl-body Body)]))

(define shen->kl-shorthand-args
  [A]       Body -> [lambda A (shen->kl-body Body)]
  [A | Rest] Body -> [lambda A (shen->kl-shorthand-args Rest Body)])

\* shen->kl: compile one top-level form.  MUST be last definition
   (after this file is loaded, the host's shen.shen->kl is replaced). *\

(define shen->kl
  [define Name | Rules]     -> (compile-define Name Rules)
  [defun Name Args Body]    -> [defun Name Args (shen->kl-body Body)]
  [set S E]                 -> [set S (shen->kl-body E)]
  [tc _]                    -> shen.skip
  [load _]                  -> shen.skip
  [set-toplevel _ _]        -> shen.skip
  [optimise _]              -> shen.skip
  [datatype _ | _]          -> shen.skip
  X                         -> (shen->kl-body X))

(define shen->kl-forms
  []      -> []
  [X | R] -> [(shen->kl X) | (shen->kl-forms R)])
