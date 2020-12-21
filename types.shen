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
  (get interp X) : zinc-value;

  X : symbol;
  ___
  (value X) : zinc-value;

  X : string;
  ___
  [symbol (intern X)] : zinc-value;

  X : zinc-code;
  Y : (list zinc-value);
  ===
  [lambda X Y] : zinc-value;

  ===
  mark : zinc-value;

  X : symbol;
  ===
  [symbol X] : zinc-value;

  X : number;
  ===
  [number X] : zinc-value;

  X : string;
  ===
  [string X] : zinc-value;

  X : boolean;
  ===
  [boolean X] : zinc-value;

  ===
  [cons] : zinc-value;

  X : zinc-value;
  Y : zinc-value;
  ===
  [cons X Y] : zinc-value;

  N : number;
  ___
  (absvector N) : absvector;

  X : absvector;
  ===
  [absvector X] : zinc-value;

  X : exception;
  ===
  [error X] : zinc-value;

  X : (stream out);
  ===
  [stream out X] : zinc-value;

  X : (stream in);
  ===
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
  ===
  [X | Y] : zinc-code;

  Y : zinc-code;
  ===
  [X | Y] : zinc-code;

  ===
  [] : zinc-code;

  Y : zinc-code;
  X : number;
  ===
  [access X | Y] : zinc-code;

  Y : zinc-code;
  X : symbol;
  ===
  [global X | Y] : zinc-code;

  Y : zinc-code;
  X : zinc-code;
  ===
  [cur X | Y] : zinc-code;

  Y : zinc-code;
  F : symbol;
  ===
  [jmpf F | Y] : zinc-code; 

  Y : zinc-code;
  F : symbol;
  ===
  [jmp F | Y] : zinc-code; 

  Y : zinc-code;
  F : symbol;
  ===
  [label F | Y] : zinc-code; 

  Y : zinc-code;
  F : boolean;
  ===
  [boolean F | Y] : zinc-code; 

  Y : zinc-code;
  F : string;
  ===
  [string F | Y] : zinc-code; 

  Y : zinc-code;
  F : number;
  ===
  [number F | Y] : zinc-code; 

  Y : zinc-code;
  F : primitive;
  ===
  [prim F | Y] : zinc-code; 

  Y : zinc-code;
  X : symbol;
  ===
  [symbol X | Y] : zinc-code;)
