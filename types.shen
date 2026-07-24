(define primitive? { symbol --> boolean }
  X -> (element? X [+ / * - trap-error simple-error error-to-string intern
                    set value number? > < >= <= string? pos tlstr cn str
                    string->n n->string absvector address-> <-address emptylist
                    absvector? cons? cons hd tl write-byte read-byte open function?
                    close = eval-kl get-time symbol? boolean? error? stream?]))

(datatype primitive
  X : primitive >> T : Y;
  ___
  (if (primitive? X) T F) : Y;

  if (primitive? X)
  X : symbol;
  ________________________
  X : primitive;)

(datatype klambda
  \* all data is valid klambda *\
  ____________
  X : klambda;

  X : klambda;
  ___
  (primitive? X) : verified >> X : klambda;

  X : klambda;
  ___
  (string? X) : verified >> X : string;

  X : klambda;
  ___
  (number? X) : verified >> X : number;

  X : klambda;
  ___
  (boolean? X) : verified >> X : boolean;

  X : klambda;
  ___
  (symbol? X) : verified >> X : symbol;)

(datatype zinc-value
  X : symbol;
  ___
  (lookup-global X) : zinc-value;

  X : symbol;
  ___
  (value X) : zinc-value;

  X : string;
  ___
  [symbol (intern X)] : zinc-value;

  X : zinc-code;
  Y : (list zinc-value);
  ___
  [lambda X Y] : zinc-value;

  ___
  mark : zinc-value;

  X : symbol;
  ___
  [symbol X] : zinc-value;

  X : number;
  ___
  [number X] : zinc-value;

  X : string;
  ___
  [string X] : zinc-value;

  X : boolean;
  ___
  [boolean X] : zinc-value;

  ___
  [cons] : zinc-value;

  X : zinc-value;
  Y : zinc-value;
  ___
  [cons X Y] : zinc-value;

  N : number;
  ___
  (absvector N) : absvector;

  A : absvector;
  A1 : number;
  ___
  (<-address A A1) : zinc-value;

  A : absvector;
  A1 : number;
  A2 : zinc-value;
  ___
  [absvector (address-> A A1 A2)] : zinc-value;

  X : absvector;
  ___
  [absvector X] : zinc-value;

  X : exception;
  ___
  [error X] : zinc-value;

  X : (stream out);
  ___
  [stream out X] : zinc-value;

  X : (stream in);
  ___
  [stream in X] : zinc-value;)

(define bytecode? { symbol --> boolean }
  X -> (element? X [grab let appterm apply
                    push pushmark return endlet]))

(datatype zinc-code
  ___
  (fail) : zinc-code;

  X : zinc-code;
  Y : (list zinc-code);
  ___
  (intersperse X Y) : (list zinc-code);

  X : (list zinc-code);
  ___
  (fold-append [] X) : zinc-code;

  X : zinc-code;
  Y : zinc-code;
  ___
  (append X Y) : zinc-code;

  if (bytecode? X)
  Y : zinc-code;
  X : symbol;
  ___
  [X | Y] : zinc-code;

  Y : zinc-code;
  ___
  [X | Y] : zinc-code;

  ___
  [] : zinc-code;

  Y : zinc-code;
  X : number;
  ___
  [access X | Y] : zinc-code;

  Y : zinc-code;
  X : symbol;
  ___
  [global X | Y] : zinc-code;

  Y : zinc-code;
  X : zinc-code;
  ___
  [cur X | Y] : zinc-code;

  Y : zinc-code;
  F : symbol;
  ___
  [jmpf F | Y] : zinc-code; 

  Y : zinc-code;
  F : symbol;
  ___
  [jmp F | Y] : zinc-code; 

  Y : zinc-code;
  F : symbol;
  ___
  [label F | Y] : zinc-code; 

  Y : zinc-code;
  F : boolean;
  ___
  [boolean F | Y] : zinc-code; 

  Y : zinc-code;
  F : string;
  ___
  [string F | Y] : zinc-code; 

  Y : zinc-code;
  F : number;
  ___
  [number F | Y] : zinc-code; 

  Y : zinc-code;
  F : primitive;
  ___
  [prim F | Y] : zinc-code; 

  Y : zinc-code;
  X : symbol;
  ___
  [symbol X | Y] : zinc-code;)
