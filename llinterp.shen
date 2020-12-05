\* Low level implementation *\

\* The QBE translator only understands memory blocks, with no tail-recursion,
   so we model the interpreter as a loop that updates registers, we can then compare output
   with the reference interpreter to validate correctness *\

(define code->buf-h
  [] Buf I      -> Buf
  [H | T] Buf I -> (do
    (address-> Buf I H)
    (code->buf-h T Buf (+ 1 I))))

(define code->buf
  Code -> (code->buf-h Code (absvector (length Code)) 0))

(define init_registers
  Code -> (let C (absvector 1)
          (let A (absvector 1)
          (let E (absvector 1)
          (let S (absvector 1)
          (let R (absvector 1)
          (do
            (address-> C 0 Code)
            (address-> A 0 (absvector 0))
            (address-> E 0 [])
            (address-> S 0 [])
            (address-> R 0 [])
            (let Regs (absvector 5)
            (do
              (address-> Regs 0 C)
              (address-> Regs 1 A)
              (address-> Regs 2 E)
              (address-> Regs 3 S)
              (address-> Regs 4 R))))))))))

(define llinterp
  (@v [[access N] | C] A E S R) -> 
)