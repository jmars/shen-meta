(define primitive? { symbol --> boolean }
  X -> (element? X [+ / * - trap-error simple-error error-to-string intern
                    set value number? > < >= <= string? pos tlstr cn str
                    string->n n->string absvector address-> <-address
                    absvector? cons? cons hd tl write-byte read-byte open
                    close = eval-kl get-time type symbol?]))

(datatype primitive-funcname
  if (primitive? X)
  X : symbol;
  ___
  X : primitive-funcname;)

(datatype zinc-code
  X : number;
  ___
  [access X] : zinc-code;

  X : symbol;
  ___
  [global X] : zinc-code;

  ___
  grab : zinc-code;

  ___
  let : zinc-code;

  X : symbol;
  ___
  [jmpf X] : zinc-code;

  X : symbol;
  ___
  [label X] : zinc-code;

  X : symbol;
  ___
  [jmp X] : zinc-code;
  
  X : boolean;
  ___
  [boolean X] : zinc-code;

  X : number;
  ___
  [number X] : zinc-code;

  X : string;
  ___
  [string X] : zinc-code;

  X : symbol;
  ___
  [symbol X] : zinc-code;

  ___
  appterm : zinc-code;

  ___
  apply : zinc-code;

  ___
  push : zinc-code;

  ___
  pushmark : zinc-code;

  X : (list zinc-code);
  ___
  [cur X] : zinc-code;

  ___
  return : zinc-code;

  ___
  endlet : zinc-code;

  P : primitive-funcname;
  ___
  [prim P] : zinc-code;)