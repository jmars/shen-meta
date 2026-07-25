\* load.shen — file loader for the meta-circular interpreter *\

(tc -)
(load "toplevel.shen")

(define interp-load
  File -> (interp-eval-all (read-file File)))

(define interp-eval-all
  [] -> loaded
  [E | Rest] -> (do (interp-eval E) (interp-eval-all Rest)))
