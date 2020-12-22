(define make ->
  (let ShenCode (read-file "primitives.shen")
       KlCode (map (function make.make-kl-code) ShenCode)
       KlString (make.list->string KlCode)
       Write (write-to-file "primitives.kl" KlString)
  "primitives.kl"))

(define make.make-kl-code
  [define F | Rules] -> (shen.elim-def [define F | Rules])
  [defcc F | Rules] -> (shen.elim-def [defcc F | Rules])
  Code -> Code)

(define make.list->string
  [] -> ""
  \* shen.fail! prints as "...", needs to be handled separately *\
  [[defun fail | _] | Y] -> (@s "(defun fail () shen.fail!)" (make.list->string Y))
  [X | Y] -> (@s (make-string "~R~%~%" X) (make.list->string Y)))