\* load.shen — file loader for the meta-circular interpreter *\

(tc -)
(load "toplevel.shen")

(define interp-load
  File -> (interp-eval-all (read-file File)))

(define interp-load-safe
  File -> (trap-error (interp-eval-all (read-file File))
                      (/. X X)))

(define interp-load-raw
  File -> (interp-eval-all (read-from-string (read-file-as-string File))))

(define interp-eval-all
  [] -> loaded
  [E | Rest] -> (do (interp-eval-safe E) (interp-eval-all Rest)))

(define interp-eval-safe
  E -> (trap-error (interp-eval E) (/. X X)))
