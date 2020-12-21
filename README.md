```
λ[jaye@hydra]: ~/repos/bta [master ≡ +0 ~2 -0 !]>  rlwrap shen-scheme

Shen, copyright (C) 2010-2015 Mark Tarver
www.shenlanguage.org, Shen 22.2
running under Scheme, implementation: chez-scheme
port 0.23 ported by Bruno Deferrari


(0-) (tc +)
true

(1+) (load "interp.shen")



id : (A --> A)
newvar : (--> symbol)
index_h : (A --> ((list A) --> (number --> number)))
index : (A --> ((list A) --> number))
intersperse : (A --> ((list A) --> (list A)))
fold-append : ((list A) --> ((list (list A)) --> (list A)))
primitive? : (symbol --> boolean)
run time: 0.008331075000000007 secs

typechecked in 1980 inferences
loaded : symbol

primitive? : (symbol --> boolean)
primitive#type : symbol
klambda#type : symbol
zinc-value#type : symbol
bytecode? : (symbol --> boolean)
zinc-code#type : symbol
run time: 0.19925938400000004 secs

typechecked in 2894 inferences
loaded : symbol
map-kmacros : ((list klambda) --> (list klambda))
kmacros : (klambda --> klambda)
atomic? : (A --> boolean)
normalize-term : (klambda --> klambda)
normalize : (klambda --> ((klambda --> klambda) --> klambda))
normalize-name : (klambda --> ((klambda --> klambda) --> klambda))
normalize-names : (klambda --> ((klambda --> klambda) --> klambda))
map-debruijn : ((list symbol) --> ((list klambda) --> (list klambda)))
debruijn : ((list symbol) --> (klambda --> klambda))
run time: 0.27253319600000003 secs

typechecked in 67863 inferences
loaded : symbol

id : (A --> A)
newvar : (--> symbol)
index_h : (A --> ((list A) --> (number --> number)))
index : (A --> ((list A) --> number))
intersperse : (A --> ((list A) --> (list A)))
fold-append : ((list A) --> ((list (list A)) --> (list A)))
primitive? : (symbol --> boolean)
run time: 0.010807946000000013 secs

typechecked in 69746 inferences
loaded : symbol

map-zinc-c : (klambda --> (list zinc-code))
zinc-t : (klambda --> zinc-code)
zinc-c : (klambda --> zinc-code)
run time: 0.01829968000000004 secs

typechecked in 97791 inferences
loaded : symbol
lookup : (number --> ((list zinc-value) --> zinc-value))
interp-jmp : (zinc-code --> (symbol --> zinc-code))
extract-kl : (zinc-value --> klambda)
interp : (zinc-code --> (zinc-value --> ((list zinc-value) --> ((list zinc-value) --> ((list zinc-value) --> zinc-value)))))
defun->lambda : (klambda --> klambda)
toplevel-interp : (zinc-code --> zinc-value)
kl->zinc : (klambda --> zinc-code)
set-toplevel : (symbol --> (symbol --> symbol))
true : boolean
false : boolean
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

run time: 0.012638937999999933 secs
loaded : symbol
number? : symbol
symbol? : symbol
string? : symbol
boolean? : symbol
cons? : symbol
simple-error : symbol
get-time : symbol
close : symbol
read-byte : symbol
tl : symbol
hd : symbol
absvector : symbol
n->string : symbol
string->n : symbol
str : symbol
tlstr : symbol
value : symbol
intern : symbol
error-to-string : symbol
trap-error : symbol
= : symbol
open : symbol
write-byte : symbol
cons : symbol
<-address : symbol
cn : symbol
pos : symbol
<= : symbol
>= : symbol
< : symbol
> : symbol
set : symbol
- : symbol
* : symbol
/ : symbol
+ : symbol
address-> : symbol
eval-kl : symbol
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
[lambda [grab access 1 return] []] : klambda
[lambda [access 0 return] []] : klambda
[lambda [boolean true return] []] : klambda
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
true : boolean
run time: 0.510607153 secs
loaded : symbol
```