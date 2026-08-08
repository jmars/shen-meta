(tc -)
(load "shen/util.shen")
(load "shen/shen-kl-helpers.shen")

\* shen->kl.shen — top-level dispatcher for the Shen-source -> KLambda
   compiler front-end.

   All helper functions (pattern compiler, clause parser, body rewriter,
   etc.) are in shen-kl-helpers.shen which is loaded above.  This file
   defines only the top-level dispatcher.

   shen->kl MUST be the last definition so that loading this file
   does not affect the host compiler's processing of earlier forms. *\

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
