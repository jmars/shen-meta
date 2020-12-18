(define primitive? { symbol --> boolean }
  X -> (element? X [+ / * - trap-error simple-error error-to-string intern
                    set value number? > < >= <= string? pos tlstr cn str
                    string->n n->string absvector address-> <-address emptylist
                    absvector? cons? cons hd tl write-byte read-byte open function?
                    close = eval-kl get-time type symbol? boolean? error? stream?]))

(datatype primitive-funcname
  if (primitive? X)
  X : symbol;
  ________________________
  X : primitive-funcname;)

(datatype zinc-code
  X : number;
  _______________________
  [access X] : zinc-code;

  X : symbol;
  _______________________
  [global X] : zinc-code;

  _________________
  grab : zinc-code;

  ________________
  let : zinc-code;

  X : symbol;
  _____________________
  [jmpf X] : zinc-code;

  X : symbol;
  ______________________
  [label X] : zinc-code;

  X : symbol;
  ____________________
  [jmp X] : zinc-code;
  
  X : boolean;
  ________________________
  [boolean X] : zinc-code;

  X : number;
  _______________________
  [number X] : zinc-code;

  X : string;
  _______________________
  [string X] : zinc-code;

  X : symbol;
  _______________________
  [symbol X] : zinc-code;

  ____________________
  appterm : zinc-code;

  __________________
  apply : zinc-code;

  _________________
  push : zinc-code;

  _____________________
  pushmark : zinc-code;

  X : (list zinc-code);
  ____________________
  [cur X] : zinc-code;

  ___________________
  return : zinc-code;

  ___________________
  endlet : zinc-code;

  P : primitive-funcname;
  _______________________
  [prim P] : zinc-code;)
