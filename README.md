```
Shen, copyright (C) 2010-2015 Mark Tarver
www.shenlanguage.org, Shen 22.2
running under Lua 5.1, implementation: LuaJIT 2.1.0-beta3
port 0.1 ported by Jaye Marshall <marshall.jaye@gmail.com>


(0-) (load "interp.shen")
map-kmacros
kmacros
atomic?
normalize-term
normalize
normalize-name
normalize-names
map-debruijn
debruijn

run time: 2 secs
loaded
id
newvar
index_h
index
intersperse
fold-append
primitive?

run time: 0 secs
loaded
map-zinc-c
zinc-t
zinc-c

run time: 0 secs
loaded
lookup
interp-jmp
extract-kl
interp
defun->lambda
toplevel-interp
kl->zinc
set-toplevel
true
safe.number?
safe.symbol?
safe.string?
safe.boolean?
safe.cons?
safe.simple-error
safe.get-time
safe.close
safe.read-byte
safe.tl
safe.hd
safe.absvector
safe.n->string
safe.string->n
safe.str
safe.tlstr
safe.value
safe.intern
safe.error-to-string
safe.trap-error
safe.=
safe.open
safe.write-byte
safe.cons
safe.<-address
safe.cn
safe.pos
safe.<=
safe.>=
safe.<
safe.>
safe.set
safe.-
safe.*
safe./
safe.+
safe.address->
safe.eval-kl

run time: 1 secs
loaded
[lambda [access 0 prim number? let access 0 jmpf l520 boolean true jmp l521 label l520 boolean false label l521 return] []]
[lambda [access 0 prim symbol? let access 0 jmpf l523 boolean true jmp l524 label l523 boolean false label l524 return] []]
[lambda [access 0 prim string? let access 0 jmpf l526 boolean true jmp l527 label l526 boolean false label l527 return] []]
[lambda [access 0 prim boolean? let access 0 jmpf l529 boolean true jmp l530 label l529 boolean false label l530 return] []]
[lambda [access 0 prim cons? let access 0 jmpf l532 boolean true jmp l533 label l532 boolean false label l533 return] []]
[lambda [pushmark access 0 push global string? apply let access 0 jmpf l535 access 1 prim simple-error jmp l536 label l535 ... etc] []]
[lambda [pushmark access 0 push symbol unix push global = apply let access 0 jmpf l539 symbol unix prim get-time jmp ... etc] []]
[lambda [access 0 prim stream? let access 0 jmpf l544 access 1 prim close jmp l545 label l544 string "close: argument must be a stream" prim ... etc] []]
[lambda [access 0 prim stream? let access 0 jmpf l547 access 1 prim read-byte jmp l548 label l547 string "read-byte: argument must be a stream" prim ... etc] []]
[lambda [number 0 prim emptylist let pushmark access 1 push access 0 push global = apply let access 0 jmpf l552 ... etc] []]
[lambda [number 0 prim emptylist let pushmark access 1 push access 0 push global = apply let access 0 jmpf l559 ... etc] []]
[lambda [pushmark access 0 push global number? apply let access 0 jmpf l564 access 1 prim absvector jmp l565 label l564 ... etc] []]
[lambda [pushmark access 0 push global number? apply let access 0 jmpf l567 access 1 prim n->string jmp l568 label l567 ... etc] []]
[lambda [pushmark access 0 push global string? apply let access 0 jmpf l570 access 1 prim string->n jmp l571 label l570 ... etc] []]
[lambda [access 0 prim str return] []]
[lambda [pushmark access 0 push string "" push global = apply let access 0 jmpf l574 string "tlstr: empty string" prim simple-error jmp ... etc] []]
[lambda [pushmark access 0 push global symbol? apply let access 0 jmpf l579 access 1 prim value jmp l580 label l579 ... etc] []]
[lambda [pushmark access 0 push string "true" push global = apply let access 0 jmpf l584 boolean true jmp l585 label ... etc] []]
[lambda [access 0 prim error? let access 0 jmpf l591 access 1 prim error-to-string jmp l592 label l591 string "error-to-string: arg must be an error" prim ... etc] []]
[lambda [grab access 1 prim function? let access 0 jmpf l596 access 1 prim function? jmp l597 label l596 boolean false ... etc] []]
[lambda [grab access 0 push access 1 prim = return] []]
[lambda [grab pushmark access 0 push symbol in push global = apply let access 0 jmpf l606 pushmark access 2 push ... etc] []]
[lambda [grab pushmark access 1 push global number? apply let access 0 jmpf l616 access 1 prim stream? jmp l617 label ... etc] []]
[lambda [grab access 0 push access 1 prim cons return] []]
[lambda [grab pushmark access 1 push global absvector? apply let access 0 jmpf l622 pushmark access 1 push global number? apply ... etc] []]
[lambda [grab pushmark access 1 push global string? apply let access 0 jmpf l628 pushmark access 1 push global string? apply ... etc] []]
[lambda [grab pushmark access 1 push global string? apply let access 0 jmpf l634 pushmark access 1 push global number? apply ... etc] []]
[lambda [grab pushmark access 1 push global number? apply let access 0 jmpf l640 pushmark access 1 push global number? apply ... etc] []]
[lambda [grab pushmark access 1 push global number? apply let access 0 jmpf l646 pushmark access 1 push global number? apply ... etc] []]
[lambda [grab pushmark access 1 push global number? apply let access 0 jmpf l652 pushmark access 1 push global number? apply ... etc] []]
[lambda [grab pushmark access 1 push global number? apply let access 0 jmpf l658 pushmark access 1 push global number? apply ... etc] []]
[lambda [grab pushmark access 1 push global symbol? apply let access 0 jmpf l663 access 1 push access 2 prim set ... etc] []]
[lambda [grab pushmark access 1 push global number? apply let access 0 jmpf l667 pushmark access 1 push global number? apply ... etc] []]
[lambda [grab pushmark access 1 push global number? apply let access 0 jmpf l673 pushmark access 1 push global number? apply ... etc] []]
[lambda [grab pushmark access 1 push global number? apply let access 0 jmpf l679 pushmark access 1 push global number? apply ... etc] []]
[lambda [grab pushmark access 1 push global number? apply let access 0 jmpf l685 pushmark access 1 push global number? apply ... etc] []]
[lambda [grab grab pushmark access 2 push global absvector? apply let access 0 jmpf l691 pushmark access 2 push global number? ... etc] []]
[lambda [access 0 prim eval-kl return] []]
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
[lambda [grab access 1 return] []]
[lambda [access 0 return] []]
[lambda [boolean true return] []]
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true
true

run time: 7 secs
loaded

(1-) 
```